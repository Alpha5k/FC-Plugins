#include "handler.hpp"

#include "native.hpp"

#include <bit>

namespace fc_plugins::network_diagnostics {

void Handler::plan_combat(fc::Plan &plan, const CombatLayout &layout) {
    combat_native_ = layout.native;

    // Track cannon calls so the interior aim-matrix sample joins the enclosing Fire record.
    plan.observe<FireInvocation>(layout.weapon_fire, fc::before([this](void *weapon, FireInvocation &invocation) noexcept {
                                     invocation.start = stamp();
                                     invocation.record = {
                                         .weapon = native::pointer_id(weapon),
                                         .owner = native::pointer_id(native::read<void *>(weapon, combat_native_.weapon_owner)),
                                         .aimer = native::pointer_id(native::read<void *>(weapon, combat_native_.weapon_aimer)),
                                         .pattern_before = native::read<std::uint32_t>(weapon, 0x11C),
                                         .prediction_turn = *simulation_prediction_turn_,
                                         .world_pass = world_pass(),
                                     };

                                     // Save the enclosing call so the after callback can restore it after nested firing.
                                     auto &state = thread_state();
                                     invocation.previous = state.fire;
                                     state.fire = &invocation;
                                 }),
                                 fc::after([this](void *weapon, bool accepted, const FireInvocation &invocation) noexcept {
                                     if (auto &state = thread_state(); state.fire == &invocation) {
                                         state.fire = invocation.previous;
                                     }

                                     auto record = invocation.record;
                                     // Client simulation can return true without creating a projectile.
                                     record.accepted = static_cast<std::uint32_t>(accepted);
                                     record.duration = stamp() - invocation.start;
                                     record.pattern_after = native::read<std::uint32_t>(weapon, 0x11C);

                                     write(trace::RecordKind::WeaponFire, record, record.weapon,
                                           static_cast<std::uint16_t>(trace::Combat | (accepted ? trace::Accepted : trace::Rejected)));
                                 }));

    // GetFireMatrix has returned; this aim basis precedes spread and random transforms for individual shots.
    // Instruction before observers run before any modifier at the site and before the displaced instructions.
    plan.observe_instruction(layout.fire_matrix, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 if (state.fire != nullptr && fc::stack_pointer(context) != 0) {
                                     auto &record = state.fire->record;
                                     record.matrix = fc::read_field<decltype(record.matrix)>(fc::stack_pointer(context),
                                                                                             combat_native_.fire_matrix_stack);
                                 }
                             }));

    // Observe bullet construction, including a null result when construction fails or the pool is exhausted.
    // Native addresses join later bullet updates only for that object's lifetime; the pool can reuse them.
    plan.observe<TimedInvocation>(
        layout.bullet_build, fc::before([this](void *, const void *, TimedInvocation &invocation) noexcept {
            invocation.start = stamp();
        }),
        fc::after([this](void *projectile_class, const void *descriptor, void *projectile, const TimedInvocation &invocation) noexcept {
            trace::ProjectileBuildRecord record{
                .projectile = native::pointer_id(projectile),
                .projectile_class = native::pointer_id(projectile_class),
                .descriptor = native::pointer_id(descriptor),
                .accepted = projectile != nullptr ? 1U : 0U,
                .lifetime = native::read<float>(projectile, combat_native_.projectile_lifetime),
                .duration = stamp() - invocation.start,
            };
            native::copy(record.position, projectile, combat_native_.projectile_position);
            native::copy(record.velocity, projectile, combat_native_.projectile_velocity);

            write(trace::RecordKind::ProjectileBuild, record, record.projectile,
                  static_cast<std::uint16_t>(trace::Combat | (projectile != nullptr ? trace::Accepted : trace::Rejected)));
        }));

    // Attribute nested collision and damage calls to the active OrdnanceBullet::Update.
    // Other ordnance classes and damage overrides have separate native paths outside this coverage.
    plan.observe<ProjectileInvocation>(
        layout.bullet_update, fc::before([this](void *projectile, float delta, ProjectileInvocation &invocation) noexcept {
            invocation.start = stamp();
            invocation.record = {.projectile = native::pointer_id(projectile), .delta = delta};

            auto &state = thread_state();
            invocation.previous = state.projectile;
            state.projectile = &invocation;
        }),
        fc::after([this](void *projectile, float, bool result, const ProjectileInvocation &invocation) noexcept {
            if (auto &state = thread_state(); state.projectile == &invocation) {
                state.projectile = invocation.previous;
            }

            auto record = invocation.record;
            record.duration = stamp() - invocation.start;
            native::copy(record.position, projectile, combat_native_.projectile_position);
            native::copy(record.velocity, projectile, combat_native_.projectile_velocity);

            // A false return requests later removal. Early-return modes can keep a bullet without simulating it.
            write(trace::RecordKind::ProjectileSimulation, record, record.projectile,
                  static_cast<std::uint16_t>(trace::Combat | (!result ? trace::End : trace::None)));
        }));

    // I_RayHit receives vectors and filters on the stack, with ray length in XMM3 on both retail images.
    plan.observe_instruction(layout.ray_query, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 auto *invocation = state.projectile;
                                 if (invocation == nullptr || fc::stack_pointer(context) == 0) {
                                     return;
                                 }

                                 auto &record = invocation->record;
                                 ++record.rays;

                                 // Sequential outer queries replace the saved details; nested queries only increase the count.
                                 if (invocation->ray_depth++ != 0) {
                                     return;
                                 }

                                 const auto stack = fc::stack_pointer(context);
                                 record.ray_filter = native::pointer_id(native::read<void *>(stack, combat_native_.ray_filter));
                                 record.ray_filter_secondary =
                                     native::pointer_id(native::read<void *>(stack, combat_native_.ray_filter_secondary));
                                 record.ray_filter_word = native::read<std::uint32_t>(stack, combat_native_.ray_filter_word);
                                 record.ray_flags =
                                     (native::read<std::uint32_t>(stack, combat_native_.ray_flags) & native::kRayPrimaryFlagsMask) |
                                     ((native::read<std::uint32_t>(stack, combat_native_.ray_flags_high) & native::kRaySecondaryFlagsMask)
                                      << native::kRaySecondaryFlagsShift);
                                 record.ray_length = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low));
                                 native::copy(record.origin, native::read<const void *>(stack, combat_native_.ray_origin));
                                 native::copy(record.direction, native::read<const void *>(stack, combat_native_.ray_direction));
                             }));

    // The common I_RayHit epilogue holds the target in EAX and hit fraction in XMM0 before its displaced instructions.
    plan.observe_instruction(layout.ray_result, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 auto *invocation = state.projectile;
                                 if (invocation == nullptr || invocation->ray_depth == 0 || --invocation->ray_depth != 0) {
                                     return;
                                 }

                                 invocation->record.ray_target = native::pointer_id(reinterpret_cast<const void *>(context.eax));
                                 invocation->record.ray_result = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
                             }));

    // These two base Damageable bodies report health and shield changes even outside a tracked bullet update.
    // The active bullet identifies the enclosing update; the recorded ray may be a different query from the damaging one.
    plan.observe<DamageInvocation>(
        layout.apply_damage,
        fc::before([this](void *damageable, const void *descriptor, std::uint32_t flags, DamageInvocation &invocation) noexcept {
            begin_damage(damageable, descriptor, flags, trace::DamageKind::Direct, invocation);
        }),
        fc::after([this](void *damageable, const void *, std::uint32_t, bool result, const DamageInvocation &invocation) noexcept {
            finish_damage(damageable, result, invocation);
        }));

    plan.observe<DamageInvocation>(layout.apply_damage_at_position,
                                   fc::before([this](void *damageable, const void *descriptor, const void *, const void *, std::uint32_t,
                                                     std::uint32_t flags, DamageInvocation &invocation) noexcept {
                                       begin_damage(damageable, descriptor, flags, trace::DamageKind::Impact, invocation);
                                   }),
                                   fc::after([this](void *damageable, const void *, const void *, const void *, std::uint32_t,
                                                    std::uint32_t, bool result, const DamageInvocation &invocation) noexcept {
                                       finish_damage(damageable, result, invocation);
                                   }));
}

