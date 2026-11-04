#pragma once

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::galaxy_buddies {

struct GalaxyError;

// Galaxy 1.112 passes both user IDs and lobby IDs as eight-byte values.
// This wrapper preserves their ABI; callers distinguish them by the operation.
struct GalaxyId {
    std::uint64_t value{};

    friend bool operator==(const GalaxyId &, const GalaxyId &) = default;
};

// This view begins at the Galaxy ID inside each NetGame player record. The trailing
// bytes preserve the 0x208-byte stride used to reach the next player's ID.
struct LobbyPlayer {
    GalaxyId id;
    std::array<std::byte, 0x200> remaining;
};

static_assert(sizeof(LobbyPlayer) == 0x208);

using GetFriends = void *(__cdecl *)();
using GetGalaxyError = GalaxyError *(__cdecl *)();
using GetGalaxyInstance = void *(__cdecl *)();
using RequestAcceptedBuddies = bool(__cdecl *)();
using AcceptedBuddyCount = std::uint32_t(__cdecl *)();
// The wrapper receives hidden result storage before its ordinal argument.
using AcceptedBuddyByIndex = GalaxyId *(__cdecl *)(GalaxyId * output, std::uint32_t index);
using AcceptedBuddyName = const char *(__cdecl *)(std::uint32_t index);
using IsAcceptedBuddy = bool(__cdecl *)(const GalaxyId *user);
using IsBuddyJoinable = bool(__cdecl *)(std::uint32_t index);
using BuddyConnection = const char *(__cdecl *)(std::uint32_t index);
using LocalDisplayName = const char *(__cdecl *)() noexcept;
using LobbyPlayerName = const char *(__cdecl *)(std::int32_t index) noexcept;
// The lobby object's display-name wrapper receives the sparse player slot in ECX.
using LobbyDisplayName = const char *(__fastcall *)(std::int32_t index) noexcept;
using LobbyPlayerIndex = std::int32_t(__fastcall *)(std::int32_t ordinal) noexcept;
using LobbyPlayerCount = std::int32_t(__cdecl *)();
// The retail buddy predicate retains Phantom's FriendBuddyList player-slot semantics.
// ECX carries the local slot, EDX the remote slot, and AL returns the answer used by HUD and voice.
using IsPlayerBuddy = bool(__fastcall *)(std::int32_t local_player, std::int32_t player) noexcept;
// Resolves a live player slot in ECX, checking the array bounds and stored slot identity.
using GetPlayer = void *(__fastcall *)(std::int32_t player) noexcept;
// Publishes the pending special-join flag after its eight-byte target has been copied.
using SetSpecialJoinPending = void(__cdecl *)(bool pending) noexcept;
// Builds the friends table and repaints from Begin/UpdateFriends on the calling Lua thread.
using PublishFriends = void(__cdecl *)() noexcept;
// The lobby-list callback forwards each returned Galaxy ID through this
// wrapper before the current native search can select it.
using AddDiscoveredLobby = void(__cdecl *)(const GalaxyId *lobby) noexcept;

// GameState update return point used to service caches after the native update and transition work.
using NativeUpdate = bool(__cdecl *)() noexcept;

// Collects game functions and memory views for one supported retail client.
// Runtime Galaxy virtual calls use separate interface assumptions in GameBridge.
struct Layout {
    // This executable requires the peer image so its optional broker operations can be selected.
    bool needs_peer;
    fc::FunctionLocation<NativeUpdate> native_update;
    fc::FunctionLocation<GetFriends> get_friends;
    fc::FunctionLocation<GetGalaxyError> get_error;
    fc::DataLocation<const GetGalaxyInstance> get_galaxy_instance;
    fc::FunctionLocation<RequestAcceptedBuddies> request_accepted;
    fc::FunctionLocation<AcceptedBuddyCount> accepted_count;
    fc::FunctionLocation<AcceptedBuddyByIndex> accepted_by_index;
    fc::FunctionLocation<AcceptedBuddyName> accepted_name;
    fc::FunctionLocation<IsAcceptedBuddy> is_accepted;
    fc::FunctionLocation<IsBuddyJoinable> is_joinable;
    fc::FunctionLocation<BuddyConnection> buddy_connection;
    fc::FunctionLocation<LobbyPlayerCount> lobby_count;
    fc::FunctionLocation<LobbyPlayerIndex> lobby_index;
    fc::FunctionLocation<LobbyPlayerName> lobby_player_name;
    fc::FunctionLocation<LobbyDisplayName> lobby_display_name;
    fc::FunctionLocation<IsPlayerBuddy> is_player_buddy;
    fc::FunctionLocation<GetPlayer> get_player;
    // Color-only friendship calls: map uses local EAX / remote EDX slots;
    // names use local ESI / remote EDI players; voice uses frame -0x0C / EBX players.
    fc::CodeLocation map_buddy_color, name_buddy_color, voice_buddy_color;
    fc::FunctionLocation<SetSpecialJoinPending> set_special_join_pending;
    fc::FunctionLocation<PublishFriends> publish_friends;
    fc::FunctionLocation<AddDiscoveredLobby> add_lobby;
    // Shared decimal format used by buddy JOIN parsing and console player/hash formatting.
    fc::DataLocation<char, 5> lobby_id_format;
    fc::DataLocation<const LobbyPlayer, 64> lobby_players;
    // The game's listener sets this byte after accepted-list completion.
    fc::DataLocation<const std::uint8_t> accepted_ready;
    fc::DataLocation<const GalaxyId> requested_lobby;
    // BeginJoinSpecial copies these eight bytes before creating its native operation.
    // Steam's target is only four-byte aligned, so the view does not require GalaxyId alignment.
    fc::DataLocation<std::byte, sizeof(GalaxyId)> special_join_target;
    std::optional<fc::FunctionLocation<LocalDisplayName>> local_display_name;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::galaxy_buddies
