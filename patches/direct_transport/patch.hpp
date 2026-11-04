#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::direct_transport {

// Declares transport for one launch role. Pass Client (including player hosts) or Server (GOG with GalaxyPeer).
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::direct_transport