void Handler::begin_damage(void *damageable, const void *descriptor, std::uint32_t flags, trace::DamageKind kind,
                           DamageInvocation &invocation) noexcept {
    const auto &state = thread_state();
    const auto *projectile = state.projectile;
    invocation.start = stamp();

    // The descriptor's first pointer identifies the source Character; it is separate from the active bullet.
    invocation.record = {
        .damageable = native::pointer_id(damageable),
        .source = descriptor == nullptr ? 0 : native::pointer_id(native::read<void *>(descriptor)),
        .descriptor = native::pointer_id(descriptor),
        .projectile = projectile != nullptr ? projectile->record.projectile : 0,
        .flags = flags,
        .health_before = native::read<float>(damageable, combat_native_.damage_health),
        .shield_before = native::read<float>(damageable, combat_native_.damage_shield),
        .kind = kind,
    };
}

void Handler::finish_damage(void *damageable, bool result, const DamageInvocation &invocation) noexcept {
    auto record = invocation.record;
    record.health_after = native::read<float>(damageable, combat_native_.damage_health);
    record.shield_after = native::read<float>(damageable, combat_native_.damage_shield);
    // ApplyDamage returns whether the target remains alive; health and shield deltas show the damage outcome.
    record.result = static_cast<std::uint8_t>(result);
    record.duration = stamp() - invocation.start;

    write(trace::RecordKind::Damage, record, record.damageable, trace::Combat);
}

} // namespace fc_plugins::network_diagnostics
