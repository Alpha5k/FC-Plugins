#include "association.hpp"

#include <Windows.h>

#include <bit>
#include <limits>

namespace fc_plugins::direct_transport {

bool TransmitGroup::begin(std::uint8_t slot, std::uint64_t generation, Carrier carrier) noexcept {
    if (depth_ == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }

    if (depth_ == 0) {
        // Only the outermost group selects a carrier; nested native sends inherit it.
        slot_ = slot;
        generation_ = generation;
        carrier_ = carrier;
    }

    ++depth_;
    return true;
}

bool TransmitGroup::end() noexcept {
    if (depth_ == 0) {
        return false;
    }
    if (--depth_ == 0) {
        slot_ = 0;
        generation_ = 0;
        carrier_ = Carrier::Galaxy;
    }
    return true;
}

bool TransmitGroup::active() const noexcept {
    return depth_ != 0;
}

bool TransmitGroup::belongs_to(std::uint8_t slot, std::uint64_t generation) const noexcept {
    return active() && slot_ == slot && generation_ == generation;
}

Carrier TransmitGroup::carrier() const noexcept {
    return carrier_;
}

bool NetworkThread::claim() noexcept {
    // The first successful compare-exchange keeps its thread ID across later game sessions.
    const auto current_thread = GetCurrentThreadId();
    auto owner = std::uint32_t{};
    if (owner_.compare_exchange_strong(owner, current_thread, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    return matches(current_thread, owner);
}

bool NetworkThread::current() noexcept {
    // Sends before the first receive may proceed without selecting the owner thread.
    const auto owner = owner_.load(std::memory_order_acquire);
    return owner == 0 || matches(GetCurrentThreadId(), owner);
}

std::uint64_t NetworkThread::rejected() const noexcept {
    return rejected_.load(std::memory_order_acquire);
}

bool NetworkThread::matches(std::uint32_t current_thread, std::uint32_t owner) noexcept {
    if (current_thread == owner) {
        return true;
    }
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool DiagnosticLogGate::admit(std::uint64_t count) noexcept {
    if (next_ == 0 || count < next_) {
        return false;
    }

    // Advance past the reported count; zero stops reporting before the threshold can overflow.
    next_ = count >= (std::numeric_limits<std::uint64_t>::max)() / 2 ? 0 : std::bit_ceil(count + 1);
    return true;
}

} // namespace fc_plugins::direct_transport
