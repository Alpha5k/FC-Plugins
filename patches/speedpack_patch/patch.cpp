#include "patch.hpp"

#include <fc/native.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::speedpack_patch {
namespace {

struct Entity;
struct WeaponDispenser;

// Fire takes the dispenser in ECX and returns the created entity in EAX.
using DispenserFire = Entity *(__thiscall *)(WeaponDispenser *) noexcept;

// Dimensionless charge strength used to interpolate the class's launch multipliers.
constexpr std::size_t kStrengthOffset = 0x114;

// Locates the native dispenser fire entry before it scales launch speed.
struct Layout {
    fc::FunctionLocation<DispenserFire> fire;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .fire = {.rva = 0x00284C90, .name = "WeaponDispenser::Fire"},
    };
}

// Clamps dispenser strength before native launch-speed scaling.
class Handler {
  public:
    // Replaces Fire while retaining its ordinary entity creation and launch behavior.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        fire_ = plan.hook(layout->fire, [this](WeaponDispenser *dispenser) noexcept {
            return fire(dispenser);
        });
    }

  private:
    // Clamps finite strength to [0,1] and replaces nonfinite strength with zero before Fire uses it.
    Entity *fire(WeaponDispenser *dispenser) const noexcept {
        const auto strength = fc::read_field<float>(dispenser, kStrengthOffset);

        // Fire interpolates between the class's minimum and maximum launch multipliers, then adds owner
        // velocity. Bounding strength prevents extrapolation; zero selects the minimum multiplier.
        const float clamped = std::isfinite(strength) ? std::clamp(strength, 0.0F, 1.0F) : 0.0F;
        fc::write_field(dispenser, kStrengthOffset, clamped);

        return fire_.call(dispenser).value_or(nullptr);
    }

    fc::NativeFunction<DispenserFire> fire_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "SpeedpackPatch",
                     .description = "Fixes the bug where players can throw glitched detpacks at very high speeds.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::speedpack_patch
