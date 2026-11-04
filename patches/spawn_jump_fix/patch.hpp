#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::spawn_jump_fix {

// Declares restoration of the first spawn wait when warmup ends.
fc::Patch definition();

} // namespace fc_plugins::spawn_jump_fix
