#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::map_hang_fix {

// Declares an end-of-round network completion override for GOG dedicated servers.
fc::Patch definition();

} // namespace fc_plugins::map_hang_fix
