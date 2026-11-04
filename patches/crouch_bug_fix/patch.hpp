#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::crouch_bug_fix {

// Declares crouch preservation for nonlocal players on supported game launches, including player hosting.
fc::Patch definition();

} // namespace fc_plugins::crouch_bug_fix
