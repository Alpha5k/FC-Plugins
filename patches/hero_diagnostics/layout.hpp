#pragma once

#include "native.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::hero_diagnostics {

// Observed calls use x86 register and stack locations; individual comments explain retained parameter names.
using PredictFunction = bool(__cdecl *)() noexcept;
using VoidFunction = void(__cdecl *)() noexcept;
// The concrete Chase callback receives RedCamera after its interpolated matrix has been copied.
using ChaseSetupFunction = void(__thiscall *)(void *camera, void *red_camera) noexcept;

using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player) noexcept;

using StateFunction = void(__thiscall *)(void *weapon) noexcept;

// SetAction takes SoldierState, a matrix, and an animation hash; blend retains that final word's bits as a float.
using ActionFunction = void(__thiscall *)(void *animator, std::uint32_t state, const float *matrix, float blend) noexcept;
using PoseFunction = void(__thiscall *)(void *animator, void *pose) noexcept;
using RenderFunction = void(__thiscall *)(void *renderable, std::uint32_t first_person, float opacity, std::uint32_t flags) noexcept;
using SimulationFunction = bool(__thiscall *)(void *controllable, float delta) noexcept;
using AnimationAdvanceFunction = void(__thiscall *)(void *instance, float delta) noexcept;

using ControllerFunction = void(__thiscall *)(void *controller, float delta) noexcept;

using ControlsFunction = bool(__thiscall *)(void *weapon) noexcept;
// Forward, lateral, and vertical speeds use scalar references; the third reference holds a dimensionless override blend.
// Native true means a velocity override was applied.
using VelocityFunction = bool(__thiscall *)(void *weapon, float *first, float *second, float *third, float *fourth) noexcept;

// Deflect takes an Ordnance projectile and position/direction pointers; filter carries the x86 direction address.
using ContactFunction = bool(__thiscall *)(void *weapon, void *target, float *position, std::uint32_t filter) noexcept;

// JumpUsingEnergy and RollUsingEnergy spend the configured cost and refund it if the virtual movement attempt fails.
using LocomotionFunction = bool(__thiscall *)(void *soldier) noexcept;

using UpdateFunction = bool(__thiscall *)(void *weapon, float delta) noexcept;
using SetNetworkStateFunction = void(__thiscall *)(void *weapon, std::int32_t state, bool flag) noexcept;
using EnterFunction = void(__thiscall *)(void *weapon, std::int32_t state) noexcept;

// SetWeaponComboState takes WeaponState, restart, current/next animation IDs, and full-body.
using AnimatorStateFunction = void(__thiscall *)(void *animator, std::int32_t state, bool active, std::uint8_t primary,
                                                 std::uint8_t secondary, bool blend) noexcept;
using GameSoundPlayFunction = void(__thiscall *)(void *sound, const void *position, const void *velocity, std::uint32_t reserved,
                                                 bool force) noexcept;

using StreamFunction = void(__thiscall *)(void *weapon, void *stream) noexcept;

// ReadObject receives the soldier in ECX, followed by a packet stream and object state.
using SoldierReadFunction = void(__thiscall *)(void *soldier, void *stream, void *object_state) noexcept;

using SoldierStateFunction = bool(__thiscall *)(void *soldier, std::int32_t state) noexcept;
// Notify receives state, elapsed seconds, input lock, aiming, and a borrowed velocity vector.
using NotifyFunction = void(__thiscall *)(void *weapon, std::int32_t state, float elapsed, bool locked, bool aiming,
                                          const float *velocity) noexcept;
using ReadBitsFunction = void(__thiscall *)(void *stream, std::uint32_t *value, std::int32_t bits) noexcept;
using WriteBitsFunction = void(__thiscall *)(void *stream, std::uint32_t value, std::int32_t bits) noexcept;

// Retail SoldierAnimator offsets mix animation phase, seconds, blend factors, enums, and pointers.
struct AnimatorOffsets {
    // Accumulated animation delta in seconds.
    std::size_t extra_delta;

