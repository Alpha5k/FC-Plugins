#pragma once

#include <fc/native.hpp>
#include <fc/plugin.hpp>

namespace fc_plugins::voice_diagnostics {

// Declares optional Steam/GOG voice tracing with VoiceChat as its dependency.
fc::Patch definition();

} // namespace fc_plugins::voice_diagnostics
