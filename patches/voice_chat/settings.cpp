#include "handler.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace fc_plugins::voice_chat {
namespace {

// The retail profile embeds tOnlineOpt at 0x638; the following offsets are relative to that object.
constexpr std::size_t kProfileOnlineOptionsOffset = 0x638;
constexpr std::size_t kVoiceEnabledOffset = 0x97;
constexpr std::size_t kVoiceRecordDeviceOffset = 0xC8;
// Sensitivity reuses the stock record-input byte, which the PC ApplySettings path leaves unused.
constexpr std::size_t kVoiceSensitivityOffset = 0xC9;
// This byte is inside the profile's 64-byte padding and checksum span. Mode 1 saves a push-to-talk choice;
// no input hook consumes it, so capture still uses voice activity. iVoiceMode separately selects network routing.
constexpr std::size_t kVoiceInputModeOffset = 0x8CC;
constexpr std::int32_t kMaximumVoiceInputMode = 1;

} // namespace

bool Handler::open_voice(fc::NativeFunction<GameVoiceOpen> original, float open_rate, bool open_devices) noexcept {
    if (const auto *profile = get_current_profile_.call().value_or(nullptr); profile != nullptr) {
        publish_profile_options(profile + kProfileOnlineOptionsOffset);
    }

    // Preserve the native rate-like float and device-open flag for a later microphone change.
    last_open_rate_.store(std::bit_cast<std::uint32_t>(open_rate), std::memory_order_relaxed);
    last_open_devices_.store(open_devices, std::memory_order_relaxed);

    const auto result = original.call(open_rate, open_devices);
    refresh_local_routing();
    const auto opened = result.value_or(false);
    report_voice_open(result);
    if (opened) {
        applied_capture_.store(selected_capture_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return opened;
}

void Handler::report_voice_open(const fc::Result<bool> &result) noexcept {
    const auto opened = result.value_or(false);
    const auto error = GetLastError();
    // Capture and playback report their own causes; identify a failed outer open only when neither explains it.
    if (opened) {
        reported_open_failures_.fetch_and(~VoiceOpenFailed, std::memory_order_relaxed);
        logger_.debug("Voice opened (microphone device {})", selected_capture_.load(std::memory_order_relaxed));
    } else if ((reported_open_failures_.load(std::memory_order_relaxed) & (CaptureOpenFailed | PlaybackOpenFailed)) == 0 &&
               (reported_open_failures_.fetch_or(VoiceOpenFailed, std::memory_order_relaxed) & VoiceOpenFailed) == 0) {
        if (!result) {
            logger_.error("Could not call native voice open: {}", result.error().message);
        } else {
            logger_.error("The game could not open voice");
        }
    }
    SetLastError(error);
}

void Handler::apply_online_options(fc::NativeFunction<OnlineOptionsMethod> original, void *options) noexcept {
    publish_profile_options(options);
    const auto voice_enabled = options == nullptr ? std::byte{0} : fc::read_field<std::byte>(options, kVoiceEnabledOffset);
    const auto selected = selected_capture_.load(std::memory_order_relaxed);
    const auto applied = applied_capture_.load(std::memory_order_relaxed);

    // A changed device closes and reopens an existing node with the last native open arguments.
    // Report the open result before applying native preferences.
    if (selected != applied && find_voice_node_.call().value_or(nullptr) != nullptr) {
        static_cast<void>(game_voice_close_.call());
        const auto open_rate = std::bit_cast<float>(last_open_rate_.load(std::memory_order_relaxed));
        const auto open_devices = last_open_devices_.load(std::memory_order_relaxed);
        const auto result = game_voice_open_.call(open_rate, open_devices);
        const auto opened = result.value_or(false);
        report_voice_open(result);
        if (opened) {
            applied_capture_.store(selected, std::memory_order_relaxed);
        }
    }

    // Apply volumes and node routing through the game's settings path before its caller computes the profile checksum.
    static_cast<void>(original.call(options));
    refresh_local_routing();
    if (options != nullptr && voice_enabled != std::byte{0} && fc::read_field<std::byte>(options, kVoiceEnabledOffset) == std::byte{0}) {
        // Stock clears the saved preference when a node or local player is not
        // ready. Preserve the user's choice so a later voice open can retry it.
        fc::write_field(options, kVoiceEnabledOffset, voice_enabled);
    }
}

void Handler::reset_online_options(fc::NativeFunction<OnlineOptionsMethod> original, void *options) noexcept {
    static_cast<void>(original.call(options));
    if (options == nullptr) {
        return;
    }

    fc::write_field(options, kVoiceEnabledOffset, std::byte{0});
    fc::write_field(options, kVoiceRecordDeviceOffset, static_cast<std::byte>(kDefaultCaptureDevice));
    // 0xFF distinguishes the default sensitivity from an explicit slider level.
    fc::write_field(options, kVoiceSensitivityOffset, std::byte{0xFF});
    fc::write_field(options, kVoiceInputModeOffset, std::byte{0});
}

void Handler::publish_profile_options(const void *options) noexcept {
    if (options == nullptr) {
        return;
    }

    const auto stored_device = fc::read_field<std::uint8_t>(options, kVoiceRecordDeviceOffset);
    const auto selected = capture_devices_.normalize(stored_device);
    selected_capture_.store(selected, std::memory_order_relaxed);
    const auto choice = (static_cast<std::uint32_t>(stored_device) << 8U) | selected;
    if (reported_capture_choice_.exchange(choice, std::memory_order_relaxed) != choice) {
        const auto error = GetLastError();
        if (stored_device != selected) {
            logger_.debug("Saved microphone device {} is absent from the startup device list; selected Windows default input ({})",
                          stored_device, selected);
        } else {
            for (const auto &device : capture_devices_.choices()) {
                if (device.native_device == selected) {
                    // DirectSound enumeration supplies ANSI names; convert only the log's copy to UTF-8.
                    constexpr auto kNameCapacity = CaptureDevices::Choice{}.name.size();
                    std::array<wchar_t, kNameCapacity> wide{};
                    std::array<char, 3 * kNameCapacity> utf8{};
                    const auto wide_count =
                        MultiByteToWideChar(CP_ACP, 0, device.name.data(), -1, wide.data(), static_cast<int>(wide.size()));
                    const auto utf8_count = wide_count > 1
                                                ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_count - 1,
                                                                      utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr)
                                                : 0;
                    if (utf8_count > 0) {
                        logger_.debug("Selected microphone device {}: {}", selected,
                                      std::string_view{utf8.data(), static_cast<std::size_t>(utf8_count)});
                    } else {
                        logger_.debug("Selected microphone device {}", selected);
                    }
                    break;
                }
            }
        }
        SetLastError(error);
    }

    // Out-of-range stored sensitivity, including the 0xFF reset sentinel, means midpoint 5.
    const auto stored = fc::read_field<std::uint8_t>(options, kVoiceSensitivityOffset);
    const auto sensitivity = stored <= kMaximumSensitivity ? static_cast<std::int32_t>(stored) : kDefaultSensitivity;
    vad_threshold_.store(sensitivity_threshold(sensitivity), std::memory_order_relaxed);

    set_local_routing(fc::read_field<std::byte>(options, kVoiceEnabledOffset) != std::byte{0});
}

namespace {

// Accepts only finite integer slider or native device values; Lua numbers are float32.
std::optional<std::int32_t> integer(fc::lua::Value value) {
    const auto number = value.as_number();
    if (!number || !std::isfinite(*number) || std::trunc(*number) != *number ||
        static_cast<double>(*number) < (std::numeric_limits<std::int32_t>::min)() ||
        static_cast<double>(*number) > (std::numeric_limits<std::int32_t>::max)()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*number);
}

// Forwards every original result after a wrapper has extended the native operation.
void forward(fc::lua::Frame &frame) {
    auto result = frame.original();
    if (result) {
        frame.return_values(result->values());
    } else {
        frame.error(result.error().message);
    }
}

// Accepts the numeric and numeric-string row forms supplied by native lobby scripts.
std::optional<std::int32_t> lobby_row(fc::lua::Value value) {
    if (const auto row = integer(value)) {
        return row;
    }
    if (const auto text = value.as_string()) {
        std::int32_t parsed{};
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), parsed);
        if (error == std::errc{} && end == text->data() + text->size()) {
            return parsed;
        }
    }
    return std::nullopt;
}

} // namespace

