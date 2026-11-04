#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::speedpack_patch {

// Declares the dispenser strength correction before native launch-speed scaling.
fc::Patch definition();

} // namespace fc_plugins::speedpack_patch
