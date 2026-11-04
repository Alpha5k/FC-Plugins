#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace fc_plugins::voice_chat {

// Matches the game's voice packet object; send reads it synchronously and receive queues copy its payload.
struct AudioPacket {
    // Borrowed payload; receive queues own their copies.
    void *data{};
    // Payload length in bytes, independent of the native PCM sample count.
    std::uint32_t size{};
    // Native app-data pointer, preserved through packet views and used by the stream's release path.
    void *app_data{};
    // Native flag that flushes incomplete stream data at a discontinuity.
    std::uint8_t discontinuity{};
    std::array<std::byte, 3> padding{};
};

static_assert(sizeof(AudioPacket) == 0x10);
static_assert(offsetof(AudioPacket, discontinuity) == 0x0C);
static_assert(std::is_standard_layout_v<AudioPacket> && std::is_trivially_copyable_v<AudioPacket>);

// The GSM stream loops use 160 PCM16 samples and 33 encoded bytes per call.
// Six calls yield 960 samples (20 ms at 48 kHz) and a 198-byte internal frame.
inline constexpr std::size_t kNativeSamples = 160;
inline constexpr std::size_t kNativeBytes = 33;
inline constexpr std::size_t kBlocksPerFrame = 6;
inline constexpr std::size_t kFrameSamples = kNativeSamples * kBlocksPerFrame;
inline constexpr std::size_t kNativeFrameBytes = kNativeBytes * kBlocksPerFrame;

// The wire contains the 8-byte header and payload, padded to at most five native blocks.
inline constexpr std::size_t kFrameHeaderBytes = 8;
inline constexpr std::size_t kMaximumWireBytes = kNativeBytes * 5;
inline constexpr std::size_t kMaximumPayloadBytes = kMaximumWireBytes - kFrameHeaderBytes;

using NativeFrame = std::array<std::byte, kNativeFrameBytes>;

// Recovery modes use the same internal frame shape as normal packets, but they
// are created only after reception and never sent over the network.
enum class FrameMode : std::uint8_t {
    Normal = 0,
    // Ask Opus to recover the preceding lost frame from this payload's in-band FEC.
    Fec = 1,
    // Ask Opus to conceal loss without an encoded payload.
    Plc = 2,
};

// Borrows the payload from the validated frame; the source bytes must outlive this view.
struct FrameView {
    std::span<const std::byte> payload;
    FrameMode mode{};
};

// Writes the header, payload and zero padding into a 198-byte internal frame.
// Its aligned wire prefix can be sent without copying; PLC alone requires an empty payload.
bool build_frame(NativeFrame &frame, std::span<const std::byte> payload, FrameMode mode = FrameMode::Normal) noexcept;

// Validates a complete internal frame, including local FEC or concealment markers.
std::optional<FrameView> parse_native_frame(std::span<const std::byte, kNativeFrameBytes> frame) noexcept;

// Validates a complete aligned carrier payload and rejects local recovery modes.
std::optional<FrameView> parse_wire_frame(std::span<const std::byte> bytes) noexcept;

// Finds the aligned carrier prefix of a valid normal internal frame.
std::optional<std::size_t> wire_size(std::span<const std::byte, kNativeFrameBytes> frame) noexcept;

// Zero-pads a validated carrier prefix for the six native codec blocks.
bool expand_wire_frame(std::span<const std::byte> bytes, NativeFrame &frame) noexcept;

// Builds a local recovery frame from the next received normal frame.
std::optional<NativeFrame> fec_frame(const NativeFrame &frame) noexcept;

// Builds a local concealment request with no encoded payload.
NativeFrame plc_frame() noexcept;

// Inserts recovery before a validated, complete internal frame in one selected native queue.
// Enqueue copies borrowed packets before returning; its current-frame result is preserved.
template <class Enqueue> bool enqueue_recovered_audio(AudioPacket &current, std::uint16_t missing_packets, Enqueue &&enqueue) noexcept {
    constexpr std::uint16_t kMaximumRecoveredGap = 4;
    if (missing_packets > kMaximumRecoveredGap) {
        AudioPacket discontinuity{.app_data = current.app_data, .discontinuity = 1};
        enqueue(discontinuity);
    } else if (missing_packets != 0) {
        auto concealment = plc_frame();
        AudioPacket recovery{
            .data = concealment.data(), .size = static_cast<std::uint32_t>(concealment.size()), .app_data = current.app_data};
        for (auto missing = missing_packets; missing > 1; --missing) {
            enqueue(recovery);
        }

        const auto frame = std::span<const std::byte, kNativeFrameBytes>{static_cast<const std::byte *>(current.data), kNativeFrameBytes};
        NativeFrame received{};
        std::copy(frame.begin(), frame.end(), received.begin());
        auto fec = fec_frame(received);
        if (fec) {
            recovery.data = fec->data();
        }
        enqueue(recovery);
    }

    return enqueue(current);
}

} // namespace fc_plugins::voice_chat
