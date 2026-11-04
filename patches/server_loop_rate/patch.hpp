#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::server_loop_rate {

// Declares the server's 120 Hz outer loop cap for supported retail builds.
fc::Patch definition();

} // namespace fc_plugins::server_loop_rate
