#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fc_plugins::update_scheduling {

inline constexpr std::size_t kMaximumTrackedUpdates = 32;

// Tracks outstanding sends for compatible clients alongside the game's two native ACK slots.
class PeerSendWindow final {
  public:
    // Clears pending sends when the full connection identity changes.
    void synchronize(std::uint64_t peer_identity) noexcept;

    // Clears map-relative turns and timestamps while retaining the connection identity.
    void reset_world() noexcept;

    // Expires leading sends and checks RTT-derived capacity; all time and RTT arguments are in seconds.
    // The caller must clear the window when its native clock or turn domain resets.
    bool can_send(float now, float average_rtt, float deviation_rtt, std::int32_t updates_per_second) noexcept;

    // Appends a turn and network-clock timestamp in seconds; a full ring leaves the send unrecorded.
    void record(std::uint32_t turn, float sent_at) noexcept;

    // Returns the timestamp in seconds for this exact pending turn, before cumulative retirement.
    std::optional<float> sent_at(std::uint32_t turn) const noexcept;

    // Removes leading sends through the acknowledged turn using modulo-2^32 ordering.
    // Pending turns and the ACK must span less than half the range; this does not establish packet delivery.
    void acknowledge(std::uint32_t turn) noexcept;

    std::uint64_t peer_identity() const noexcept;

    std::size_t count() const noexcept;

    // Converts RTT mean plus twice its deviation (seconds) to at least two updates; nonpositive rates or estimates use two.
    // The ceiling is 16 through 30 TPS, scales to 32 at 60 TPS, and stays at 32 above it.
    static std::size_t capacity(float average_rtt, float deviation_rtt, std::int32_t updates_per_second) noexcept;

  private:
    struct SentUpdate {
        std::uint32_t turn{};
        float sent_at{}; // Network-clock seconds in the caller's current native world.
    };

    // Removes leading sends whose response deadline has elapsed.
    void expire(float now, float timeout) noexcept;

    // Releases the oldest entry and advances the fixed ring.
    void pop_front() noexcept;

    std::array<SentUpdate, kMaximumTrackedUpdates> entries_{};
    std::uint8_t begin_{};
    std::uint8_t count_{};

    std::uint64_t peer_identity_{};
};

} // namespace fc_plugins::update_scheduling
