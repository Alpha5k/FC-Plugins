#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::late_update_recovery {

// Defines ordered recovery of queued client updates for supported game builds.
fc::Patch definition();

} // namespace fc_plugins::late_update_recovery
