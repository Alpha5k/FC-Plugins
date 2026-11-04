#pragma once

#include <fc/native.hpp>
#include <fc/plugin.hpp>

namespace fc_plugins::lobby_player_list {

// Declares host roster publication and server browser consumption on Steam and GOG.
fc::Patch definition();

} // namespace fc_plugins::lobby_player_list
