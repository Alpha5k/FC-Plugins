#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::sound_diagnostics {

// Declares optional shared mixer and DirectSound output tracing independently of voice chat.
fc::Patch definition();

} // namespace fc_plugins::sound_diagnostics
