#include "patch.hpp"
#include "layout.hpp"

#include <cstdint>
#include <format>

namespace fc_plugins::crouch_bug_fix {
namespace {

// Keeps nonlocal players' retained crouch levels while running the native trigger update.
class Handler {
  public:
    // Verifies the crouch arguments and destination, then replaces this one direct CALL.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan.check(layout->crouch_arguments, Layout::kArgumentSize);

        get_joystick_index_ = plan.function(layout->get_joystick_index);

        trigger_update_ = plan.hook_call(layout->crouch_update, [this](Trigger *trigger, float elapsed_time, bool down) noexcept {
            update_crouch(trigger, elapsed_time, down);
        });
    }

  private:
    // Keeps a nonlocal player's retained crouch level while the native Trigger still advances elapsed time.
    void update_crouch(Trigger *trigger, float elapsed_time, bool down) const noexcept {
        // This call receives the crouch member, so recover its containing Controllable to read player identity.
        const auto player_id = fc::read_field<std::int32_t>(trigger, Layout::kPlayerIdOffset - Layout::kCrouchTriggerOffset);

        // Nonlocal humans have no local input slot. This also includes authoritative remote players on a
        // player host, whose received crouch input is replaced by the retained level under this rule.
        const auto joystick = get_joystick_index_.call(player_id);
        if (joystick && *joystick < 0) {
            const auto state = fc::read_field<std::uint32_t>(trigger);
            down = (state & Layout::kTriggerLevel) != 0;
        }

        // Trigger::Update still advances hold time and press/release actions with the selected down level.
        static_cast<void>(trigger_update_.call(trigger, elapsed_time, down));
    }

    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::NativeFunction<TriggerUpdate> trigger_update_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "CrouchBugFix",
                     .description = "Fixes the bug where crouching players can appear uncrouched in multiplayer.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::crouch_bug_fix
