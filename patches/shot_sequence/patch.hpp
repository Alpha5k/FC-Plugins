#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::shot_sequence {

// Declares cooldown completion correction and client projectile pattern preservation.
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::shot_sequence
