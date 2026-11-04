#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::collision_history_fix {

// Declares local collision replay with exact contact priority and unchanged skipped Soldiers.
fc::Patch definition();

} // namespace fc_plugins::collision_history_fix
