#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::melee_timing {

// Declares melee queue and condition timing for the selected client or server launch role.
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::melee_timing
