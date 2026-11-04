#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::host_random_state_fix {

// Declares the saved server random-state restoration for Steam and GOG client processes.
fc::Patch definition();

} // namespace fc_plugins::host_random_state_fix