    // Normalized phases in the upper and lower animations.
    std::size_t upper_time;
    std::size_t lower_time;

    // Animation timers in seconds.
    std::size_t upper_timer;
    std::size_t lower_timer;

    // Dimensionless blend factors and weapon-animation rate.
    std::size_t upper_blend;
    std::size_t lower_blend;

    std::size_t animation_rate;

    // Borrowed ZephyrAnim pointers in animator storage.
    std::size_t upper_clip;
    std::size_t lower_clip;

    // ActionAnimation, WeaponAnimation, and generic WeaponState enum values.
    std::size_t action;
    std::size_t weapon_animation;
    std::size_t weapon_state;

    // High byte of SoldierAnimator's packed aim-state flags.
    std::size_t animation_flags;

    // Native finished flags can also mark loop completion.
    std::size_t upper_finished;
    std::size_t lower_finished;
};

// Native reset/destruction entries receive the complete object in ECX with no stack arguments.
struct LifecycleLayout {
    fc::FunctionLocation<StateFunction> clear_input;
    fc::FunctionLocation<StateFunction> weapon_destroy;
    fc::FunctionLocation<StateFunction> soldier_reset;
    fc::FunctionLocation<StateFunction> soldier_destroy;
};

struct DetailLayout {
    fc::FunctionLocation<ControllerFunction> controller;
    fc::CodeLocation controller_selected;
    fc::CodeLocation controller_reset;
    fc::CodeLocation forward_neutral;
    fc::CodeLocation trigger_entry;
    std::array<fc::CodeLocation, 8> trigger_returns;
    std::array<fc::CodeLocation, 4> sprint_routes;
    std::array<fc::CodeLocation, 9> controls_gates;
    std::array<fc::CodeLocation, 5> automatic_decisions;
    std::array<fc::CodeLocation, 4> selector_returns;

    fc::FunctionLocation<SoldierStateFunction> soldier_state;
    fc::FunctionLocation<NotifyFunction> notify;
    fc::FunctionLocation<LocomotionFunction> sprint_admission;
    std::array<fc::CodeLocation, 9> locomotion_decisions;
    // Scalar comparison targets for decision 5 and decisions 7/8, respectively.
    std::array<std::uint32_t, 2> locomotion_thresholds;
    std::array<fc::CodeLocation, 4> sprint_cone;
    fc::CodeLocation animation_producer;
    std::array<fc::CodeLocation, 3> movement_animation;
    std::array<fc::CodeLocation, 6> motion_operands;
    std::array<fc::CodeLocation, 2> orientation;
    std::array<fc::CodeLocation, 4> rotation_producers;
    fc::FunctionLocation<StreamFunction> energy_read;
    fc::FunctionLocation<StreamFunction> energy_write;
    fc::FunctionLocation<ReadBitsFunction> read_bits;
    fc::FunctionLocation<WriteBitsFunction> write_bits;
    fc::CodeLocation condition_entry;
    std::array<fc::CodeLocation, 5> condition_returns;
    std::array<fc::CodeLocation, 5> predicate_operands;
    std::array<fc::CodeLocation, 5> predicate_results;
    std::array<fc::CodeLocation, 2> time_comparisons;
    fc::CodeLocation native_queue_ready;
    fc::CodeLocation consume_input;
    fc::CodeLocation base_read_return;
    std::array<fc::CodeLocation, 2> submissions;
    std::array<fc::CodeLocation, 2> submission_returns;
    fc::FunctionLocation<RenderFunction> render;
    std::array<fc::CodeLocation, 4> pose_clock;
    std::array<fc::CodeLocation, 2> animation_entry;
    std::array<fc::CodeLocation, 2> animation_selection;
    fc::FunctionLocation<AnimationAdvanceFunction> animation_advance;
    fc::CodeLocation asset_name;
    std::array<fc::CodeLocation, 2> generic_timer;
    fc::CodeLocation generic_timer_write;
    fc::FunctionLocation<SimulationFunction> simulation;
    fc::FunctionLocation<SimulationFunction> post_collision;
    std::array<fc::CodeLocation, 3> collision_decisions;
    // Scalar comparison targets for the first two collision decisions.
    std::array<std::uint32_t, 2> collision_thresholds;
    std::array<fc::CodeLocation, 14> integration;
    fc::DataLocation<std::uint32_t> gravity_sign_operand;
    fc::FunctionLocation<UpdateFunction> combat_update;
    std::array<fc::CodeLocation, 11> combat_decisions;
    fc::CodeLocation jump_compare;
    fc::CodeLocation jump_cost;
    std::array<fc::CodeLocation, 2> jump_results;
};

