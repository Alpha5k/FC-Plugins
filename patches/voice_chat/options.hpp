#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

struct _GUID;

namespace fc_plugins::voice_chat {

constexpr std::int32_t kDefaultSensitivity = 5;
constexpr std::int32_t kMaximumSensitivity = 10;
constexpr std::uint8_t kDefaultCaptureDevice = 0xFE;

// Maps levels 0 through 10 to mean-square PCM16 power; midpoint 5 uses the native threshold of 30,000.
std::uint32_t sensitivity_threshold(std::int32_t level) noexcept;

// Copies the startup DirectSound capture list for settings and native device-index selection.
// The game enumerates again on open; this list does not track hotplug or later index reordering.
class CaptureDevices final {
  public:
    struct Choice {
        // Truncated ANSI display name, terminated within patch-owned storage.
        std::array<char, 128> name{};
        // Native enumeration index, or 0xFE for Windows default input.
        std::uint8_t native_device{};
    };

    // Copies device names before native or Lua readers start; the default choice stays listed on failure.
    bool prepare() noexcept;

    std::int32_t enumeration_error() const noexcept {
        return enumeration_error_;
    }

    // Borrows the prepared choices, including the Windows default alias, until another prepare.
    std::span<const Choice> choices() const noexcept;

    // Finds a saved native index, falling back to the default input choice.
    std::uint8_t normalize(std::uint8_t native_device) const noexcept;

  private:
    // Receives DirectSound devices in the same order as the native capture source.
    static int __stdcall enumerate(_GUID *id, const char *description, const char *module, void *context) noexcept;

    // Copies one display name into bounded storage owned by the patch.
    void append(std::string_view name, std::uint8_t native_device) noexcept;

    static constexpr std::size_t kMaximumExplicitDevices = 254;
    static constexpr std::size_t kCapacity = 1 + kMaximumExplicitDevices;
    std::array<Choice, kCapacity> choices_{};
    std::size_t count_{};
    // Counts every native index, including the hidden null-GUID primary-driver alias.
    std::size_t enumerated_{};
    std::int32_t enumeration_error_{};
};

} // namespace fc_plugins::voice_chat
