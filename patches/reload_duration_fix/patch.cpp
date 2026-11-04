#include "patch.hpp"

#include "layout.hpp"

#include <cstddef>
#include <expected>
#include <format>
#include <cstdint>

namespace fc_plugins::reload_duration_fix {
namespace {

// Restores firearm reload duration after network updates to locally controlled weapons on joining clients.
class Handler {
  public:
    // Retains the local input lookup and replaces the native weapon-state reader.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        get_joystick_index_ = plan.function(layout->get_joystick_index);
        melee_vtable_memory_ = plan.access(layout->melee_vtable);

        weapon_read_ = plan.hook(layout->weapon_read, [this](Weapon *weapon, NetPktGroup *packet) noexcept {
            read_weapon(weapon, packet);
        });
    }

    // Retains current game participation for the native reader's duration repair.
    fc::Result<> prepare(fc::PrepareContext &context) {
        game_ = context.game();
        const auto melee_vtable = melee_vtable_memory_.view();
        if (!melee_vtable) {
            return std::unexpected(melee_vtable.error());
        }
        melee_vtable_ = fc::read_field<const void *>(melee_vtable->data());
        return {};
    }

  private:
    // Reads the network update, then restores the duration if a locally controlled firearm is reloading.
    void read_weapon(Weapon *weapon, NetPktGroup *packet) const noexcept {
        // Decode first so the repair uses the received state and preserves its elapsed reload time.
        if (!weapon_read_.call(weapon, packet)) {
            return;
        }

        const auto state = game_.current();
        if (!state || state->mode != fc::GameMode::Client) {
            return;
        }

        if (fc::read_field<std::int32_t>(weapon, kWeaponStateOffset) != kReloadState) {
            return;
        }

        // Melee uses RELOAD for deflection and keeps the cooldown duration assigned by the game.
        if (fc::read_field<const void *>(weapon) == melee_vtable_) {
            return;
        }

        // Weapon::Read has already used this owner. A local input slot identifies the joining player's weapon.
        const auto *owner = fc::read_field<const void *>(weapon, kWeaponOwnerOffset);
        const auto player_id = fc::read_field<std::int32_t>(owner, kPlayerIdOffset);
        if (get_joystick_index_.call(player_id).value_or(-1) < 0) {
            return;
        }

        // Weapon::Read omits the duration that EnterReload normally sets. Restore it without restarting
        // reload audio or resetting the elapsed time; Cannon and Grenade readers also do this after base Read.
        const auto *weapon_class = fc::read_field<const void *>(weapon, kWeaponClassOffset);
        const auto duration = fc::read_field<float>(weapon_class, kClassReloadTimeOffset);
        fc::write_field(weapon, kWeaponStateLimitOffset, duration);
    }

    fc::NativeFunction<WeaponRead> weapon_read_;
    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::Game game_;
    fc::Memory<const std::byte> melee_vtable_memory_;
    const void *melee_vtable_{};
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "ReloadDurationFix",
                     .description = "Fixes the duplicated weapon reload sounds in multiplayer on high ping.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::reload_duration_fix
