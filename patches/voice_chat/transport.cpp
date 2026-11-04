#include "transport.hpp"

#include <algorithm>

namespace fc_plugins::voice_chat {
namespace {

constexpr std::array kMagic{
    std::byte{0xF9},
    std::byte{0x7D},
    std::byte{'F'},
    std::byte{'C'},
};
constexpr std::byte kVersion{0x01};
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kFlagsOffset = 5;
constexpr std::size_t kLengthOffset = 6;

// Rounds a carrier payload to the native codec block boundary.
constexpr std::size_t align_to_native_block(std::size_t size) noexcept {
    return ((size + kNativeBytes - 1) / kNativeBytes) * kNativeBytes;
}

// Recognizes normal frames and the two recovery modes used only for local decoding.
std::optional<FrameMode> parse_mode(std::byte value) noexcept {
    switch (std::to_integer<std::uint8_t>(value)) {
    case static_cast<std::uint8_t>(FrameMode::Normal):
        return FrameMode::Normal;
    case static_cast<std::uint8_t>(FrameMode::Fec):
        return FrameMode::Fec;
    case static_cast<std::uint8_t>(FrameMode::Plc):
        return FrameMode::Plc;
    default:
        return std::nullopt;
    }
}

// Validates framing and padding before exposing the encoded payload.
std::optional<FrameView> parse_frame(std::span<const std::byte> bytes, bool wire) noexcept {
    if (bytes.size() < kFrameHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) || bytes[kVersionOffset] != kVersion) {
        return std::nullopt;
    }

    const auto mode = parse_mode(bytes[kFlagsOffset]);
    if (!mode || (wire && *mode != FrameMode::Normal)) {
        return std::nullopt;
    }

    // The two-byte length is little-endian; PLC alone has no encoded payload.
    const auto length = static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[kLengthOffset])) |
                        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[kLengthOffset + 1])) << 8U);
    if (length > kMaximumPayloadBytes || (*mode == FrameMode::Plc) != (length == 0)) {
        return std::nullopt;
    }

    // Wire frames use the shortest 33-byte-aligned prefix; internal frames retain six zero-padded blocks.
    const auto meaningful_size = kFrameHeaderBytes + length;
    if (meaningful_size > bytes.size() ||
        (wire && (bytes.size() > kMaximumWireBytes || bytes.size() % kNativeBytes != 0 ||
                  bytes.size() != align_to_native_block(meaningful_size))) ||
        !std::all_of(bytes.begin() + meaningful_size, bytes.end(), [](std::byte value) {
            return value == std::byte{};
        })) {
        return std::nullopt;
    }

    return FrameView{
        .payload = bytes.subspan(kFrameHeaderBytes, length),
        .mode = *mode,
    };
}

} // namespace

bool build_frame(NativeFrame &frame, std::span<const std::byte> payload, FrameMode mode) noexcept {
    if (payload.size() > kMaximumPayloadBytes || (mode == FrameMode::Plc) != payload.empty()) {
        return false;
    }

    frame.fill(std::byte{});
    std::copy(kMagic.begin(), kMagic.end(), frame.begin());
    frame[kVersionOffset] = kVersion;
    frame[kFlagsOffset] = static_cast<std::byte>(mode);
    frame[kLengthOffset] = static_cast<std::byte>(payload.size() & 0xFFU);
    frame[kLengthOffset + 1] = static_cast<std::byte>((payload.size() >> 8U) & 0xFFU);
    std::copy(payload.begin(), payload.end(), frame.begin() + kFrameHeaderBytes);
    return true;
}

std::optional<FrameView> parse_native_frame(std::span<const std::byte, kNativeFrameBytes> frame) noexcept {
    return parse_frame(frame, false);
}

std::optional<FrameView> parse_wire_frame(std::span<const std::byte> bytes) noexcept {
    return parse_frame(bytes, true);
}

std::optional<std::size_t> wire_size(std::span<const std::byte, kNativeFrameBytes> frame) noexcept {
    const auto parsed = parse_native_frame(frame);
    if (!parsed || parsed->mode != FrameMode::Normal) {
        return std::nullopt;
    }
    return align_to_native_block(kFrameHeaderBytes + parsed->payload.size());
}

bool expand_wire_frame(std::span<const std::byte> bytes, NativeFrame &frame) noexcept {
    if (!parse_wire_frame(bytes)) {
        return false;
    }

    frame.fill(std::byte{});
    std::copy(bytes.begin(), bytes.end(), frame.begin());
    return true;
}

std::optional<NativeFrame> fec_frame(const NativeFrame &frame) noexcept {
    const auto parsed = parse_native_frame(frame);
    if (!parsed || parsed->mode != FrameMode::Normal) {
        return std::nullopt;
    }

    NativeFrame result{};
    if (!build_frame(result, parsed->payload, FrameMode::Fec)) {
        return std::nullopt;
    }
    return result;
}

NativeFrame plc_frame() noexcept {
    NativeFrame frame{};
    build_frame(frame, {}, FrameMode::Plc);
    return frame;
}

} // namespace fc_plugins::voice_chat
