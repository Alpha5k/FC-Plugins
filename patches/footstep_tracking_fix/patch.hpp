#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::footstep_tracking_fix {

// Declares the client patch that prevents false footsteps after brief interruptions in foot tracking.
fc::Patch definition();

} // namespace fc_plugins::footstep_tracking_fix
