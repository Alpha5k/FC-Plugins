#pragma once

#include <fc/plugin.hpp>
#include <fc/native.hpp>

namespace fc_plugins::network_diagnostics {

// Standard records identity, network progress, acceptance and readiness. Combat adds controls, combat and poses.
// Full adds packet, primitive codec, storage and detailed frame timing observations.
enum class CaptureMode {
    Standard = 0,
    Combat = 1,
    Full = 2
};

struct Settings {
    CaptureMode capture{CaptureMode::Standard};
};

// Defines the passive client and dedicated server observations, disabled by default.
fc::Patch definition();

} // namespace fc_plugins::network_diagnostics
