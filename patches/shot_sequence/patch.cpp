#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::shot_sequence {
namespace {

// Supplies the launch role selected by the runtime variant.
template <fc::Role Role> struct RoleHandler : Handler {
    RoleHandler() noexcept : Handler(Role) {}
};

} // namespace

fc::Patch definition(fc::Role role) {
    fc::Patch patch{
        .id = "ShotSequence",
        .description = "Fixes weapon cooldowns and inconsistent projectile spread in multiplayer.",
        .category = "Multiplayer",
        .enabled = true,
    };
    if (role == fc::Role::Client) {
        patch.variants = {fc::variant<RoleHandler<fc::Role::Client>>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, role)};
    } else {
        patch.variants = {fc::variant<RoleHandler<fc::Role::Server>>({"BF2.GOG.59EDF52B"}, role)};
    }
    return patch;
}

} // namespace fc_plugins::shot_sequence
