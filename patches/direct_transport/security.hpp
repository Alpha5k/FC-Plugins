#pragma once

#include "protocol.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fc_plugins::direct_transport {

using SessionKey = std::array<std::uint8_t, 16>;

// Separates security algorithms from the system random provider and deterministic test sources.
struct RandomSource {
    void *context{};
    bool (*fill)(void *context, std::uint8_t *output, std::size_t bytes) noexcept {};
};

// Returns the Windows cryptographic random provider used for connection IDs and session keys.
RandomSource system_random_source() noexcept;
// Fills caller-owned bytes through the selected provider; empty output succeeds without calling it.
[[nodiscard]] bool fill_random(const RandomSource &source, std::span<std::uint8_t> output) noexcept;
// Tries at most 128 candidates for a nonzero ID absent from live_ids; failure leaves output zero.
[[nodiscard]] bool generate_connection_id(const RandomSource &source, std::span<const std::uint32_t> live_ids,
                                          std::uint32_t &output) noexcept;

// Computes SipHash-2-4 for the supplied key and complete message.
std::uint64_t sip_hash24(std::span<const std::uint8_t, 16> key, std::span<const std::uint8_t> message) noexcept;
// Authenticates direction, header and payload with the header's tag field treated as zero.
// Returns zero unless serialized_header contains exactly the 24-byte Direct header.
std::uint64_t compute_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                         std::span<const std::uint8_t> serialized_header, std::span<const std::uint8_t> payload) noexcept;
// Checks all eight tag bytes against the tag for this direction and payload.
[[nodiscard]] bool verify_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                             std::span<const std::uint8_t> serialized_header,
                                             std::span<const std::uint8_t> payload) noexcept;
// Compares equal-size byte ranges without returning early at the first different byte.
bool constant_time_equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept;

enum class ReplayResult : std::uint8_t {
    AcceptedFirst,
    AcceptedNew,
    AcceptedReordered,
    Duplicate,
    Stale,
    InvalidJump,
};

// Accepts each sequence once within a 256-packet window, including reordered packets and counter wraparound.
class ReplayWindow final {
  public:
    static constexpr std::uint32_t kWidth = 256;

    // Advances for newer sequences, accepts unseen older sequences within the window, and rejects
    // duplicates and stale packets. A jump of exactly half the uint32 range has no defined order.
    [[nodiscard]] ReplayResult admit(std::uint32_t sequence) noexcept;
    // Clears the latest sequence and history before a new association uses the window.
    void reset() noexcept;

  private:
    static constexpr std::uint32_t kHalfSequenceRange = std::uint32_t{1} << 31;

    // Bit zero is the latest sequence; higher bits mark older sequences by their age.
    std::bitset<kWidth> seen_{};
    std::uint32_t frontier_{};
    bool initialized_{};
};

} // namespace fc_plugins::direct_transport
