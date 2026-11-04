#pragma once

#include "protocol.hpp"
#include <fc/native.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::lobby_player_list {

using LobbyPlayerCount = std::int32_t(__cdecl *)() noexcept;
using LobbyPlayerIndex = std::int32_t(__fastcall *)(std::int32_t) noexcept;
using LobbyPlayerName = const char *(__cdecl *)(std::int32_t) noexcept;

// Marks the native game-thread boundary used to copy mutable roster data.
using NativeUpdate = bool(__cdecl *)() noexcept;

// CNetFindGame::Update receives its live object in ECX and returns a bool in AL, with no stack arguments.
using BrowserUpdate = fc::NativeCall<bool(void *object) noexcept,
                                     fc::abi::X86<fc::abi::Args<fc::abi::Ecx>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

// Retail stores these adjacent x86 fields for Character::GetPtr and GetPlayerStats.
struct CharacterStorage {
    std::int32_t count{};
    std::uint32_t characters{};
    std::uint32_t stats{};
};

static_assert(sizeof(CharacterStorage) == 12);

inline constexpr std::size_t kPlayerTotalsStride = 0x60;
inline constexpr std::size_t kPlayerTotalsSize = (protocol::kMaximumPlayers - 1) * kPlayerTotalsStride + 8;
// Retail GetPlayerStats selects a record; GetPoints(true/false) sign-extends these two fields.
inline constexpr std::size_t kPlayerStatsStride = 0x1F8;
inline constexpr std::size_t kPlayerPointsOffset = 0x1F0;
inline constexpr std::size_t kHeroPointsOffset = 0x1F2;

// Names the qualified native roster accessors and browser type identity.
struct Layout {
    fc::FunctionLocation<NativeUpdate> native_update; // GameState update on the native application thread.
    fc::FunctionLocation<LobbyPlayerCount> player_count;
    fc::FunctionLocation<LobbyPlayerIndex> player_index; // Converts a lobby ordinal to a native player slot.
    fc::FunctionLocation<LobbyPlayerName> player_name;   // General NetGame accessor, also called outside lobby UI.
    fc::DataLocation<void *const> browser_operation;     // Current find/join object; its vtable distinguishes the browser.
    fc::DataLocation<const void *const> browser_vtable;  // Read-only native entries; the table address identifies the browser.
    fc::FunctionLocation<BrowserUpdate> browser_update;
    fc::CodeLocation browser_dirty_lookup; // Vtable getter reads the persistent sort/filter dirty byte at object+4.
    fc::DataLocation<const CharacterStorage> character_storage;
    fc::DataLocation<const std::byte, kPlayerTotalsSize> player_totals;
    fc::CodeLocation character_lookup;
    fc::CodeLocation stats_lookup;
    fc::CodeLocation points_lookup;
    fc::CodeLocation kills_lookup;
    fc::CodeLocation deaths_store;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::lobby_player_list
