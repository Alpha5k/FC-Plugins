#pragma once

#include <fc/native.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::direct_transport::server {

// Reads the current playing mask when next_state is false, or the next playing mask when true.
using IsPlaying = bool(__cdecl *)(std::int32_t player, bool next_state) noexcept;
// May remap a guest slot to its primary owner; starts/advances the native two-second boot and writes -1 when finished.
using UpdateBootPlayer = void(__cdecl *)(std::int32_t *player) noexcept;
// Clears next-playing and guest masks, then disconnects this slot through the native connection path.
using SetNotPlaying = void(__cdecl *)(std::int32_t player) noexcept;
// Returns the guest's primary player, or -1 when this player has no primary owner.
using GetGuestOwner = std::int32_t(__cdecl *)(std::int32_t player) noexcept;
// Ends a native endpoint connection; ECX carries the slot and the caller owns the stack.
using Disconnect = fc::NativeCall<void(std::int32_t) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// Groups server-only resources not present in the shared client/server game layout.
struct ServerLayout {
    // Configured /gameport, before the native socket's possible retry on a different port.
    fc::DataLocation<std::uint32_t> game_port;
    fc::FunctionLocation<IsPlaying> is_playing;
    fc::FunctionLocation<UpdateBootPlayer> update_boot_player;
    fc::FunctionLocation<SetNotPlaying> set_not_playing;
    fc::FunctionLocation<GetGuestOwner> get_guest_owner;
    fc::FunctionLocation<Disconnect> disconnect;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ServerLayout> select_server_layout(std::string_view profile);

} // namespace fc_plugins::direct_transport::server
