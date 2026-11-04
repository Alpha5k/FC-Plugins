#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::input_sync {

// Declares complete client input delivery, prediction history and received-input retirement.
fc::Patch definition(fc::Role role);

} // namespace fc_plugins::input_sync
