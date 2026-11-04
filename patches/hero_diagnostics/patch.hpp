#pragma once

#include "api.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <cstdint>

namespace fc_plugins::hero_diagnostics {

struct Settings {
    CaptureMode capture{CaptureMode::Standard};
};

// Defines the client and dedicated server variants of the diagnostics patch.
fc::Patch definition();

} // namespace fc_plugins::hero_diagnostics
