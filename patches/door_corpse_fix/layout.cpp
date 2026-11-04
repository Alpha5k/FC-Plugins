#include "layout.hpp"

#include <array>
#include <string>

namespace fc_plugins::door_corpse_fix {
namespace {

// RVA of the object query; supported images share the door sites and soldier getter.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t object_query;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .object_query = 0x001CF820},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .object_query = 0x001D07A0},
};

// Describes the door query and stack cleanup, plus the soldier getter's identity address.
Layout make_layout(const ProfileSites &sites) {
    return {
        .door_query = {0x00098A41, "EntityDoor::Update object query",
                       fc::direct_call_to({std::string{sites.profile}, sites.object_query, "NetGame::GetObjectsInRange"})},
        .query_cleanup = {0x00098A46, "EntityDoor::Update query stack cleanup", fc::exact_bytes({0x83, 0xC4, 0x18})},
        .soldier_rtti_getter = {0x000DE050, "EntitySoldier::GetDerivedRtti"},
    };
}

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

} // namespace fc_plugins::door_corpse_fix
