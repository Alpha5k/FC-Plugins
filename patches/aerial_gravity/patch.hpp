#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::aerial_gravity {

// Declares melee aerial gravity compensation for one launch role: Client (including player hosts) or Server (GOG).
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::aerial_gravity
