#include "options.hpp"

#define DIRECTSOUND_VERSION 0x0900
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <algorithm>
#include <cstdlib>

namespace fc_plugins::voice_chat {
namespace {

constexpr std::uint32_t kNativeEnergyThreshold = 30'000;

} // namespace

std::uint32_t sensitivity_threshold(std::int32_t level) noexcept {
    // Each step changes required power by a factor of two, about 3 dB; higher sensitivity lowers it.
    const auto bounded = std::clamp(level, 0, kMaximumSensitivity);
    const auto scale = 1U << static_cast<std::uint32_t>(std::abs(kDefaultSensitivity - bounded));
    if (bounded < kDefaultSensitivity) {
        return kNativeEnergyThreshold * scale;
    }

    // Round the divided power threshold to the nearest integer.
    return (kNativeEnergyThreshold + scale / 2U) / scale;
}

bool CaptureDevices::prepare() noexcept {
    count_ = 0;
    enumerated_ = 0;
    append("Default Input Device", kDefaultCaptureDevice);
    enumeration_error_ = DirectSoundCaptureEnumerateA(enumerate, this);
    return enumeration_error_ == DS_OK;
}

std::span<const CaptureDevices::Choice> CaptureDevices::choices() const noexcept {
    return std::span{choices_}.first(count_);
}

std::uint8_t CaptureDevices::normalize(std::uint8_t native_device) const noexcept {
    for (const auto &choice : choices()) {
        if (choice.native_device == native_device) {
            return native_device;
        }
    }
    return kDefaultCaptureDevice;
}

int __stdcall CaptureDevices::enumerate(_GUID *id, const char *description, const char *, void *context) noexcept {
    auto *devices = static_cast<CaptureDevices *>(context);
    if (devices == nullptr || devices->enumerated_ >= kMaximumExplicitDevices) {
        return FALSE;
    }

    const auto native_device = static_cast<std::uint8_t>(devices->enumerated_++);
    // The null GUID is DirectSound's primary-driver alias. The explicit native
    // index still advances, but the default choice already represents it.
    if (id != nullptr) {
        devices->append(description == nullptr ? "DirectSound Input Device" : description, native_device);
    }
    return TRUE;
}

void CaptureDevices::append(std::string_view name, std::uint8_t native_device) noexcept {
    if (count_ >= choices_.size()) {
        return;
    }

    auto &choice = choices_[count_++];
    auto &output = choice.name;
    const auto copied = std::min(name.size(), output.size() - 1);
    std::copy_n(name.begin(), copied, output.begin());
    output[copied] = '\0';
    choice.native_device = native_device;
}

} // namespace fc_plugins::voice_chat
