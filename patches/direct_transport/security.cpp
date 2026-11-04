#include "security.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

namespace fc_plugins::direct_transport {
namespace {

constexpr std::size_t kConnectionIdAttempts = 128;

// Reads one unaligned SipHash word from the supported little-endian host.
std::uint64_t load_little_endian64(const std::uint8_t *bytes) noexcept {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// Incremental input avoids copying the direction, header, and payload into a temporary packet.
class SipHasher final {
  public:
    // Initializes the four SipHash words from the two halves of the key.
    explicit SipHasher(std::span<const std::uint8_t, 16> key) noexcept {
        const auto first = load_little_endian64(key.data());
        const auto second = load_little_endian64(key.data() + 8);
        v0_ = 0x736f6d6570736575ULL ^ first;
        v1_ = 0x646f72616e646f6dULL ^ second;
        v2_ = 0x6c7967656e657261ULL ^ first;
        v3_ = 0x7465646279746573ULL ^ second;
    }

    // Compresses full words across input boundaries and retains the final partial word.
    void update(std::span<const std::uint8_t> bytes) noexcept {
        total_bytes_ += bytes.size();
        if (tail_bytes_ != 0) {
            // Finish a word split across input spans before processing complete words.
            while (!bytes.empty() && tail_bytes_ < tail_.size()) {
                tail_[tail_bytes_++] = bytes.front();
                bytes = bytes.subspan(1);
            }
            if (tail_bytes_ == tail_.size()) {
                compress(load_little_endian64(tail_.data()));
                tail_bytes_ = 0;
            }
        }

        while (bytes.size() >= 8) {
            compress(load_little_endian64(bytes.data()));
            bytes = bytes.subspan(8);
        }

        while (!bytes.empty()) {
            tail_[tail_bytes_++] = bytes.front();
            bytes = bytes.subspan(1);
        }
    }

    // Includes the byte count in the final word, then applies the SipHash-2-4 finalization rounds.
    std::uint64_t finish() noexcept {
        std::uint64_t final_block = static_cast<std::uint64_t>(total_bytes_) << 56;
        for (std::size_t index = 0; index < tail_bytes_; ++index) {
            final_block |= static_cast<std::uint64_t>(tail_[index]) << (index * 8);
        }

        v3_ ^= final_block;
        run_rounds(2);
        v0_ ^= final_block;
        v2_ ^= 0xFF;
        run_rounds(4);
        return v0_ ^ v1_ ^ v2_ ^ v3_;
    }

  private:
    // Applies one SipHash mixing round to the four state words.
    void round() noexcept {
        v0_ += v1_;
        v1_ = std::rotl(v1_, 13);
        v1_ ^= v0_;
        v0_ = std::rotl(v0_, 32);
        v2_ += v3_;
        v3_ = std::rotl(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = std::rotl(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = std::rotl(v1_, 17);
        v1_ ^= v2_;
        v2_ = std::rotl(v2_, 32);
    }

    // Applies the compression or finalization round count selected by the caller.
    void run_rounds(unsigned int count) noexcept {
        for (unsigned int index = 0; index < count; ++index) {
            round();
        }
    }

    // Absorbs one complete message word with two compression rounds.
    void compress(std::uint64_t word) noexcept {
        v3_ ^= word;
        run_rounds(2);
        v0_ ^= word;
    }

    std::uint64_t v0_{};
    std::uint64_t v1_{};
    std::uint64_t v2_{};
    std::uint64_t v3_{};
    std::array<std::uint8_t, 8> tail_{};
    std::size_t tail_bytes_{};
    std::size_t total_bytes_{};
};

// Requests cryptographic random bytes using the Windows system-preferred provider.
bool system_random(void *, std::uint8_t *output, std::size_t bytes) noexcept {
    if (output == nullptr || bytes > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    return BCryptGenRandom(nullptr, output, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

} // namespace

RandomSource system_random_source() noexcept {
    return {nullptr, &system_random};
}

bool fill_random(const RandomSource &source, std::span<std::uint8_t> output) noexcept {
    return output.empty() || (source.fill != nullptr && source.fill(source.context, output.data(), output.size()));
}

bool generate_connection_id(const RandomSource &source, std::span<const std::uint32_t> live_ids, std::uint32_t &output) noexcept {
    output = 0;
    // Random retries are bounded even if an injected source repeatedly returns zero or collisions.
    for (std::size_t attempt = 0; attempt < kConnectionIdAttempts; ++attempt) {
        std::array<std::uint8_t, 4> bytes{};
        if (!fill_random(source, bytes)) {
            return false;
        }
        const auto candidate = load_big_endian32(bytes.data());
        if (candidate != 0 && !std::ranges::contains(live_ids, candidate)) {
            output = candidate;
            return true;
        }
    }
    return false;
}

std::uint64_t sip_hash24(std::span<const std::uint8_t, 16> key, std::span<const std::uint8_t> message) noexcept {
    SipHasher hasher(key);
    hasher.update(message);
    return hasher.finish();
}

std::uint64_t compute_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                         std::span<const std::uint8_t> serialized_header, std::span<const std::uint8_t> payload) noexcept {
    if (serialized_header.size() != kDirectHeaderBytes) {
        return 0;
    }

    // Clear the transmitted tag before hashing. The direction byte prevents reflecting a valid
    // client datagram back to that client as a server datagram.
    std::array<std::uint8_t, kDirectHeaderBytes> authenticated_header{};
    std::ranges::copy(serialized_header, authenticated_header.begin());
    std::ranges::fill(std::span(authenticated_header).subspan(offsetof(DirectHeaderV1, authentication_tag)), std::uint8_t{});
    const std::array direction_domain{static_cast<std::uint8_t>(direction)};

    SipHasher hasher(key);
    hasher.update(direction_domain);
    hasher.update(authenticated_header);
    hasher.update(payload);
    return hasher.finish();
}

bool verify_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key, std::span<const std::uint8_t> serialized_header,
                               std::span<const std::uint8_t> payload) noexcept {
    if (serialized_header.size() != kDirectHeaderBytes) {
        return false;
    }

    std::array<std::uint8_t, 8> expected{};
    store_big_endian64(expected.data(), compute_authentication_tag(direction, key, serialized_header, payload));
    const auto received = serialized_header.subspan(offsetof(DirectHeaderV1, authentication_tag), expected.size());
    return constant_time_equal(expected, received);
}

bool constant_time_equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    std::uint8_t difference{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
    }
    return difference == 0;
}

ReplayResult ReplayWindow::admit(std::uint32_t sequence) noexcept {
    if (!initialized_) {
        initialized_ = true;
        frontier_ = sequence;
        seen_.reset();
        seen_.set(0);
        return ReplayResult::AcceptedFirst;
    }

    // Unsigned subtraction keeps the direction correct when the sequence number wraps to zero.
    const auto forward_distance = sequence - frontier_;
    if (forward_distance == 0) {
        return ReplayResult::Duplicate;
    }
    if (forward_distance == kHalfSequenceRange) {
        return ReplayResult::InvalidJump;
    }
    if (forward_distance < kHalfSequenceRange) {
        // Shift toward older positions; a distance of at least the width clears the history.
        seen_ <<= forward_distance;
        frontier_ = sequence;
        seen_.set(0);
        return ReplayResult::AcceptedNew;
    }

    // Older packets can arrive out of order, but each age can be admitted only once.
    const auto age = frontier_ - sequence;
    if (age >= kWidth) {
        return ReplayResult::Stale;
    }
    if (seen_.test(age)) {
        return ReplayResult::Duplicate;
    }

    seen_.set(age);
    return ReplayResult::AcceptedReordered;
}

void ReplayWindow::reset() noexcept {
    seen_.reset();
    frontier_ = 0;
    initialized_ = false;
}

} // namespace fc_plugins::direct_transport
