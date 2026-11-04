#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::reload_duration_fix {

// Declares reload-duration repair for weapons owned by locally controlled players on joining clients.
fc::Patch definition();

} // namespace fc_plugins::reload_duration_fix
