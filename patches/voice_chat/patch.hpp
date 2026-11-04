#pragma once

#include <fc/native.hpp>
#include <fc/plugin.hpp>

namespace fc_plugins::voice_chat {

// Declares Opus voice, native relay hooks and player-launch volume corrections; saved voice enablement defaults off.
fc::Patch definition();

} // namespace fc_plugins::voice_chat
