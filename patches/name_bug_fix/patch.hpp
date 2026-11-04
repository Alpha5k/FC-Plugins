#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::name_bug_fix {

// Declares pending playing-list cleanup on GOG dedicated-server warmup departures and post-load timeouts.
fc::Patch definition();

} // namespace fc_plugins::name_bug_fix
