#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::inactive_window_fix {

// Declares the inactive-window yield correction for the selected client or server launch role.
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::inactive_window_fix
