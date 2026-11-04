#include "handler.hpp"

#include "comparison.hpp"

#include <cstdint>
#include <format>
#include <utility>

namespace fc_plugins::shot_sequence {
namespace {

// Cannon::FireDone compares its elapsed cooldown in XMM0 with this field on the Cannon in ESI.
constexpr auto kCooldownLimitOffset = 0xB8;

// Cannon::Fire resolves local input from its owner at +6C and that owner's native player index at +D4.
constexpr auto kOwnerOffset = 0x6C;
constexpr auto kPlayerIndexOffset = 0xD4;

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    plan_cooldown(plan, *layout);
    if (role_ == fc::Role::Client) {
        plan_pattern(plan, *layout);
    }
}

void Handler::plan_cooldown(fc::Plan &plan, const Layout &layout) {
    plan.check(layout.compare.rva, 7, layout.compare.evidence);
    plan.hook_instruction(layout.incomplete_branch, [this](fc::CpuContext &context) noexcept {
        const float elapsed = fc::read_field<float>(&context.xmm[0].low);
        const float limit = fc::read_field<float>(context.esi, kCooldownLimitOffset);
        context.eflags = correct_cooldown_comparison(context.eflags, elapsed, limit, applies());
    });
}

void Handler::plan_pattern(fc::Plan &plan, const Layout &layout) {
    get_joystick_index_ = plan.function(layout.get_joystick_index);
    network_enabled_memory_ = plan.access(layout.network_enabled);
    client_world_memory_ = plan.access(layout.client_world);
    local_turn_memory_ = plan.access(layout.local_turn);
    plan.check(layout.pattern_store.rva - 3, 3, fc::exact_bytes({0x8D, 0x41, 0x01}));

    plan.hook_instruction(layout.pattern_store, [this](fc::CpuContext &context) noexcept {
        if (*network_enabled_ == 0 || *client_world_ == 0 || *local_turn_ != 0) {
            return;
        }

        const auto owner = fc::read_field<std::uintptr_t>(context.esi, kOwnerOffset);
        const auto player = fc::read_field<std::int32_t>(owner, kPlayerIndexOffset);
        const auto local_slot = get_joystick_index_.call(player);
        if (local_slot && *local_slot >= 0) {
            // Replayed local shots create no projectile, so keep the pattern index for the next fresh shot.
            context.eax = context.ecx;
        }
    });
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    // Hosts advertise the correction; joining clients require agreement independently of pattern preservation.
    const auto feature = multiplayer_.feature({"FC.CannonCooldownFix", fc::FeatureRole::Both});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    if (role_ == fc::Role::Client) {
        return prepare_pattern();
    }
    return {};
}

fc::Result<> Handler::prepare_pattern() {
    const auto bind_view = [](const fc::Memory<const std::uint8_t> &memory, const std::uint8_t *&pointer) -> fc::Result<> {
        const auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }
        pointer = view->data();
        return {};
    };
    if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
        return result;
    }
    if (auto result = bind_view(client_world_memory_, client_world_); !result) {
        return result;
    }
    return bind_view(local_turn_memory_, local_turn_);
}

bool Handler::applies() const noexcept {
    const auto state = game_.current();
    if (!state) {
        return false;
    }
    if (state->mode == fc::GameMode::Local || state->mode == fc::GameMode::Host || state->mode == fc::GameMode::Dedicated) {
        return true;
    }
    if (state->mode == fc::GameMode::Client) {
        const auto host = multiplayer_.host();
        return host.value != 0 && feature_.agreement(host) == fc::Agreement::Compatible;
    }
    return false;
}

} // namespace fc_plugins::shot_sequence
