#pragma once

#include <fc/base.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fc_plugins::multiplayer_gc {

inline constexpr std::size_t kCampaignCapacity = 512;
inline constexpr std::size_t kMissionCapacity = 256;

// Owns copies of the game's compressed campaign and mission data.
struct State {
    std::string campaign;
    std::string mission;

    bool operator==(const State &) const = default;
};

// Copies each buffer's recorded bytes; an empty campaign or a length exceeding storage returns an error.
fc::Result<State> capture_state(std::span<const char> campaign, std::uint32_t campaign_length, std::span<const char> mission,
                                std::uint32_t mission_length);

// Checks both buffers before restoring captured data and updating their lengths.
// Call on the Lua VM's owning thread with captured campaign and mission bytes.
fc::Result<> install_state(std::span<char> campaign, std::uint32_t &campaign_length, std::span<char> mission, std::uint32_t &mission_length,
                           const State &state);

} // namespace fc_plugins::multiplayer_gc
