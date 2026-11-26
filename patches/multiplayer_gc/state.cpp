#include "state.hpp"

#include <Windows.h>

#include <algorithm>

namespace fc_plugins::multiplayer_gc {
namespace {

// Checks the wire limits; mission setup may be absent before the first battle.
fc::Result<> validate_sizes(std::size_t campaign, std::size_t mission) {
    if (campaign == 0 || campaign > kCampaignCapacity) {
        return std::unexpected(fc::Error{"Campaign data must contain 1 to 512 bytes"});
    }
    if (mission > kMissionCapacity) {
        return std::unexpected(fc::Error{"Mission setup exceeds 256 bytes"});
    }
    return {};
}

} // namespace

fc::Result<std::span<char>> select_campaign_buffer(std::uintptr_t saved, std::uintptr_t loaded, std::span<char> stock) {
    if (!saved || saved != loaded) {
        return std::unexpected(fc::Error{"Metagame save and load do not share a campaign buffer."});
    }
    auto *buffer = reinterpret_cast<char *>(saved);
    if (buffer == stock.data()) {
        return stock;
    }

    // GameExt owns the relocated buffer. Expose only the prefix MPGC can put on the wire.
    const auto module = GetModuleHandleW(L"BF2GameExt.dll");
    MEMORY_BASIC_INFORMATION region{};
    if (!module || !VirtualQuery(buffer, &region, sizeof(region)) || region.AllocationBase != module ||
        region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_WRITECOPY)) {
        return std::unexpected(fc::Error{"The redirected campaign buffer is outside writable BF2GameExt storage."});
    }
    const auto offset = saved - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (offset > region.RegionSize || kCampaignCapacity > region.RegionSize - offset) {
        return std::unexpected(fc::Error{"The redirected campaign buffer is smaller than the MPGC campaign limit."});
    }
    return std::span<char>{buffer, kCampaignCapacity};
}

fc::Result<State> capture_state(std::span<const char> campaign, std::uint32_t campaign_length, std::span<const char> mission,
                                std::uint32_t mission_length) {
    if (auto valid = validate_sizes(campaign_length, mission_length); !valid) {
        return std::unexpected(valid.error());
    }
    if (campaign_length > campaign.size() || mission_length > mission.size()) {
        return std::unexpected(fc::Error{"Saved data exceeds the native storage range"});
    }
    return State{std::string(campaign.data(), campaign_length), std::string(mission.data(), mission_length)};
}

fc::Result<> install_state(std::span<char> campaign, std::uint32_t &campaign_length, std::span<char> mission, std::uint32_t &mission_length,
                           const State &state) {
    if (auto valid = validate_sizes(state.campaign.size(), state.mission.size()); !valid) {
        return valid;
    }
    if (state.campaign.size() > campaign.size() || state.mission.size() > mission.size()) {
        return std::unexpected(fc::Error{"Saved data exceeds the native storage range"});
    }

    // The native loaders use these lengths as their saved-state flags; publish them after both byte copies.
    std::ranges::copy(state.campaign, campaign.begin());
    std::ranges::copy(state.mission, mission.begin());
    campaign_length = static_cast<std::uint32_t>(state.campaign.size());
    mission_length = static_cast<std::uint32_t>(state.mission.size());
    return {};
}

} // namespace fc_plugins::multiplayer_gc
