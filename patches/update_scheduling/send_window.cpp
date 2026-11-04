#include "send_window.hpp"

#include <algorithm>
#include <cmath>

namespace fc_plugins::update_scheduling {
namespace {

constexpr float kMinimumResponseTime = 0.1F;
constexpr std::uint32_t kHalfTurnRange = std::uint32_t{1} << 31;

// Allows the RTT mean plus twice its deviation, with a minimum of one tenth of a second.
float response_time(float average_rtt, float deviation_rtt) noexcept {
    const auto estimate = average_rtt + 2.0F * deviation_rtt;
    return estimate > kMinimumResponseTime ? estimate : kMinimumResponseTime;
}

// Compares turns modulo 2^32; pending sends and ACKs must span less than half that range.
bool acknowledges(std::uint32_t acknowledged_turn, std::uint32_t sent_turn) noexcept {
    return acknowledged_turn - sent_turn < kHalfTurnRange;
}

} // namespace

void PeerSendWindow::synchronize(std::uint64_t peer_identity) noexcept {
    if (peer_identity_ == peer_identity) {
        return;
    }

    reset_world();
    peer_identity_ = peer_identity;
}

void PeerSendWindow::reset_world() noexcept {
    begin_ = 0;
    count_ = 0;
}

bool PeerSendWindow::can_send(float now, float average_rtt, float deviation_rtt, std::int32_t updates_per_second) noexcept {
    expire(now, response_time(average_rtt, deviation_rtt));

    return count_ < capacity(average_rtt, deviation_rtt, updates_per_second);
}

void PeerSendWindow::record(std::uint32_t turn, float sent_at) noexcept {
    if (count_ == entries_.size()) {
        return;
    }

    const auto index = (static_cast<std::size_t>(begin_) + count_) % entries_.size();
    entries_[index] = {.turn = turn, .sent_at = sent_at};
    ++count_;
}

std::optional<float> PeerSendWindow::sent_at(std::uint32_t turn) const noexcept {
    for (std::size_t offset = 0; offset < count_; ++offset) {
        const auto &entry = entries_[(static_cast<std::size_t>(begin_) + offset) % entries_.size()];
        if (entry.turn == turn) {
            return entry.sent_at;
        }
    }

    return std::nullopt;
}

void PeerSendWindow::acknowledge(std::uint32_t turn) noexcept {
    while (count_ != 0 && acknowledges(turn, entries_[begin_].turn)) {
        pop_front();
    }
}

std::uint64_t PeerSendWindow::peer_identity() const noexcept {
    return peer_identity_;
}

std::size_t PeerSendWindow::count() const noexcept {
    return count_;
}

std::size_t PeerSendWindow::capacity(float average_rtt, float deviation_rtt, std::int32_t updates_per_second) noexcept {
    if (updates_per_second <= 0) {
        return 2;
    }

    const auto estimate = average_rtt + 2.0F * deviation_rtt;
    if (!(estimate > 0.0F)) {
        return 2;
    }

    // Preserve the sixteen-update allowance at 30 TPS, scaling its ceiling through 60 TPS within the fixed ring.
    const auto ceiling = static_cast<std::size_t>((16 * std::clamp(updates_per_second, 30, 60) + 29) / 30);
    const auto desired = std::ceil(estimate * static_cast<float>(updates_per_second));
    if (!(desired > 2.0F)) {
        return 2;
    }

    if (desired >= static_cast<float>(ceiling)) {
        return ceiling;
    }

    return static_cast<std::size_t>(desired);
}

void PeerSendWindow::expire(float now, float timeout) noexcept {
    while (count_ != 0 && now - entries_[begin_].sent_at > timeout) {
        pop_front();
    }
}

void PeerSendWindow::pop_front() noexcept {
    begin_ = static_cast<std::uint8_t>((begin_ + 1) % entries_.size());
    --count_;
}

} // namespace fc_plugins::update_scheduling