void Handler::get_online_options(fc::lua::Frame &frame) {
    auto result = frame.original();
    if (!result) {
        frame.error(result.error().message);
        return;
    }
    frame.return_values(result->values());
    if (result->size() != 1 || (*result)[0].type() != fc::lua::Type::Table) {
        return;
    }

    // Extend only the game's single options table; preserve every original return value.
    auto table = (*result)[0];
    auto device = kDefaultCaptureDevice;
    auto sensitivity = kDefaultSensitivity;
    auto input_mode = 0;
    auto enabled = false;
    if (const auto *profile = get_current_profile_.call().value_or(nullptr)) {
        const auto *options = profile + kProfileOnlineOptionsOffset;
        device = fc::read_field<std::uint8_t>(options, kVoiceRecordDeviceOffset);
        const auto stored = fc::read_field<std::uint8_t>(options, kVoiceSensitivityOffset);
        sensitivity = stored <= kMaximumSensitivity ? stored : kDefaultSensitivity;
        const auto mode = fc::read_field<std::uint8_t>(options, kVoiceInputModeOffset);
        input_mode = mode <= kMaximumVoiceInputMode ? mode : 0;
        enabled = fc::read_field<std::byte>(options, kVoiceEnabledOffset) != std::byte{0};
    }

    table.set("bVoiceEnable", frame.value(enabled));
    table.set("iVoiceRecordDevice", frame.value(capture_devices_.normalize(device)));
    table.set("iVoiceSensitivity", frame.value(sensitivity));
    table.set("iVoiceInputMode", frame.value(input_mode));

    // Authored dropdowns expect one-based rows containing a display name and the saved native index.
    auto devices = frame.table();
    std::size_t index = 1;
    for (const auto &choice : capture_devices_.choices()) {
        auto row = frame.table();
        row.set("name", frame.value(std::string_view{choice.name.data()}));
        row.set("value", frame.value(choice.native_device));
        devices.set(index++, row);
    }
    table.set("aVoiceRecordDevices", devices);
}