// Client locations cover melee simulation, presentation, and the network globals sampled by each record.
struct ClientLayout {
    LifecycleLayout lifetime;
    DetailLayout detail;
    fc::FunctionLocation<PredictFunction> predict;
    fc::FunctionLocation<ChaseSetupFunction> chase_setup;
    AnimatorOffsets animator;

    fc::FunctionLocation<UpdateFunction> update;
    fc::FunctionLocation<SetNetworkStateFunction> set_network_state;
    fc::FunctionLocation<EnterFunction> enter_state;

    fc::FunctionLocation<AnimatorStateFunction> animator_state;

    fc::CodeLocation input_queue;
    fc::CodeLocation prediction_transition;
    fc::CallLocation<GameSoundPlayFunction> transition_sound;

    fc::FunctionLocation<SoldierReadFunction> soldier_read;

    fc::FunctionLocation<StreamFunction> read;
    fc::FunctionLocation<StateFunction> exit_state;

    fc::FunctionLocation<ActionFunction> action;
    fc::FunctionLocation<PoseFunction> pose;

    fc::FunctionLocation<ControlsFunction> controls;
    fc::FunctionLocation<VelocityFunction> velocity;

    fc::CodeLocation parsed_move;

    fc::FunctionLocation<ContactFunction> contact;

    fc::FunctionLocation<LocomotionFunction> jump;
    fc::FunctionLocation<LocomotionFunction> roll;

    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;

    fc::DataLocation<const std::int32_t> host_turn;
    fc::DataLocation<const std::int32_t> client_turn;
    fc::DataLocation<const std::int32_t> update_turn;
    fc::DataLocation<const std::int32_t> prediction_turn;
    fc::DataLocation<const std::int32_t> acknowledged_turn;
    fc::DataLocation<const std::int32_t> destination;

    fc::DataLocation<const std::uint8_t> local_prediction_pass;
    fc::DataLocation<const std::uint8_t> authoritative_update_pass;
    fc::DataLocation<const std::uint8_t> rollback;

    fc::DataLocation<const std::uint8_t> network_enabled;
    fc::DataLocation<const std::uint8_t> network_client_active;

    fc::DataLocation<const float> frame_delta;

    fc::DataLocation<const native::PlayerMove, native::kPlayerCapacity> remote_moves;
};

// Dedicated server locations omit presentation state and expose prepared input and readiness boundaries.
struct ServerLayout {
    LifecycleLayout lifetime;
    DetailLayout detail;
    fc::CodeLocation readiness_retained;
    fc::CodeLocation readiness_default;
    fc::CodeLocation readiness_publication;
    fc::FunctionLocation<VoidFunction> late_replay;
    fc::DataLocation<const native::PlayerMove *const, native::kPlayerCapacity> active_inputs;
    fc::FunctionLocation<UpdateFunction> update;
    fc::FunctionLocation<EnterFunction> enter_state;
    fc::FunctionLocation<StateFunction> exit_state;

    fc::FunctionLocation<ControlsFunction> controls;
    fc::FunctionLocation<VelocityFunction> velocity;

    fc::FunctionLocation<StreamFunction> write;

    fc::FunctionLocation<ContactFunction> contact;

    fc::DataLocation<const std::int32_t> host_turn;
    fc::DataLocation<const std::int32_t> destination;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ClientLayout> select_client_layout(const std::string_view profile);

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ServerLayout> select_server_layout(const std::string_view profile);

} // namespace fc_plugins::hero_diagnostics
