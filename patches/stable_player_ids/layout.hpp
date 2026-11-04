#pragma once

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::stable_player_ids {

// The game MD5 function consumes a byte count and writes 32 lowercase hex characters plus a null.
using Md5 = void (*)(const char *input, std::uint32_t length, char *output) noexcept;
// The bootstrap imports take the embedded mutex address and return zero on success, using the compiler x64 ABI.
using MutexOperation = int (*)(void *mutex) noexcept;

// Restricts replacement hashes to the three admin callers of the shared native MD5 implementation.
struct GameLayout {
    fc::FunctionLocation<Md5> md5;
    std::array<fc::CallLocation<Md5>, 3> admin_hashes;
};

// Locates room publication and the RedNet owner whose client protects the Photon roster.
struct BootstrapLayout {
    fc::CodeLocation publish_user_id;
    fc::DataLocation<void *> rednet;
    fc::FunctionLocation<MutexOperation> lock;
    fc::FunctionLocation<MutexOperation> unlock;
};

// RedNet owns its client; the client owns Photon and an in-place native mutex.
constexpr std::ptrdiff_t kRedNetClientOffset = 0x10;
constexpr std::ptrdiff_t kPhotonClientOffset = 0x08;
constexpr std::ptrdiff_t kClientMutexOffset = 0x3F0;

// Photon stores the current room, whose embedded player vector has a count and pointer array.
constexpr std::ptrdiff_t kCurrentRoomOffset = 0xE0;
constexpr std::ptrdiff_t kPlayerCountOffset = 0xD8;
constexpr std::ptrdiff_t kPlayersOffset = 0xE8;

// Player embeds adjacent JStrings; lengths count UTF-16 code units and exclude the terminating null.
constexpr std::ptrdiff_t kPlayerNameOffset = 0x10;
constexpr std::ptrdiff_t kPlayerUserIdOffset = 0x28;
constexpr std::ptrdiff_t kStringDataOffset = 0x08;
constexpr std::ptrdiff_t kStringLengthOffset = 0x14;

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<GameLayout> select_game_layout(std::string_view profile);

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<BootstrapLayout> select_bootstrap_layout(std::string_view profile);

} // namespace fc_plugins::stable_player_ids