void Handler::set_online_options(fc::lua::Frame &frame) {
    auto *profile = get_current_profile_.call().value_or(nullptr);
    const auto table = frame.argument(0);
    if (profile && table.type() == fc::lua::Type::Table) {
        auto *options = profile + kProfileOnlineOptionsOffset;
        if (auto value = table.get("bVoiceEnable").as_bool()) {
            fc::write_field(options, kVoiceEnabledOffset, *value ? std::byte{1} : std::byte{0});
        }
        if (auto value = integer(table.get("iVoiceRecordDevice"))) {
            const auto device = static_cast<std::uint8_t>(std::clamp(*value, 0, 255));
            fc::write_field(options, kVoiceRecordDeviceOffset, static_cast<std::byte>(capture_devices_.normalize(device)));
        }
        if (auto value = integer(table.get("iVoiceSensitivity"))) {
            fc::write_field(options, kVoiceSensitivityOffset, static_cast<std::byte>(std::clamp(*value, 0, kMaximumSensitivity)));
        }
        if (auto value = integer(table.get("iVoiceInputMode"))) {
            fc::write_field(options, kVoiceInputModeOffset, static_cast<std::byte>(std::clamp(*value, 0, kMaximumVoiceInputMode)));
        }
    }

    // The native setter applies options, then updates the checksum over the custom bytes and saved volumes.
    forward(frame);
}

std::optional<std::uint8_t> Handler::lobby_player(fc::lua::Frame &frame) const {
    const auto index = lobby_row(frame.argument(0));
    if (!index) {
        return std::nullopt;
    }

    // A displayed lobby row is not a player ID; the game's helper resolves the active lobby/browser mapping.
    const auto player = get_lobby_player_index_.call(*index).value_or(-1);
    return player >= 0 && player < static_cast<std::int32_t>(kMaximumPlayers) ? std::optional{static_cast<std::uint8_t>(player)}
                                                                              : std::nullopt;
}

void Handler::voice_lobby_glyph(fc::lua::Frame &frame) {
    const auto player = lobby_player(frame);
    const auto row = diagnostics_enabled() ? lobby_row(frame.argument(0)).value_or(-1) : -1;
    auto observed = ui_observation(voice_diagnostics::UiQuery::Glyph, player.value_or(kInvalidPlayer));
    observed.context = frame.state().value;
    observed.row = row;
    observed.reason = voice_diagnostics::UiReason::Inactive;
    if (!player) {
        observed.reason = voice_diagnostics::UiReason::NoPlayer;
        observe_ui(observed);
        frame.return_values({frame.value(0)});
        return;
    }

    const auto local = local_player_.load(std::memory_order_acquire);
    const auto sending = *player == local;
    // Authored glyph values: 0 hidden/unavailable, 1 muted, 4 available speaker, 5 speaking.
    std::uint8_t status{};
    if (voice_active()) {
        const auto local_flags = local_flags_.load(std::memory_order_acquire);
        const auto flags = sending ? local_flags : remote_players_.get(*player);
        observed.local_flags = local_flags;
        observed.player_flags = flags;
        observed.reason = sending ? voice_diagnostics::UiReason::SendUnavailable : voice_diagnostics::UiReason::ReceiveUnavailable;

        if ((sending && can_send_voice(local_flags)) || (!sending && flags != 0 && can_receive_voice(local_flags))) {
            status = (flags & Speaking) != 0 ? 5 : 4;

            observed.reason = voice_diagnostics::UiReason::Complete;
            if (!sending) {
                const auto transport = get_voice_transport_.call();
                // The local listen mask controls remote glyphs. Speaking advertises send activity, not audible playback.
                bool unavailable{};
                if (!transport) {
                    unavailable = true;
                    observed.reason = voice_diagnostics::UiReason::TransportReadFailed;
                } else if (*transport == nullptr) {
                    unavailable = true;
                    observed.reason = voice_diagnostics::UiReason::TransportMissing;
                } else {
                    const auto receive = receive_enable_.call(*transport, local, *player);
                    observed.receive_mask = receive ? static_cast<std::uint8_t>(*receive) : 2;
                    if (!receive) {
                        unavailable = true;
                        observed.reason = voice_diagnostics::UiReason::MaskReadFailed;
                    } else if (!*receive) {
                        unavailable = true;
                        observed.reason = voice_diagnostics::UiReason::MaskDisabled;
                    } else if ((flags & (Talk | SendConnected)) != (Talk | SendConnected)) {
                        unavailable = true;
                        observed.reason = voice_diagnostics::UiReason::SendUnavailable;
                    }
                }
                if (unavailable) {
                    status = 1;
                }
            }
        }
    }
    // Preserve the actual glyph result, including brief speaking transitions.
    observed.result = status;
    observe_ui(observed);
    frame.return_values({frame.value(status)});
}

} // namespace fc_plugins::voice_chat
