#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::distance_lag_fix {

// Declares the nearest-player move limit correction for GOG dedicated servers.
fc::Patch definition();

} // namespace fc_plugins::distance_lag_fix
