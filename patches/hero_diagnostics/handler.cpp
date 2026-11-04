#include "handler.hpp"

#include "native.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cstring>
#include <expected>
#include <format>
#include <utility>

namespace fc_plugins::hero_diagnostics {
namespace {

// Optional owner callbacks preserve the native thread's ambient error values.
struct AmbientErrors {
    DWORD win32{GetLastError()};
    int winsock{WSAGetLastError()};

    ~AmbientErrors() {
        WSASetLastError(winsock);
        SetLastError(win32);
    }
};

// Labels local passes Prediction and update-only passes Authority; both native flags can also be restored between passes.
std::uint16_t turn_flags(const trace::TurnContext &turn) noexcept {
    if (turn.local_turn != 0) {
        return trace::Prediction;
    }

    return turn.update_pass != 0 ? trace::Authority : trace::None;
}

// Compares the discrete locomotion state, exhaustion, and airborne status for change records.
bool same_locomotion_classification(const trace::LocomotionState &first, const trace::LocomotionState &second) noexcept {
    return first.state == second.state &&
           (first.energy_flags & native::kEnergyExhausted) == (second.energy_flags & native::kEnergyExhausted) &&
           (first.movement_flags & native::kAirborne) == (second.movement_flags & native::kAirborne);
}

} // namespace

thread_local Handler::ThreadState Handler::thread_state_{};

Handler::Handler(const Settings &settings, fc::Role role) noexcept : settings_(settings), role_(role), subjects_(role_) {}

void Handler::plan(fc::Plan &plan) {
    if (role_ == fc::Role::Client) {
        // Function observers surround the behavior owner, which can omit the original native call.
        const auto layout = select_client_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan_client(plan, *layout);
        plan_native_details(plan, layout->detail);
        animator_ = layout->animator;

        // These globals describe the selected native world's passes and latest remote input rows.
        get_joystick_index_ = plan.function(layout->get_joystick_index);

        host_turn_memory_ = plan.access(layout->host_turn);
        client_turn_memory_ = plan.access(layout->client_turn);
        update_turn_memory_ = plan.access(layout->update_turn);
        prediction_turn_memory_ = plan.access(layout->prediction_turn);
        acknowledged_turn_memory_ = plan.access(layout->acknowledged_turn);
        destination_memory_ = plan.access(layout->destination);

        local_prediction_pass_memory_ = plan.access(layout->local_prediction_pass);
        authoritative_update_pass_memory_ = plan.access(layout->authoritative_update_pass);
        rollback_memory_ = plan.access(layout->rollback);

        network_enabled_memory_ = plan.access(layout->network_enabled);
        network_client_active_memory_ = plan.access(layout->network_client_active);

        frame_delta_memory_ = plan.access(layout->frame_delta);

        remote_moves_memory_ = plan.access(layout->remote_moves);

        return;
    }

    // Dedicated server observations use the GOG image's simulation and serialization paths.
    const auto layout = select_server_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    plan_server(plan, *layout);
    plan_native_details(plan, layout->detail);

    // The native server input-pointer table identifies each player's currently prepared processed input.
    host_turn_memory_ = plan.access(layout->host_turn);
    destination_memory_ = plan.access(layout->destination);

    active_inputs_memory_ = plan.access(layout->active_inputs);
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    // The runtime prepares all access reservations before Prepare exposes their borrowed native views.
    const auto bind_view = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }

        pointer = view->data();
        return {};
    };

    if (role_ == fc::Role::Client) {
        if (auto result = bind_view(host_turn_memory_, host_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(client_turn_memory_, client_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(update_turn_memory_, update_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(prediction_turn_memory_, prediction_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(acknowledged_turn_memory_, acknowledged_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(destination_memory_, destination_); !result) {
            return result;
        }

        if (auto result = bind_view(local_prediction_pass_memory_, local_prediction_pass_); !result) {
            return result;
        }

        if (auto result = bind_view(authoritative_update_pass_memory_, authoritative_update_pass_); !result) {
            return result;
        }

        if (auto result = bind_view(rollback_memory_, rollback_); !result) {
            return result;
        }

        if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
            return result;
        }

        if (auto result = bind_view(network_client_active_memory_, network_client_active_); !result) {
            return result;
        }

        if (auto result = bind_view(frame_delta_memory_, frame_delta_); !result) {
            return result;
        }

        if (auto result = bind_view(remote_moves_memory_, remote_moves_); !result) {
            return result;
        }

        subjects_.set_joystick_index(get_joystick_index_);
    } else {
        if (auto result = bind_view(host_turn_memory_, host_turn_); !result) {
            return result;
        }

        if (auto result = bind_view(destination_memory_, destination_); !result) {
            return result;
        }

        if (auto result = bind_view(active_inputs_memory_, active_inputs_); !result) {
            return result;
        }
    }

    // Disabled tracing returns an inert channel; the installed hooks still sample native objects.
    auto prepared_trace = context.trace({
        .name = "HeroDiagnostics",
        .format_version = trace::kSchemaVersion,
        .buffer_bytes = trace::kBufferBytes,
    });
    if (!prepared_trace) {
        return std::unexpected(prepared_trace.error());
    }

    trace_ = std::move(*prepared_trace);
    subjects_.reset();
    asset_names_.clear();

    if (auto result = context.on_available("Diagnostics.NetworkDiagnostics", network_diagnostics::api_contract(),
                                           [this](network_diagnostics::NetworkDiagnosticsApi api) noexcept {
                                               network_diagnostics_ = api;
                                               network_diagnostics_ready_.store(true, std::memory_order_release);
                                           });
        !result) {
        return result;
    }

    return context.publish(api_contract(), DiagnosticsApi{
                                               .update_decision = fc::bind<&Handler::observe_update_decision>(*this),
                                               .network_state_decision = fc::bind<&Handler::observe_network_state_decision>(*this),
                                               .history_decision = fc::bind<&Handler::observe_history_decision>(*this),
                                               .physics_decision = fc::bind<&Handler::observe_physics_decision>(*this),
                                               .current_scope = fc::bind<&Handler::current_scope>(*this),
                                               .weapon_scope = fc::bind<&Handler::weapon_scope>(*this),
                                               .selection_decision = fc::bind<&Handler::observe_selection_decision>(*this),
                                               .owner_decision = fc::bind<&Handler::observe_owner_decision>(*this),
                                               .capture_mode = fc::bind<&Handler::capture_mode>(*this),
                                           });
}

void Handler::write_status(fc::StatusContext &context) const noexcept {
    const auto health = trace_.health();
    context.line("Records: {}; dropped: {}", health.written, health.dropped);
}

std::optional<HeroSubject> Handler::snapshot_subject(HeroSubject *subject) noexcept {
    return subject != nullptr ? std::optional<HeroSubject>{*subject} : std::nullopt;
}

void Handler::plan_lifetime(fc::Plan &plan, const LifecycleLayout &layout) {
    plan.observe<ClearInputInvocation>(
        layout.clear_input, fc::before([this](void *weapon, ClearInputInvocation &invocation) noexcept {
            invocation.subject = snapshot_subject(subjects_.find_weapon(weapon));
            if (invocation.subject) {
                write_subject(trace::RecordKind::Lifecycle,
                              trace::LifecycleRecord{.turn = turn_context(), .object = native::pointer_id(weapon), .reason = 0},
                              *invocation.subject);
                invocation.queue = record_queue(*invocation.subject, 5, current_scope().operation_scope);
            }
        }),
        fc::after([this](void *, const ClearInputInvocation &invocation) noexcept {
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                record_queue(*invocation.subject, 6, invocation.queue.scope, 0.0F, &invocation.queue);
            }
        }));
    plan.observe(layout.weapon_destroy, fc::before([this](void *weapon) noexcept {
                     if (auto *subject = subjects_.find_weapon(weapon)) {
                         write_subject(trace::RecordKind::Lifecycle,
                                       trace::LifecycleRecord{.turn = turn_context(), .object = native::pointer_id(weapon), .reason = 1},
                                       *subject);
                     }
                     subjects_.retire_weapon(weapon);
                 }));
    std::uint8_t reason{2};
    for (const auto *location : {&layout.soldier_reset, &layout.soldier_destroy}) {
        plan.observe(*location, fc::before([this, reason](void *soldier) noexcept {
            if (auto *subject = subjects_.find_soldier(soldier)) {
                write_subject(trace::RecordKind::Lifecycle,
                              trace::LifecycleRecord{.turn = turn_context(), .object = native::pointer_id(soldier), .reason = reason},
                              *subject);
            }
            subjects_.retire_soldier(soldier);
        }));
        ++reason;
    }
}

HeroSubject *Handler::bind(void *weapon) noexcept {
    // The subject table follows the synchronous native simulation thread, independent of preparation and sound threads.
    auto expected = std::uint32_t{};
    const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
    subject_thread_.compare_exchange_strong(expected, thread, std::memory_order_relaxed);
    if (subject_thread_.load(std::memory_order_relaxed) != thread) {
        return nullptr;
    }
    auto *subject = subjects_.bind(weapon);
    if (subject != nullptr) {
        announce(*subject);
    }

    return subject;
}

bool Handler::announce(HeroSubject &subject) noexcept {
    if (subject.announced && subject.asset_announced) {
        return true;
    }

    const trace::SubjectRecord record{
        .subject = subject.id,
        .generation = subject.generation,
        .weapon = native::pointer_id(subject.weapon),
        .owner = native::pointer_id(subject.owner),
        .soldier = native::pointer_id(subject.soldier),
        .animator = native::pointer_id(subject.animator),
        .combo = native::pointer_id(subject.combo),
        .player = subject.player,
        .local_player = subject.local_player,
        .role = static_cast<std::uint8_t>(role_),
    };

    const auto identity = subject.local_player == trace::kNoLocalPlayer ? trace::Remote : trace::Local;
    if (!subject.announced) {
        subject.announced = write(trace::RecordKind::Subject, record, subject.id, identity);
    }
    if (subject.announced) {
        // ReadConfig copies the config's name hash into Combo +0 in both retail images.
        const auto *loaded_class = native::read<void *>(subject.soldier, 0x440);
        const trace::AssetRecord asset{.combo = native::pointer_id(subject.combo),
                                       .class_pointer = native::pointer_id(loaded_class),
                                       .class_hash = native::read<std::uint32_t>(loaded_class, 0x18),
                                       .combo_hash = native::read<std::uint32_t>(subject.combo)};
        subject.asset_announced = write(trace::RecordKind::Asset, asset, subject.id, identity);
    }
    return subject.announced && subject.asset_announced;
}

trace::RecordHeader Handler::record_header(trace::RecordKind kind, std::uint32_t subject, std::uint16_t flags) const noexcept {
    return {.subject = subject, .kind = kind, .flags = flags};
}

trace::TurnContext Handler::turn_context() const noexcept {
    // Server records substitute host_turn for update_turn and leave unavailable pass and replay fields at their defaults.
    const bool active = role_ == fc::Role::Server || (network_enabled_ != nullptr && network_client_active_ != nullptr &&
                                                      *network_enabled_ != 0 && *network_client_active_ != 0);
    const auto update = role_ == fc::Role::Server && host_turn_ != nullptr ? *host_turn_ : (update_turn_ != nullptr ? *update_turn_ : -1);
    return {
        .host_turn = host_turn_ != nullptr ? *host_turn_ : -1,
        .client_turn = client_turn_ != nullptr ? *client_turn_ : -1,
        .update_turn = update,
        .predict_turn = prediction_turn_ != nullptr ? *prediction_turn_ : -1,
        .acknowledged_turn = acknowledged_turn_ != nullptr ? *acknowledged_turn_ : -1,
        .destination = destination_ != nullptr ? *destination_ : -1,
        .local_turn = local_prediction_pass_ != nullptr ? *local_prediction_pass_ : std::uint8_t{},
        .update_pass = authoritative_update_pass_ != nullptr ? *authoritative_update_pass_ : std::uint8_t{},
        .rollback = rollback_ != nullptr ? *rollback_ : std::uint8_t{},
        .network_active = static_cast<std::uint8_t>(active),
    };
}

std::uint16_t Handler::transition_flags(void *weapon, const trace::TurnContext &turn) noexcept {
    const auto *network = thread_state_.network_state;

    // A matching topmost SetNetworkState labels nested entries Authority even when the pass globals still indicate prediction.
    return network != nullptr && network->weapon == weapon ? trace::Authority : turn_flags(turn);
}

trace::LocomotionState Handler::locomotion_state(const void *soldier) const noexcept {
    return soldier == nullptr ? trace::LocomotionState{}
                              : trace::LocomotionState{
                                    .state = native::read<std::int32_t>(soldier, native::kLocomotionStateOffset),
                                    .energy = native::read<float>(soldier, native::kEnergyOffset),
                                    .energy_flags = native::read<std::uint32_t>(soldier, native::kEnergyFlagsOffset),
                                    .movement_flags = native::read<std::uint8_t>(soldier, native::kMovementFlagsOffset),
                                };
}

void Handler::record_locomotion(HeroSubject &subject, trace::LocomotionOperation operation, const trace::LocomotionState &before,
                                const trace::LocomotionState &after, bool result) noexcept {
    const trace::LocomotionRecord record{
        .turn = turn_context(),
        .before = before,
        .after = after,
        .soldier = native::pointer_id(subject.soldier),
        .operation = operation,
        .result = static_cast<std::uint8_t>(result),
    };

    auto flags = turn_flags(record.turn);
    if (!same_locomotion_classification(before, after)) {
        flags = static_cast<std::uint16_t>(flags | trace::StateChanged);
    }

    flags = static_cast<std::uint16_t>(flags | trace::NativeCalled | (result ? trace::NativeResult : trace::None));

    write_subject(trace::RecordKind::Locomotion, record, subject, flags);
}

void Handler::begin_update(void *weapon, float requested_delta, float effective_delta, MeleeMode mode,
                           UpdateInvocation &invocation) noexcept {
    // Link this invocation before the behavior owner reports its decision; nested calls restore the previous head on return.
    invocation.weapon = weapon;
    invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
    invocation.previous = thread_state_.update;
    thread_state_.update = &invocation;

    auto *subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }

    invocation.subject = snapshot_subject(subject);

    auto &record = invocation.record;
    record.turn = turn_context();
    record.scope = invocation.scope;
    record.mode_before = native::read<std::int32_t>(weapon, native::kModeOffset);
    record.mode_time_before = native::read<float>(weapon, native::kModeTimeOffset);
    record.notified_time_before = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
    record.delta = requested_delta;
    record.effective_delta = effective_delta;
    record.state_time_before = native::read<float>(weapon, native::kStateTimeOffset);
    record.input_time_before = native::read<float>(weapon, native::kInputTimeOffset);

    record.state_before = static_cast<std::int8_t>(native::read<std::int32_t>(weapon, native::kStateOffset));
    record.previous_before = static_cast<std::int8_t>(native::read<std::int32_t>(weapon, native::kPreviousStateOffset));
    record.decision = static_cast<std::uint8_t>(mode);
    invocation.flags = turn_flags(record.turn);
}

void Handler::finish_update(void *weapon, bool result, const UpdateInvocation &invocation) noexcept {
    if (thread_state_.update == &invocation) {
        thread_state_.update = invocation.previous;
    }

    if (invocation.weapon == weapon && invocation.subject && subjects_.live(*invocation.subject)) {
        auto record = invocation.record;

        // NativeCalled follows the reported decision, defaulting to Native; result is the hook return, including suppression.
        const auto native_called = record.decision == static_cast<std::uint8_t>(MeleeMode::Native);
        record.mode_after = native::read<std::int32_t>(weapon, native::kModeOffset);
        record.mode_time_after = native::read<float>(weapon, native::kModeTimeOffset);
        record.notified_time_after = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
        record.state_time_after = native::read<float>(weapon, native::kStateTimeOffset);
        record.input_time_after = native::read<float>(weapon, native::kInputTimeOffset);
        record.state_after = static_cast<std::int8_t>(native::read<std::int32_t>(weapon, native::kStateOffset));
        record.previous_after = static_cast<std::int8_t>(native::read<std::int32_t>(weapon, native::kPreviousStateOffset));
        record.native_called = static_cast<std::uint8_t>(native_called);
        record.result = static_cast<std::uint8_t>(result);
        native::copy_floats(record.position, invocation.subject->soldier, native::kPositionOffset);
        native::copy_floats(record.velocity, invocation.subject->soldier, native::kVelocityOffset);

        auto flags = static_cast<std::uint16_t>(invocation.flags | (native_called ? trace::NativeCalled : trace::Suppressed) |
                                                (result ? trace::NativeResult : trace::None));
        if (record.state_before != record.state_after) {
            flags = static_cast<std::uint16_t>(flags | trace::StateChanged);
        }

        write_subject(trace::RecordKind::MeleeTick, record, *invocation.subject, flags);
    }
}

void Handler::observe_update_decision(void *weapon, float delta, float effective_delta, MeleeMode mode) noexcept {
    auto *invocation = thread_state_.update;
    if (invocation == nullptr || invocation->weapon != weapon) {
        return;
    }

    invocation->record.delta = delta;
    invocation->record.effective_delta = effective_delta;
    invocation->record.decision = static_cast<std::uint8_t>(mode);
}

void Handler::begin_network_state(void *weapon, std::int32_t state, bool flag, NetworkMode mode, NetworkInvocation &invocation) noexcept {
    // Link this setter to the enclosing call and copy the current read scope for its decision record.
    invocation.weapon = weapon;
    invocation.previous = thread_state_.network_state;
    thread_state_.network_state = &invocation;

    auto *subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }

    invocation.subject = snapshot_subject(subject);

    invocation.record = {
        .turn = turn_context(),
        .state_before = native::read<std::int32_t>(weapon, native::kStateOffset),
        .requested_state = state,
        .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),

        .scope = thread_state_.read_scope,
        .action = native::read<std::uint16_t>(weapon, native::kActionOffset),
        .state_flags = native::read<std::uint8_t>(weapon, native::kStateFlagsOffset),
        .set_flag = static_cast<std::uint8_t>(flag),
        .decision = static_cast<std::uint8_t>(mode),
        .operation = trace::NetworkStateOperation::Apply,
    };
    invocation.record.mode_before = native::read<std::int32_t>(weapon, native::kModeOffset);
    invocation.record.mode_time_before = native::read<float>(weapon, native::kModeTimeOffset);
    invocation.record.input_time_before = native::read<float>(weapon, native::kInputTimeOffset);
    invocation.record.notified_time_before = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
    invocation.flags = trace::Authority;
}

void Handler::finish_network_state(void *weapon, const NetworkInvocation &invocation) noexcept {
    if (thread_state_.network_state == &invocation) {
        thread_state_.network_state = invocation.previous;
    }

    if (invocation.weapon == weapon && invocation.subject && subjects_.live(*invocation.subject)) {
        auto record = invocation.record;

        // Masks and flags remain entry samples; the combo ID and timer are sampled again.
        const auto native_called = record.decision == static_cast<std::uint8_t>(NetworkMode::Native);
        record.state_after = native::read<std::int32_t>(weapon, native::kStateOffset);
        record.state_time_after = native::read<float>(weapon, native::kStateTimeOffset);
        record.mode_after = native::read<std::int32_t>(weapon, native::kModeOffset);
        record.mode_time_after = native::read<float>(weapon, native::kModeTimeOffset);
        record.input_time_after = native::read<float>(weapon, native::kInputTimeOffset);
        record.notified_time_after = native::read<float>(weapon, native::kNotifiedStateTimeOffset);
        record.native_called = static_cast<std::uint8_t>(native_called);

        auto flags = static_cast<std::uint16_t>(invocation.flags | (native_called ? trace::NativeCalled : trace::Suppressed));
        if (record.state_before != record.state_after) {
            flags = static_cast<std::uint16_t>(flags | trace::StateChanged);
        }

        write_subject(trace::RecordKind::NetworkState, record, *invocation.subject, flags);
    }
}

void Handler::observe_network_state_decision(void *weapon, std::int32_t state, std::uint32_t flag, NetworkMode mode) noexcept {
    auto *invocation = thread_state_.network_state;
    if (invocation == nullptr || invocation->weapon != weapon) {
        return;
    }

    invocation->record.requested_state = state;
    invocation->record.set_flag = static_cast<std::uint8_t>(flag);
    invocation->record.decision = static_cast<std::uint8_t>(mode);
}

void Handler::begin_enter_state(void *weapon, std::int32_t state, TransitionInvocation &invocation) noexcept {
    invocation.previous = thread_state_.transition;
    invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
    thread_state_.transition = &invocation;
    // A matching topmost SetNetworkState takes precedence over native pass labels for nested state entry.
    auto *subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }

    invocation.subject = snapshot_subject(subject);
    invocation.record = {
        .turn = turn_context(),
        .requested_state = state,
        .state_before = native::read<std::int32_t>(weapon, native::kStateOffset),
        .state_time_before = native::read<float>(weapon, native::kStateTimeOffset),
        .kind = trace::TransitionKind::Enter,
        .scope = invocation.scope,
    };
    invocation.flags = transition_flags(weapon, invocation.record.turn);
}

void Handler::finish_transition(void *weapon, const TransitionInvocation &invocation) noexcept {
    if (thread_state_.transition == &invocation) {
        thread_state_.transition = invocation.previous;
    }
    if (invocation.subject && subjects_.live(*invocation.subject)) {
        auto record = invocation.record;
        record.state_after = native::read<std::int32_t>(weapon, native::kStateOffset);
        record.previous_state = native::read<std::int32_t>(weapon, native::kPreviousStateOffset);
        record.state_time_after = native::read<float>(weapon, native::kStateTimeOffset);

        native::copy_floats(record.position, invocation.subject->soldier, native::kPositionOffset);
        native::copy_floats(record.velocity, invocation.subject->soldier, native::kVelocityOffset);

        auto flags = static_cast<std::uint16_t>(invocation.flags | trace::NativeCalled);
        if (record.state_before != record.state_after) {
            flags = static_cast<std::uint16_t>(flags | trace::StateChanged);
        }

        write_subject(trace::RecordKind::Transition, record, *invocation.subject, flags);
    }
}

void Handler::plan_velocity(fc::Plan &plan, const fc::FunctionLocation<VelocityFunction> &location) {
    // Arguments are forward and lateral velocity, override blend, and vertical velocity; the blend is dimensionless.
    plan.observe<MovementInvocation>(
        location,
        fc::before([this](void *weapon, float *first, float *second, float *third, float *fourth, MovementInvocation &invocation) noexcept {
            invocation.subject = snapshot_subject(bind(weapon));
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }

            invocation.record = trace::MovementFrameRecord{
                .turn = turn_context(),
                .before = native::vector_arguments(first, second, third, fourth),
                .state = native::read<std::int32_t>(weapon, native::kStateOffset),
                .kind = trace::MovementKind::Velocity,
            };
        }),
        fc::after([this](void *, float *first, float *second, float *third, float *fourth, bool result,
                         const MovementInvocation &invocation) noexcept {
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }

            auto record = invocation.record;
            record.after = native::vector_arguments(first, second, third, fourth);
            native::copy_floats(record.position, invocation.subject->soldier, native::kPositionOffset);
            native::copy_floats(record.velocity, invocation.subject->soldier, native::kVelocityOffset);
            record.result = static_cast<std::uint8_t>(result);

            const auto flags = static_cast<std::uint16_t>((role_ == fc::Role::Server ? trace::Authority : trace::None) |
                                                          trace::NativeCalled | (result ? trace::NativeResult : trace::None));
            write_subject(trace::RecordKind::MovementFrame, record, *invocation.subject, flags);
        }));
}

void Handler::plan_contact(fc::Plan &plan, const fc::FunctionLocation<ContactFunction> &location) {
    // WeaponMelee::Deflect receives a projectile and position pointer; the uint32_t argument named filter carries the direction pointer.
    plan.observe<ContactInvocation>(
        location,
        fc::before([this](void *weapon, void *target, float *position, std::uint32_t filter, ContactInvocation &invocation) noexcept {
            const auto parent = current_scope().operation_scope;
            invocation.previous = thread_state_.contact;
            invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
            thread_state_.contact = &invocation;
            invocation.subject = snapshot_subject(bind(weapon));
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }

            trace::MeleeContactRecord record{
                .turn = turn_context(),
                .scope = invocation.scope,
                .parent_scope = parent,
                .target = native::pointer_id(target),
                .filter = filter,
                .state = native::read<std::int32_t>(weapon, native::kStateOffset),

            };
            native::copy_floats(record.input_position, position, 0);
            invocation.record = record;
            write_subject(trace::RecordKind::MeleeContact, record, *invocation.subject, trace::Begin);
        }),
        fc::after([this](void *, void *, float *position, std::uint32_t, bool result, const ContactInvocation &invocation) noexcept {
            if (thread_state_.contact == &invocation) {
                thread_state_.contact = invocation.previous;
            }
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }

            auto record = invocation.record;
            native::copy_floats(record.output_position, position, 0);

            record.result = static_cast<std::uint8_t>(result);
            const auto flags = static_cast<std::uint16_t>(trace::End | (role_ == fc::Role::Server ? trace::Authority : trace::None) |
                                                          trace::NativeCalled | (result ? trace::NativeResult : trace::None));

            write_subject(trace::RecordKind::MeleeContact, record, *invocation.subject, flags);
        }));
}

void Handler::begin_animator_state(void *animator, std::int32_t state, bool active, std::uint8_t primary, std::uint8_t secondary, bool,
                                   AnimatorInvocation &invocation) noexcept {
    // Save entry state and requested animation IDs: state is WeaponState and active requests an animation restart.
    auto *subject = subjects_.find_animator(animator);
    if (subject == nullptr) {
        return;
    }

    invocation.subject = snapshot_subject(subject);
    invocation.record = presentation_sample(*subject, trace::PresentationKind::Combo);
    invocation.record.argument0 = static_cast<std::uint32_t>(state);
    invocation.record.argument1 = static_cast<std::uint32_t>(active);
    invocation.record.upper_clip = primary;
    invocation.record.lower_clip = secondary;
}

void Handler::finish_animator_state(const AnimatorInvocation &invocation) noexcept {
    // Emit the saved entry state after return, with requested animation IDs in upper_clip/lower_clip.
    if (invocation.subject && subjects_.live(*invocation.subject)) {
        write_subject(trace::RecordKind::PresentationFrame, invocation.record, *invocation.subject, trace::NativeCalled);
    }
}

void Handler::observe_transition_sound(void *sound, const void *, const void *, std::uint32_t, bool force) noexcept {
    const auto *transition = thread_state_.transition;
    if (transition == nullptr || !transition->subject || !subjects_.live(*transition->subject)) {
        return;
    }

    // Call observers share the typed sound site with its behavior owner, which may suppress playback.
    auto &subject = *transition->subject;
    const trace::AudioCueRecord record{
        .turn = turn_context(),
        .sound = native::pointer_id(sound),
        .scope = transition->scope,
        .force = static_cast<std::uint8_t>(force),
        .state = native::read<std::int32_t>(subject.weapon, native::kStateOffset),
    };

    write_subject(trace::RecordKind::AudioCue, record, subject, trace::Begin);
}

void Handler::begin_input_queue(const fc::CpuContext &context, InputInvocation &invocation) noexcept {
    if (context.ecx < native::kInputQueueOffset) {
        return;
    }

    // ECX addresses the embedded queue at entry; the displaced prologue and queue update have yet to execute.
    auto *weapon = reinterpret_cast<void *>(context.ecx - native::kInputQueueOffset);
    auto *subject = bind(weapon);
    if (subject == nullptr) {
        return;
    }

    invocation.weapon = weapon;
    invocation.subject = snapshot_subject(subject);
    invocation.queue = record_queue(*subject, 3, current_scope().operation_scope);
    const auto queue = context.ecx;
    invocation.record = {
        .turn = turn_context(),
        .state = static_cast<std::uint32_t>(native::read<std::int32_t>(weapon, native::kStateOffset)),
        .buttons = native::read<std::uint8_t>(fc::stack_pointer(context), sizeof(std::uint32_t)),
        .down_before = native::read<std::uint8_t>(queue, native::kInputQueueDownOffset),
        .queue_count = native::read<std::uint8_t>(queue, native::kInputQueueCountOffset),
        .queue_head = native::read<std::uint8_t>(queue, native::kInputQueueHeadOffset),
    };
    invocation.flags = turn_flags(invocation.record.turn);
}

void Handler::finish_input_queue(const fc::CpuContext &context, const InputInvocation &invocation) noexcept {
    if (!invocation.subject || !subjects_.live(*invocation.subject) || context.ecx < native::kInputQueueOffset ||
        invocation.weapon != reinterpret_cast<void *>(context.ecx - native::kInputQueueOffset)) {
        return;
    }

    // Sample held buttons after FC modifiers and before native InputQueue::Update runs.
    auto record = invocation.record;
    record.down_after = native::read<std::uint8_t>(context.ecx, native::kInputQueueDownOffset);

    write_subject(trace::RecordKind::InputDecision, record, *invocation.subject, invocation.flags);
    record_queue(*invocation.subject, 4, current_scope().operation_scope, 0.0F, &invocation.queue);
}

void Handler::begin_prediction_transition(const fc::CpuContext &context, PredictionInvocation &invocation) noexcept {
    // Ordinary Update keeps the weapon in EBX and proposed state in ESI before its displaced ExitState/EnterState calls.
    auto *weapon = reinterpret_cast<void *>(context.ebx);
    auto *subject = subjects_.find_weapon(weapon);
    if (subject == nullptr) {
        return;
    }

    invocation.weapon = weapon;
    record_queue(*subject, static_cast<std::uint8_t>(2U | ((context.eax & 0xFU) << 4U)), current_scope().operation_scope);
    invocation.subject = snapshot_subject(subject);
    invocation.instruction = context.eip;

    const auto state = native::read<std::int32_t>(weapon, native::kStateOffset);
    const auto state_time = native::read<float>(weapon, native::kStateTimeOffset);
    invocation.record = {
        .turn = turn_context(),
        .requested_state = static_cast<std::int32_t>(context.esi),
        .state_before = state,
        .state_after = state,
        .previous_state = native::read<std::int32_t>(weapon, native::kPreviousStateOffset),
        .state_time_before = state_time,
        .state_time_after = state_time,

        .kind = trace::TransitionKind::Prediction,
        .condition_scope = thread_state_.condition.parent_scope == current_scope().operation_scope ? thread_state_.condition.scope : 0,
        .candidate_ordinal = thread_state_.update ? thread_state_.update->condition_ordinal : 0,
        .condition = native::read<std::uint32_t>(context.ebp, -0x24),
        .eflags = context.eflags,
        .candidate_time = native::read<float>(context.ebp, -0x14),
        .winning_time = native::read<float>(context.ebp, -0x10),
    };
}

void Handler::finish_prediction_transition(const fc::CpuContext &context, const PredictionInvocation &invocation) noexcept {
    if (!invocation.subject || !subjects_.live(*invocation.subject) || invocation.weapon != reinterpret_cast<void *>(context.ebx)) {
        return;
    }

    // Prediction is this site's label; a changed EIP reports redirection before the transition calls execute.
    auto flags = static_cast<std::uint16_t>(trace::Prediction);
    if (invocation.instruction != context.eip) {
        flags = static_cast<std::uint16_t>(flags | trace::Suppressed);
    }

    write_subject(trace::RecordKind::Transition, invocation.record, *invocation.subject, flags);
}

void Handler::before_soldier_read(void *soldier, SoldierReadInvocation &invocation) noexcept {
    auto *subject = subjects_.find_soldier(soldier);
    invocation.subject = snapshot_subject(subject);
    if (subject == nullptr) {
        return;
    }
    invocation.turn = turn_context();
    invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
    invocation.boundary =
        record_local_state(*subject, trace::LocalStage::AuthorityRead, invocation.scope, invocation.turn, trace::Begin | trace::Authority);
}

void Handler::after_soldier_read(const SoldierReadInvocation &invocation) noexcept {
    // A nested bind or native retirement cannot relabel this read with a replacement subject.
    auto *subject = invocation.subject ? &*invocation.subject : nullptr;
    if (subject == nullptr || !subjects_.live(*subject)) {
        return;
    }

    record_local_state(*subject, trace::LocalStage::AuthorityRead, invocation.scope, invocation.turn,
                       trace::End | trace::Authority | trace::NativeCalled, false, &invocation.boundary);
}

trace::BoundaryRecord Handler::record_local_state(HeroSubject &subject, trace::LocalStage stage, std::uint32_t scope,
                                                  const trace::TurnContext &turn, std::uint16_t flags, bool result,
                                                  const trace::BoundaryRecord *previous) noexcept {
    if (subject.local_player == trace::kNoLocalPlayer || !subjects_.live(subject)) {
        return {};
    }

    if (settings_.capture == CaptureMode::Standard) {
        trace::BoundaryRecord record{.turn = turn,
                                     .position = native::read<std::array<float, 3>>(subject.soldier, native::kPositionOffset),
                                     .velocity = native::read<std::array<float, 3>>(subject.soldier, native::kVelocityOffset),
                                     .scope = scope,
                                     .mode = native::read<std::int32_t>(subject.weapon, native::kModeOffset),
                                     .mode_time = native::read<float>(subject.weapon, native::kModeTimeOffset),
                                     .combo = native::read<std::int32_t>(subject.weapon, native::kStateOffset),
                                     .combo_time = native::read<float>(subject.weapon, native::kStateTimeOffset),
                                     .input_time = native::read<float>(subject.weapon, native::kInputTimeOffset),
                                     .soldier_state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
                                     .notified_state_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset),
                                     .soldier_state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
                                     .stage = stage,
                                     .result = static_cast<std::uint8_t>(result),
                                     .state_flags = native::read<std::uint8_t>(subject.weapon, native::kStateFlagsOffset)};
        record.compared = static_cast<std::uint8_t>(previous != nullptr);
        record.changed = previous != nullptr &&
                         (previous->position != record.position || previous->velocity != record.velocity || previous->mode != record.mode ||
                          previous->mode_time != record.mode_time || previous->combo != record.combo ||
                          previous->combo_time != record.combo_time || previous->input_time != record.input_time ||
                          previous->soldier_state != record.soldier_state || previous->soldier_state_time != record.soldier_state_time ||
                          previous->notified_state_time != record.notified_state_time || previous->state_flags != record.state_flags);
        write_subject(trace::RecordKind::Boundary, record, subject, flags);
        return record;
    }

    const auto animation = presentation_sample(subject, trace::PresentationKind::Pose);
    trace::LocalStateRecord record{
        .turn = turn,
        .mode = native::read<std::int32_t>(subject.weapon, native::kModeOffset),
        .mode_time = native::read<float>(subject.weapon, native::kModeTimeOffset),
        .combo = native::read<std::int32_t>(subject.weapon, native::kStateOffset),
        .combo_time = animation.state_time,
        .upper_time = animation.upper_time,
        .lower_time = animation.lower_time,
        .upper_timer = animation.upper_timer,
        .lower_timer = animation.lower_timer,
        .upper_clip = animation.upper_clip,
        .lower_clip = animation.lower_clip,
        .animation_mode = animation.weapon_state,
        .soldier_state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
        .soldier_state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
        .sprint_trigger = native::read<std::uint32_t>(subject.owner, native::kSprintTriggerOffset),
        .secondary_fire_trigger = native::read<std::uint32_t>(subject.owner, native::kSecondaryTriggerOffset),
        .posture = native::read<std::uint32_t>(subject.weapon, native::kPostureOffset),
        .notified_state_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset),
        .scope = scope,
        .stage = stage,
        .result = static_cast<std::uint8_t>(result),
        .state_flags = native::read<std::uint8_t>(subject.weapon, native::kStateFlagsOffset),
        .animation_flags = animation.animation_flags,
    };
    native::copy_floats(record.position, subject.soldier, native::kPositionOffset);
    native::copy_floats(record.velocity, subject.soldier, native::kVelocityOffset);
    if (settings_.capture == CaptureMode::Full) {
        trace::LocalStateBasisRecord full{.state = record};
        for (std::size_t index{}; index < full.basis.size(); ++index) {
            full.basis[index] = native::read<std::array<float, 3>>(subject.soldier, 0xF0 + index * 0x10);
        }
        write_subject(trace::RecordKind::LocalStateBasis, full, subject, flags);
    } else {
        write_subject(trace::RecordKind::LocalState, record, subject, flags);
    }
    return {};
}

void Handler::record_display(HeroSubject &subject, trace::LocalStage stage, std::uint32_t scope, const float *matrix,
                             const void *source) noexcept {
    if (matrix == nullptr || !subjects_.live(subject)) {
        return;
    }
    if (settings_.capture == CaptureMode::Standard) {
        const auto position = native::read<std::array<float, 3>>(matrix, 0x30);
        const auto index = stage == trace::LocalStage::BodySubmission ? 0U : 1U;
        // The authoritative table retains cadence state across invocation-owned identity copies.
        auto *current = subjects_.find_weapon(subject.weapon);
        if (current == nullptr) {
            return;
        }
        const bool changed = !current->display_seen[index] || current->last_display_position[index] != position;
        current->last_display_position[index] = position;
        current->display_seen[index] = true;
        const trace::DisplayPositionRecord record{
            .turn = turn_context(),
            .position = position,
            .scope = scope,
            .source = native::pointer_id(source),
            .simulation_scope = current->simulation_scope,
            .pose_scope = current->pose_scope,
            .upper_clip = native::pointer_id(native::read<void *>(subject.animator, animator_.upper_clip)),
            .lower_clip = native::pointer_id(native::read<void *>(subject.animator, animator_.lower_clip)),
            .action = native::read<std::uint32_t>(subject.animator, animator_.action),
            .stage = stage,
            .changed = static_cast<std::uint8_t>(changed)};
        write_subject(trace::RecordKind::DisplayPosition, record, subject,
                      stage == trace::LocalStage::BodySubmission ? trace::Begin : trace::End | trace::NativeCalled);
        return;
    }
    trace::DisplayMatrixRecord record{.turn = turn_context(), .scope = scope, .source = native::pointer_id(source), .stage = stage};
    native::copy_floats(record.matrix, matrix, 0);
    write_subject(trace::RecordKind::DisplayMatrix, record, subject,
                  stage == trace::LocalStage::BodySubmission ? trace::Begin : trace::End | trace::NativeCalled);
}

trace::PresentationFrameRecord Handler::presentation_sample(const HeroSubject &subject, trace::PresentationKind kind) const noexcept {
    auto *animator = subject.animator;

    // SetupPose can reuse a cached pose or choose another delta; frame_delta samples the outer game-loop global.
    const auto &offsets = animator_;
    return {
        .turn = turn_context(),
        .state_time = native::read<float>(subject.weapon, native::kStateTimeOffset),
        .frame_delta = frame_delta_ != nullptr ? *frame_delta_ : 0.0F,
        .extra_delta = native::read<float>(animator, offsets.extra_delta),
        .upper_time = native::read<float>(animator, offsets.upper_time),
        .lower_time = native::read<float>(animator, offsets.lower_time),
        .upper_timer = native::read<float>(animator, offsets.upper_timer),
        .lower_timer = native::read<float>(animator, offsets.lower_timer),
        .upper_blend = native::read<float>(animator, offsets.upper_blend),
        .lower_blend = native::read<float>(animator, offsets.lower_blend),
        .animation_rate = native::read<float>(animator, offsets.animation_rate),
        .upper_clip = native::pointer_id(native::read<const void *>(animator, offsets.upper_clip)),
        .lower_clip = native::pointer_id(native::read<const void *>(animator, offsets.lower_clip)),
        .action = native::read<std::uint32_t>(animator, offsets.action),
        .weapon_animation = native::read<std::uint32_t>(animator, offsets.weapon_animation),
        .weapon_state = native::read<std::uint32_t>(animator, offsets.weapon_state),
        .state = static_cast<std::int8_t>(native::read<std::int32_t>(subject.weapon, native::kStateOffset)),
        .previous_state = static_cast<std::int8_t>(native::read<std::int32_t>(subject.weapon, native::kPreviousStateOffset)),
        .kind = kind,
        .status = static_cast<std::uint8_t>(
            (native::read<std::uint8_t>(animator, offsets.upper_finished) != 0 ? trace::UpperAnimationFinished : 0) |
            (native::read<std::uint8_t>(animator, offsets.lower_finished) != 0 ? trace::LowerAnimationFinished : 0)),
        .animation_flags = native::read<std::uint8_t>(animator, offsets.animation_flags),
    };
}

HeroScope Handler::weapon_scope(void *weapon) noexcept {
    const AmbientErrors errors;
    if (weapon == nullptr || subject_thread_.load(std::memory_order_relaxed) != GetCurrentThreadId()) {
        return {};
    }
    auto *subject = subjects_.find_weapon(weapon);
    if (subject == nullptr) {
        return {};
    }
    auto scope = current_scope();
    if (scope.subject == subject->id && scope.generation == subject->generation) {
        return scope;
    }
    return {.subject = subject->id, .generation = subject->generation, .weapon = native::pointer_id(subject->weapon)};
}

HeroScope Handler::current_scope() const noexcept {
    const HeroSubject *subject{};
    std::uint32_t scope{};
    std::uint8_t phase{};
    if (const auto *contact = thread_state_.contact; contact != nullptr && contact->subject) {
        subject = &*contact->subject;
        scope = contact->scope;
        phase = 6;
    } else if (const auto *transition = thread_state_.transition; transition != nullptr && transition->subject) {
        subject = &*transition->subject;
        scope = transition->scope;
        phase = 3;
    } else if (const auto *network = thread_state_.network_state; network != nullptr && network->subject) {
        subject = &*network->subject;
        scope = network->record.scope;
        phase = 2;
    } else if (const auto *combat = thread_state_.combat; combat != nullptr && combat->subject) {
        subject = &*combat->subject;
        scope = combat->scope;
        phase = 5;
    } else if (const auto *update = thread_state_.update; update != nullptr && update->subject) {
        subject = &*update->subject;
        scope = update->scope;
        phase = 1;
    }
    if (subject == nullptr) {
        if (const auto *render = thread_state_.render; render != nullptr && render->subject) {
            subject = &*render->subject;
            scope = render->scope;
            phase = 7;
        } else if (const auto *simulation = thread_state_.simulation; simulation != nullptr && simulation->subject) {
            subject = &*simulation->subject;
            scope = simulation->scope;
            phase = 4;
        }
    }
    if (subject == nullptr) {
        return {};
    }

    const auto turn = turn_context();
    return {.subject = subject->id,
            .generation = subject->generation,
            .weapon = native::pointer_id(subject->weapon),
            .read_scope = thread_state_.read_scope,
            .operation_scope = scope,
            .prediction_turn = turn.predict_turn,
            .update_turn = turn.update_turn,
            .acknowledged_turn = turn.acknowledged_turn,
            .phase = phase,
            .host_turn = turn.host_turn};
}

void Handler::observe_owner_decision(void *weapon, const OwnerDecision *decision) noexcept {
    const AmbientErrors errors;
    if (decision == nullptr) {
        return;
    }
    if (decision->kind == CauseKind::NativeBypass) {
        if (auto *update = thread_state_.update; update != nullptr && update->weapon == weapon) {
            const auto bit = static_cast<std::uint8_t>(1U << decision->stage);
            update->record.bypass_known |= bit;
            if (decision->result != 0) {
                update->record.bypass_applied |= bit;
            }
        }
        if (settings_.capture == CaptureMode::Standard && decision->result == 0) {
            return;
        }
    }
    if (settings_.capture == CaptureMode::Standard && decision->kind == CauseKind::InputTiming && decision->result == 0) {
        return;
    }
    // Other threads and early reports retain explicit object identity without touching the native subject table.
    auto *subject = subject_thread_.load(std::memory_order_relaxed) == GetCurrentThreadId() ? subjects_.find_weapon(weapon) : nullptr;
    {
        const auto active = current_scope();
        const auto parent =
            subject != nullptr && active.subject == subject->id && active.generation == subject->generation && active.operation_scope != 0
                ? active.operation_scope
                : thread_state_.predict_scope;
        trace::CauseRecord record{.turn = turn_context(),
                                  .decision = *decision,
                                  .scope = decision->occurrence != 0 ? decision->occurrence
                                           : parent != 0             ? parent
                                                                     : next_scope_.fetch_add(1, std::memory_order_relaxed) + 1,
                                  .parent_scope = parent};
        record.weapon = native::pointer_id(weapon);
        if (settings_.capture == CaptureMode::Full) {
            if (subject != nullptr) {
                write_subject(trace::RecordKind::Cause, record, *subject);
            } else {
                write(trace::RecordKind::Cause, record, 0);
            }
        } else {
            // Counts describe exact operation-specific prefixes; Audio retains its complete occurrence token.
            std::uint8_t values{};
            std::uint8_t words{};
            switch (decision->kind) {
            case CauseKind::Controller:
                words = 2;
                break;
            case CauseKind::Trigger:
                values = 1;
                words = 7;
                break;
            case CauseKind::TriggerReset:
                values = 1;
                words = 4;
                break;
            case CauseKind::SprintRoute:
                words = 6;
                break;
            case CauseKind::Controls:
                words = 4;
                break;
            case CauseKind::InstalledAnimation:
                values = 5;
                words = 10;
                break;
            case CauseKind::Replay:
                values = decision->stage == 6 ? 5 : 4;
                words = decision->stage == 6 ? 12 : 7;
                break;
            case CauseKind::PresentationLookup:
                values = 3;
                words = 12;
                break;
            case CauseKind::Audio:
                words = 12;
                break;
            case CauseKind::AnimationRequest:
            case CauseKind::AnimationLane:
                words = 12;
                break;
            case CauseKind::ContactIsolation:
                values = 2;
                words = 4;
                break;
            case CauseKind::NativeBypass:
                words = 2;
                break;
            case CauseKind::JumpReset:
                words = decision->stage == 0 ? 5 : decision->stage == 1 ? 3 : 4;
                break;
            case CauseKind::AutomaticTransition:
                values = 4;
                words = 7;
                break;
            case CauseKind::MovementLock:
                values = 1;
                words = 3;
                break;
            case CauseKind::LiveGait:
                words = 7;
                break;
            case CauseKind::JumpClock:
                values = 3;
                words = 6;
                break;
            case CauseKind::AmbientAssignment:
                values = 3;
                words = 8;
                break;
            case CauseKind::InputTiming:
                values = 5;
                words = 3;
                break;
            case CauseKind::ConditionTime:
                values = 2;
                words = 3;
                break;
            default:
                values = 8;
                words = 12;
                break;
            }
            if (subject == nullptr || announce(*subject)) {
                const auto identity = subject == nullptr                               ? trace::None
                                      : subject->local_player == trace::kNoLocalPlayer ? trace::Remote
                                                                                       : trace::Local;
                const trace::Record<trace::CauseSummaryRecord> prefix{
                    .header = record_header(trace::RecordKind::CauseSummary, subject != nullptr ? subject->id : 0, identity),
                    .payload = {.turn = record.turn,
                                .scope = record.scope,
                                .parent_scope = record.parent_scope,
                                .weapon = record.weapon,
                                .occurrence = decision->occurrence,
                                .kind = decision->kind,
                                .stage = decision->stage,
                                .result = decision->result,
                                .reason = decision->reason,
                                .value_count = values,
                                .word_count = words}};
                std::array<std::byte, sizeof(prefix) + sizeof(OwnerDecision::values) + sizeof(OwnerDecision::words)> bytes{};
                std::memcpy(bytes.data(), &prefix, sizeof(prefix));
                std::memcpy(bytes.data() + sizeof(prefix), decision->values, values * sizeof(float));
                std::memcpy(bytes.data() + sizeof(prefix) + values * sizeof(float), decision->words, words * sizeof(std::uint32_t));
                trace_.write(std::span<const std::byte>{bytes}.first(sizeof(prefix) + (values + words) * 4U));
            }
        }
        if (subject != nullptr && settings_.capture != CaptureMode::Standard && decision->kind == CauseKind::Replay &&
            decision->stage <= 6) {
            record_queue(*subject, static_cast<std::uint8_t>(16 + decision->stage), record.scope);
        }
    }
}

void Handler::observe_history_decision(void *weapon, const HistoryDecision *decision) noexcept {
    const AmbientErrors errors;
    if (decision == nullptr) {
        return;
    }
    const trace::HistoryRecord record{
        .turn = turn_context(), .decision = *decision, .scope = current_scope().operation_scope, .weapon = native::pointer_id(weapon)};
    if (auto *subject = subjects_.find_weapon(weapon)) {
        write_subject(trace::RecordKind::History, record, *subject);
    } else {
        write(trace::RecordKind::History, record, 0);
    }
}

void Handler::observe_physics_decision(void *object, std::uint8_t soldier, const PhysicsDecision *decision) noexcept {
    const AmbientErrors errors;
    if (decision == nullptr) {
        return;
    }
    const trace::PhysicsRecord record{.turn = turn_context(),
                                      .decision = *decision,
                                      .scope = current_scope().operation_scope,
                                      .object = native::pointer_id(object),
                                      .soldier = soldier};
    auto *subject = soldier != 0 ? subjects_.find_soldier(object) : subjects_.find_weapon(object);
    if (subject != nullptr) {
        write_subject(trace::RecordKind::Physics, record, *subject);
    } else {
        write(trace::RecordKind::Physics, record, 0);
    }
}

void Handler::observe_selection_decision(void *soldier, const SelectionDecision *decision) noexcept {
    const AmbientErrors errors;
    if (decision == nullptr) {
        return;
    }
    const trace::SelectionRecord record{
        .turn = turn_context(), .decision = *decision, .scope = current_scope().operation_scope, .soldier = native::pointer_id(soldier)};
    if (auto *subject = subjects_.find_soldier(soldier)) {
        write_subject(trace::RecordKind::Selection, record, *subject);
    } else {
        write(trace::RecordKind::Selection, record, 0);
    }
}

trace::NativeQueueRecord Handler::record_queue(HeroSubject &subject, std::uint8_t stage, std::uint32_t scope, float threshold,
                                               const trace::NativeQueueRecord *previous) noexcept {
    if (settings_.capture == CaptureMode::Standard) {
        return {};
    }
    const auto *queue = static_cast<const std::byte *>(subject.weapon) + native::kInputQueueOffset;
    trace::NativeQueueRecord record{.turn = turn_context(),
                                    .clock = native::read<float>(subject.weapon, native::kInputTimeOffset),
                                    .threshold = threshold,
                                    .scope = scope,
                                    .count = native::read<std::uint8_t>(queue, native::kInputQueueCountOffset),
                                    .head = native::read<std::uint8_t>(queue, native::kInputQueueHeadOffset),
                                    .down = native::read<std::uint8_t>(queue, native::kInputQueueDownOffset),
                                    .stage = stage};
    for (std::size_t index{}; index < std::min<std::size_t>(record.count, 8); ++index) {
        const auto row = native::read<std::array<std::uint32_t, 2>>(queue, ((record.head + index) & 7U) * 8U);
        record.rows[index * 2] = row[0];
        record.rows[index * 2 + 1] = row[1];
    }
    record.unchanged = previous != nullptr && previous->clock == record.clock && previous->count == record.count &&
                       previous->head == record.head && previous->down == record.down && previous->rows == record.rows;
    if (announce(subject)) {
        const auto identity = subject.local_player == trace::kNoLocalPlayer ? trace::Remote : trace::Local;
        const trace::Record<trace::NativeQueueRecord> output{.header = record_header(trace::RecordKind::NativeQueue, subject.id, identity),
                                                             .payload = record};
        const auto rows = record.unchanged != 0 ? 0U : std::min<std::size_t>(record.count, 8);
        trace_.write(
            std::as_bytes(std::span{&output, 1}).first(sizeof(trace::RecordHeader) + offsetof(trace::NativeQueueRecord, rows) + rows * 8U));
    }
    return record;
}

void Handler::record_dispatch(HeroSubject &subject, std::uint8_t stage, std::uint32_t scope) noexcept {
    if (settings_.capture == CaptureMode::Standard) {
        return;
    }
    const trace::DispatchRecord record{.turn = turn_context(),
                                       .mode = native::read<std::int32_t>(subject.weapon, native::kModeOffset),
                                       .mode_time = native::read<float>(subject.weapon, native::kModeTimeOffset),
                                       .combo = native::read<std::int32_t>(subject.weapon, native::kStateOffset),
                                       .combo_time = native::read<float>(subject.weapon, native::kStateTimeOffset),
                                       .input_time = native::read<float>(subject.weapon, native::kInputTimeOffset),
                                       .movement_blend = native::read<float>(subject.weapon, 0x19C),
                                       .energy = native::read<float>(subject.soldier, native::kEnergyOffset),
                                       .energy_flags = native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset),
                                       .posture = native::read<std::uint32_t>(subject.weapon, native::kPostureOffset),
                                       .soldier_state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
                                       .soldier_state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
                                       .sprint_trigger = native::read<std::uint32_t>(subject.owner, native::kSprintTriggerOffset),
                                       .secondary_fire_trigger =
                                           native::read<std::uint32_t>(subject.owner, native::kSecondaryTriggerOffset),
                                       .notified_state_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset),
                                       .scope = scope,
                                       .stage = stage,
                                       .state_flags = native::read<std::uint8_t>(subject.weapon, native::kStateFlagsOffset)};
    write_subject(trace::RecordKind::Dispatch, record, subject);
}

void Handler::plan_native_details(fc::Plan &plan, const DetailLayout &layout) {
    // Native registration already reads this C string; copy at most eighty bytes, stopping at its terminator.
    plan.observe_instruction(layout.asset_name, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (context.ecx == 0 || context.edx == 0) {
                                     return;
                                 }
                                 trace::AssetNameRecord record{.hash = context.ecx, .pointer = context.edx};
                                 while (record.length < record.name.size()) {
                                     const auto character = native::read<char>(context.edx, record.length);
                                     if (character == '\0') {
                                         break;
                                     }
                                     record.name[record.length++] = character;
                                 }
                                 record.truncated = static_cast<std::uint8_t>(record.length == record.name.size());
                                 const auto duplicate = std::ranges::find_if(asset_names_, [&](const auto &known) {
                                     return known.hash == record.hash && known.length == record.length &&
                                            known.truncated == record.truncated && known.name == record.name;
                                 });
                                 if (duplicate == asset_names_.end() && write(trace::RecordKind::AssetName, record, 0)) {
                                     asset_names_.push_back(record);
                                 }
                             }));
    plan_simulation(plan, layout);
    plan_causes(plan, layout);
    plan_presentation(plan, layout);
    if (settings_.capture == CaptureMode::Standard) {
        return;
    }

    plan_gameplay_decisions(plan, layout);
    plan_combat_decisions(plan, layout);
    if (settings_.capture == CaptureMode::Full) {
        plan_energy_codec(plan, layout);
        for (std::size_t index{}; index < layout.orientation.size(); ++index) {
            plan.observe_instruction(
                layout.orientation[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                    const auto soldier = index == 0 ? context.esi : context.edi;
                    if (auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(soldier))) {
                        const auto stack = fc::stack_pointer(context);
                        const auto matrix = index == 0 ? native::read<std::uint32_t>(stack, 0x14) : stack + 0x80;
                        record_orientation(
                            *subject, reinterpret_cast<void *>(matrix), index == 0 ? reinterpret_cast<void *>(stack + 0x20) : nullptr,
                            reinterpret_cast<void *>(index == 0 ? soldier + native::kVelocityOffset : stack + 0x24),
                            index == 0 ? context.edi : context.esi, current_scope().operation_scope, static_cast<std::uint8_t>(index));
                    }
                }));
        }
        plan.check(layout.gravity_sign_operand.rva, 4, layout.gravity_sign_operand.evidence);
        plan.observe_instruction(layout.generic_timer_write, fc::before([this](const fc::CpuContext &context) noexcept {
                                     if (auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.esi))) {
                                         const trace::GenericTimerRecord record{
                                             .turn = turn_context(),
                                             .scope = current_scope().operation_scope,
                                             .stream = context.ebx,
                                             .mode = native::read<std::int32_t>(subject->weapon, native::kModeOffset),
                                             .timer_bits = native::read<std::uint32_t>(fc::stack_pointer(context)),
                                             .bit_offset = network_diagnostics_ready_.load(std::memory_order_acquire)
                                                               ? network_diagnostics_.codec_position(context.ebx, 1)
                                                               : UINT32_MAX,
                                             .stage = 2};
                                         write_subject(trace::RecordKind::GenericTimer, record, *subject, trace::Begin);
                                     }
                                 }));
    }

    // Instruction observers retain the evaluator's custom XMM calling convention and native return paths.
    plan.observe_instruction(layout.condition_entry, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.ecx));
                                 thread_state_.condition_subject = snapshot_subject(subject);
                                 thread_state_.predicate.kind = 0xFF;
                                 if (subject == nullptr) {
                                     return;
                                 }
                                 const auto stack = fc::stack_pointer(context);
                                 const auto condition = native::read<std::uint32_t>(stack, 4);
                                 thread_state_.condition = {
                                     .turn = turn_context(),

                                     .start = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[2].low)),
                                     .end = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low)),
                                     .input_clock = native::read<float>(subject->weapon, native::kInputTimeOffset),
                                     .scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1,
                                     .condition = condition,
                                     .parent_scope = current_scope().operation_scope,
                                     .candidate_ordinal = thread_state_.update ? ++thread_state_.update->condition_ordinal : 0,
                                     .posture = native::read<std::uint32_t>(subject->weapon, 0xAC),
                                     .soldier_state = native::read<std::int32_t>(subject->soldier, native::kLocomotionStateOffset),
                                     .caller = native::read<std::uint32_t>(stack)};
                                 if (settings_.capture == CaptureMode::Full) {
                                     thread_state_.condition.row = native::read<std::array<std::uint32_t, 5>>(condition);
                                     auto decision = soldier_decision(*subject, 6);
                                     decision.scope = thread_state_.condition.scope;
                                     decision.caller = thread_state_.condition.caller;
                                     write_soldier_decision(*subject, decision);
                                 }
                             }));
    for (std::size_t index{}; index < layout.condition_returns.size(); ++index) {
        plan.observe_instruction(layout.condition_returns[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                     auto &subject = thread_state_.condition_subject;
                                     if (!subject || !subjects_.live(*subject)) {
                                         return;
                                     }
                                     auto record = thread_state_.condition;
                                     record.result = static_cast<std::uint8_t>((context.eax & 0xFFU) != 0);
                                     record.stage = static_cast<std::uint8_t>(0x10 + index);
                                     // Held-button testing is the last predicate; its result equals this evaluator's return.
                                     if (thread_state_.predicate.kind == 4) {
                                         auto predicate = thread_state_.predicate;
                                         predicate.stage = 1;
                                         predicate.result = record.result;
                                         write_subject(trace::RecordKind::Predicate, predicate, *subject, trace::End | trace::NativeCalled);
                                     }
                                     // Failed predicates leave the caller's output uninitialized; read it only for native success.
                                     if (record.result != 0) {
                                         record.output = native::read<float>(native::read<std::uint32_t>(context.ebp, 0xC));
                                     }
                                     if (settings_.capture == CaptureMode::Full) {
                                         write_subject(trace::RecordKind::Condition, record, *subject, trace::End | trace::NativeCalled);
                                     } else {
                                         const trace::ConditionOutcomeRecord outcome{.turn = record.turn,
                                                                                     .scope = record.scope,
                                                                                     .parent_scope = record.parent_scope,
                                                                                     .condition = record.condition,
                                                                                     .candidate_ordinal = record.candidate_ordinal,
                                                                                     .start = record.start,
                                                                                     .end = record.end,
                                                                                     .input_clock = record.input_clock,
                                                                                     .output = record.output,
                                                                                     .result = record.result,
                                                                                     .stage = record.stage};
                                         write_subject(trace::RecordKind::ConditionOutcome, outcome, *subject,
                                                       trace::End | trace::NativeCalled);
                                     }
                                     subject.reset();
                                 }));
    }

    if (settings_.capture == CaptureMode::Full) {
        // Keep actual call operands until the corresponding native AL result; never invoke a predicate twice.
        for (std::size_t index{}; index < layout.predicate_operands.size(); ++index) {
            plan.observe_instruction(layout.predicate_operands[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         auto &subject = thread_state_.condition_subject;
                                         if (!subject || !subjects_.live(*subject)) {
                                             return;
                                         }
                                         auto &record = thread_state_.predicate;
                                         record = {.turn = thread_state_.condition.turn,
                                                   .values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                                                              std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[2].low)),
                                                              std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low))},
                                                   .scope = thread_state_.condition.scope,
                                                   .condition = thread_state_.condition.condition,
                                                   .kind = static_cast<std::uint8_t>(index),
                                                   .stage = 0};
                                         if (index == 0) {
                                             record.words[0] = native::read<std::uint32_t>(fc::stack_pointer(context));
                                         } else if (index == 4) {
                                             record.words = native::read<std::array<std::uint32_t, 2>>(fc::stack_pointer(context));
                                         }
                                     }));
            if (index == 4) {
                continue;
            }
            plan.observe_instruction(layout.predicate_results[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         auto &subject = thread_state_.condition_subject;
                                         if (!subject || !subjects_.live(*subject) || thread_state_.predicate.kind != index) {
                                             return;
                                         }
                                         auto record = thread_state_.predicate;
                                         record.stage = 1;
                                         record.result = static_cast<std::uint8_t>((context.eax & 0xFFU) != 0);
                                         write_subject(trace::RecordKind::Predicate, record, *subject, trace::End | trace::NativeCalled);
                                     }));
        }
        for (std::size_t index{}; index < layout.time_comparisons.size(); ++index) {
            plan.observe_instruction(layout.time_comparisons[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         auto &subject = thread_state_.condition_subject;
                                         if (!subject || !subjects_.live(*subject)) {
                                             return;
                                         }
                                         const trace::PredicateRecord record{
                                             .turn = thread_state_.condition.turn,
                                             .values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low)),
                                                        std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low))},
                                             .scope = thread_state_.condition.scope,
                                             .condition = thread_state_.condition.condition,
                                             .eflags = context.eflags,
                                             .kind = static_cast<std::uint8_t>(5 + index),
                                             .result = static_cast<std::uint8_t>((context.eflags & 0x41U) == 0),
                                             .stage = 2};
                                         write_subject(trace::RecordKind::Predicate, record, *subject);
                                     }));
        }
    }
    plan.observe_instruction(layout.native_queue_ready, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.ebx))) {
                                     record_queue(*subject, 0, current_scope().operation_scope);
                                 }
                             }));
    plan.observe_instruction(layout.consume_input, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.ebx))) {
                                     record_queue(*subject, 1, current_scope().operation_scope,
                                                  std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)));
                                 }
                             }));

    if (role_ == fc::Role::Client) {
        if (settings_.capture == CaptureMode::Full) {
            for (std::size_t index{}; index < layout.generic_timer.size(); ++index) {
                plan.observe_instruction(layout.generic_timer[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                             if (auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.esi))) {
                                                 const trace::GenericTimerRecord record{
                                                     .turn = turn_context(),
                                                     .scope = thread_state_.read_scope,
                                                     .stream = context.ebx,
                                                     .mode = native::read<std::int32_t>(subject->weapon, native::kModeOffset),
                                                     .timer_bits = native::read<std::uint32_t>(subject->weapon, native::kModeTimeOffset),
                                                     .bit_offset = network_diagnostics_ready_.load(std::memory_order_acquire)
                                                                       ? network_diagnostics_.codec_position(context.ebx, 0)
                                                                       : UINT32_MAX,
                                                     .stage = static_cast<std::uint8_t>(index)};
                                                 write_subject(trace::RecordKind::GenericTimer, record, *subject,
                                                               index == 0 ? trace::Begin : trace::End | trace::NativeCalled);
                                             }
                                         }));
            }
        }
        plan.observe_instruction(layout.base_read_return, fc::before([this](const fc::CpuContext &context) noexcept {
                                     if (auto *subject = subjects_.find_weapon(reinterpret_cast<void *>(context.edi))) {
                                         record_dispatch(*subject, 4, thread_state_.read_scope);
                                     }
                                 }));
        if (settings_.capture == CaptureMode::Full) {
            // Selection receives delta in XMM1. Its final SetAnim call has already chosen the start phase and retained delta.
            for (std::size_t lane{}; lane < layout.animation_entry.size(); ++lane) {
                plan.observe_instruction(layout.animation_selection[lane], fc::before([this, lane](const fc::CpuContext &context) noexcept {
                                             if (auto *subject = subjects_.find_animator(reinterpret_cast<void *>(context.edi))) {
                                                 auto record = thread_state_.animation_selection;
                                                 record.selected_phase = native::read<float>(context.ebp, 8);
                                                 record.effective_delta = native::read<float>(context.ebp, -4);
                                                 if (record.lane == lane && thread_state_.animation_selection_subject == subject->id) {
                                                     write_subject(trace::RecordKind::AnimationSelection, record, *subject);
                                                 }
                                             }
                                         }));
            }

            plan.observe<AnimationInvocation>(
                layout.animation_advance, fc::before([this](void *instance, float delta, AnimationInvocation &invocation) noexcept {
                    const auto address = reinterpret_cast<std::uintptr_t>(instance);
                    auto *subject = address >= 0xC64 ? subjects_.find_animator(reinterpret_cast<void *>(address - 0xC64)) : nullptr;
                    std::uint8_t lane{};
                    if (subject == nullptr && address >= 0x1614) {
                        subject = subjects_.find_animator(reinterpret_cast<void *>(address - 0x1614));
                        lane = 1;
                    }
                    invocation.subject = snapshot_subject(subject);
                    if (subject == nullptr) {
                        return;
                    }
                    const auto *clip = native::read<void *>(instance, 0x980);
                    invocation.record = {.turn = turn_context(),
                                         .scope = thread_state_.pose_scope,
                                         .instance = native::pointer_id(instance),
                                         .clip = native::pointer_id(clip),
                                         .delta = delta,
                                         .rate = native::read<float>(instance, 0x998),
                                         .phase_before = native::read<float>(instance, 0x99C),
                                         .frames = native::read<std::uint16_t>(clip, 8),
                                         .flags_before = native::read<std::array<std::uint8_t, 3>>(instance, 0x9AA),
                                         .lane = lane};
                }),
                fc::after([this](void *instance, float, const AnimationInvocation &invocation) noexcept {
                    if (invocation.subject && subjects_.live(*invocation.subject)) {
                        auto record = invocation.record;
                        record.phase_after = native::read<float>(instance, 0x99C);
                        record.flags_after = native::read<std::array<std::uint8_t, 3>>(instance, 0x9AA);
                        write_subject(trace::RecordKind::AnimationAdvance, record, *invocation.subject, trace::End | trace::NativeCalled);
                    }
                }));
            for (std::size_t index{}; index < layout.pose_clock.size(); ++index) {
                plan.observe_instruction(layout.pose_clock[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                             if (auto *subject = subjects_.find_animator(reinterpret_cast<void *>(context.edi))) {
                                                 const trace::AnimationClockRecord record{
                                                     .turn = turn_context(),
                                                     .scope = thread_state_.pose_scope,
                                                     .cached_frame = native::read<std::int32_t>(subject->animator, 0xC4),
                                                     .current_frame = index == 0 ? static_cast<std::int32_t>(context.eax) : -1,
                                                     .cached_tick = native::read<std::uint32_t>(subject->animator, 0xC8),
                                                     .current_tick = index == 3 ? static_cast<std::uint32_t>(context.eax) : 0xFFFFFFFFU,
                                                     .delta = native::read<float>(fc::stack_pointer(context), 0xC),
                                                     .accumulated = native::read<float>(subject->animator, 0xCC),
                                                     .flags = native::read<std::uint16_t>(subject->animator, 0x201A),
                                                     .stage = static_cast<std::uint8_t>(index)};
                                                 write_subject(trace::RecordKind::AnimationClock, record, *subject);
                                             }
                                         }));
            }
        }
    }

    // The branch samples the patched threshold and native comparison flags; the cost site precedes EnergyBar admission.
    plan.observe_instruction(layout.jump_compare, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const PhysicsDecision decision{
                                     .values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                                                std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low)),
                                                native::read<float>(context.eax, 0x69C)},
                                     .words = {context.eflags},
                                     .operation = PhysicsOperation::JumpCost,
                                     .reason = 0,
                                     .result = static_cast<std::uint8_t>((context.eflags & 0x41U) != 0)};
                                 observe_physics_decision(reinterpret_cast<void *>(context.edx), 1, &decision);
                             }));
    plan.observe_instruction(layout.jump_cost, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const PhysicsDecision decision{
                                     .values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low))},
                                     .operation = PhysicsOperation::JumpCost,
                                     .reason = 1};
                                 observe_physics_decision(reinterpret_cast<void *>(context.edx), 1, &decision);
                             }));
    for (std::size_t index{}; index < layout.jump_results.size(); ++index) {
        plan.observe_instruction(layout.jump_results[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                     const PhysicsDecision decision{
                                         .values = {native::read<float>(context.esi, 4), native::read<float>(context.ebp, -4)},
                                         .words = {native::read<std::uint32_t>(context.esi, 8)},
                                         .operation = PhysicsOperation::JumpCost,
                                         .reason = static_cast<std::uint8_t>(index + 2),
                                         .result = static_cast<std::uint8_t>((context.eax & 0xFFU) != 0)};
                                     observe_physics_decision(reinterpret_cast<void *>(context.esi - 0xA10), 1, &decision);
                                 }));
    }
}

void Handler::begin_animation_selection(const fc::CpuContext &context, std::size_t lane) noexcept {
    thread_state_.selector_subjects[lane] = snapshot_subject(subjects_.find_animator(reinterpret_cast<void *>(context.ecx)));
    thread_state_.selector_stack[lane] = static_cast<std::uint32_t>(fc::stack_pointer(context));
    thread_state_.selector_scope[lane] = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
    thread_state_.selector_descriptor[lane] = native::read<std::uint32_t>(fc::stack_pointer(context), 4);
    thread_state_.selector_caller[lane] = native::read<std::uint32_t>(fc::stack_pointer(context));
    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    thread_state_.animation_selection_subject = 0;
    if (auto &entry = thread_state_.selector_subjects[lane]; entry) {
        auto *subject = &*entry;
        thread_state_.animation_selection_subject = subject->id;
        const auto descriptor = native::read<std::uint32_t>(fc::stack_pointer(context), 4);
        const auto metadata = native::read<void *>(descriptor, 8);
        const auto old_metadata = native::read<void *>(native::read<void *>(subject->animator, lane == 0 ? 0x1FC8 : 0x1FC4), 8);
        const auto opposite_metadata = native::read<void *>(native::read<void *>(subject->animator, lane == 0 ? 0x1FC4 : 0x1FC8), 8);
        thread_state_.animation_selection = {
            .turn = turn_context(),
            .descriptor = {native::read<std::uint32_t>(descriptor), native::read<std::uint32_t>(descriptor, 4),
                           native::read<std::uint32_t>(metadata)},
            .scope = thread_state_.pose_scope,
            .old_clip = native::pointer_id(native::read<void *>(subject->animator, lane == 0 ? 0x15E4 : 0x1F94)),
            .caller = native::read<std::uint32_t>(fc::stack_pointer(context)),
            .old_phase = native::read<float>(subject->animator, lane == 0 ? 0x1600 : 0x1FB0),
            .requested_delta = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
            .old_metadata = native::read<std::uint32_t>(old_metadata),
            .opposite_metadata = native::read<std::uint32_t>(opposite_metadata),
            .opposite_clip = native::pointer_id(native::read<void *>(subject->animator, lane == 0 ? 0x1F94 : 0x15E4)),
            .opposite_phase = native::read<float>(subject->animator, lane == 0 ? 0x1FB0 : 0x1600),
            .old_frames = native::read<std::uint16_t>(native::read<void *>(subject->animator, lane == 0 ? 0x15E4 : 0x1F94), 8),
            .requested_frames = native::read<std::uint16_t>(native::read<void *>(descriptor), 8),
            .flags_before = native::read<std::uint16_t>(subject->animator, 0x201A),
            .lane = static_cast<std::uint8_t>(lane),
            .metadata_valid = static_cast<std::uint8_t>((metadata != nullptr ? 1U : 0U) | (old_metadata != nullptr ? 2U : 0U) |
                                                        (opposite_metadata != nullptr ? 4U : 0U)),
            .pending_flags = native::read<std::uint16_t>(subject->animator, 0x201C),
            .action = native::read<std::uint32_t>(subject->animator, 0x1FEC),
            .leg_angle = native::read<float>(subject->animator, 0x2000)};
    }
}

void Handler::record_action(HeroSubject &subject, const PresentationInvocation &invocation, std::uint16_t flags) noexcept {
    const auto *animator = subject.animator;
    const trace::ActionFrameRecord record{.turn = turn_context(),
                                          .scope = invocation.scope,
                                          .parent_scope =
                                              thread_state_.render ? thread_state_.render->scope : current_scope().operation_scope,
                                          .pose_scope = thread_state_.pose_scope,
                                          .caller = invocation.previous_scope,
                                          .requested = invocation.argument0,
                                          .argument = invocation.argument1,
                                          .current = native::read<std::uint32_t>(animator, 0x70),
                                          .action = native::read<std::uint32_t>(animator, 0x1FEC),
                                          .previous = native::read<std::uint32_t>(animator, 0x1FE8),
                                          .weapon_animation = native::read<std::uint32_t>(animator, 0x2014),
                                          .upper_clip = native::pointer_id(native::read<void *>(animator, animator_.upper_clip)),
                                          .lower_clip = native::pointer_id(native::read<void *>(animator, animator_.lower_clip)),
                                          .timer = native::read<float>(animator, 0x1FF0),
                                          .upper_phase = native::read<float>(animator, animator_.upper_time),
                                          .lower_phase = native::read<float>(animator, animator_.lower_time),
                                          .requested_position = invocation.requested_position,
                                          .flags = native::read<std::uint16_t>(animator, 0x201A),
                                          .pending_flags = native::read<std::uint16_t>(animator, 0x201C)};
    write_subject(trace::RecordKind::ActionFrame, record, subject, flags);
}

void Handler::plan_presentation(fc::Plan &plan, const DetailLayout &layout) {
    if (role_ != fc::Role::Client) {
        return;
    }
    plan.observe<RenderInvocation>(
        layout.render, fc::before([this](void *renderable, std::uint32_t, float, std::uint32_t, RenderInvocation &invocation) noexcept {
            invocation.previous = thread_state_.render;
            invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
            thread_state_.render = &invocation;
            const auto address = reinterpret_cast<std::uintptr_t>(renderable);
            invocation.subject =
                snapshot_subject(address >= 0x94 ? subjects_.find_soldier(reinterpret_cast<void *>(address - 0x94)) : nullptr);
        }),
        fc::after([](void *, std::uint32_t, float, std::uint32_t, const RenderInvocation &invocation) noexcept {
            if (thread_state_.render == &invocation) {
                thread_state_.render = invocation.previous;
            }
        }));
    for (std::size_t index{}; index < layout.submissions.size(); ++index) {
        plan.observe_instruction(layout.submissions[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                     if (context.edi < 0x94) {
                                         return;
                                     }
                                     auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(context.edi - 0x94));
                                     if (subject == nullptr) {
                                         return;
                                     }
                                     const auto stack = fc::stack_pointer(context);
                                     if (settings_.capture == CaptureMode::Standard) {
                                         const auto root = index == 0 ? native::read<std::uint32_t>(stack) : stack + 0x230;
                                         const auto scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                         record_display(*subject, trace::LocalStage::BodySubmission, scope,
                                                        reinterpret_cast<const float *>(root), reinterpret_cast<void *>(context.ecx));
                                         return;
                                     }
                                     const auto first = index == 0 ? 4 : 0;
                                     const auto root = index == 0 ? native::read<std::uint32_t>(stack) : stack + 0x230;
                                     const auto scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                     const trace::SubmissionRecord record{.turn = turn_context(),
                                                                          .scope = scope,
                                                                          .model = context.ecx,
                                                                          .pose = native::read<std::uint32_t>(stack, first),
                                                                          .root = static_cast<std::uint32_t>(root),
                                                                          .tint = native::read<std::uint32_t>(stack, first + 4),
                                                                          .flags = native::read<std::uint32_t>(stack, first + 8),
                                                                          .parameters = native::read<std::uint32_t>(stack, first + 12),
                                                                          .simulation_scope = subject->simulation_scope,
                                                                          .pose_scope = subject->pose_scope,
                                                                          .pass = static_cast<std::uint8_t>(index)};
                                     if (auto *render = thread_state_.render;
                                         render != nullptr && render->subject && render->subject->id == subject->id) {
                                         render->submissions[index] = record;
                                         render->pending |= static_cast<std::uint8_t>(1U << index);
                                     }
                                     write_subject(trace::RecordKind::Submission, record, *subject, trace::Begin);

                                     record_display(*subject, trace::LocalStage::BodySubmission, scope,
                                                    reinterpret_cast<const float *>(root), reinterpret_cast<void *>(context.ecx));
                                 }));
        if (settings_.capture != CaptureMode::Standard) {
            plan.observe_instruction(layout.submission_returns[index], fc::before([this, index](const fc::CpuContext &) noexcept {
                                         auto *render = thread_state_.render;
                                         const auto bit = static_cast<std::uint8_t>(1U << index);
                                         if (render == nullptr || !render->subject || (render->pending & bit) == 0 ||
                                             !subjects_.live(*render->subject)) {
                                             return;
                                         }
                                         render->pending &= static_cast<std::uint8_t>(~bit);
                                         write_subject(trace::RecordKind::Submission, render->submissions[index], *render->subject,
                                                       trace::End | trace::NativeCalled);
                                     }));
        }
    }
}

void Handler::record_simulation(HeroSubject &subject, std::uint32_t scope, float delta, std::uint8_t stage, bool result) noexcept {
    if (settings_.capture == CaptureMode::Standard) {
        if (stage != 21) {
            return;
        }
        const trace::SimulationOutcomeRecord record{
            .turn = turn_context(),
            .position = native::read<std::array<float, 3>>(subject.soldier, native::kPositionOffset),
            .velocity = native::read<std::array<float, 3>>(subject.soldier, native::kVelocityOffset),
            .scope = scope,
            .parent_scope = subject.simulation_scope,
            .state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
            .energy = native::read<float>(subject.soldier, native::kEnergyOffset),
            .energy_flags = native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset),
            .collision_flags = native::read<std::uint8_t>(subject.soldier, 0x9D4),
            .movement_flags = native::read<std::uint8_t>(subject.soldier, native::kMovementFlagsOffset),
            .stage = stage,
            .result = static_cast<std::uint8_t>(result)};
        write_subject(trace::RecordKind::SimulationOutcome, record, subject);
        return;
    }
    if (settings_.capture == CaptureMode::Combat) {
        trace::SimulationSummaryRecord record{.turn = turn_context(),
                                              .position = native::read<std::array<float, 3>>(subject.soldier, native::kPositionOffset),
                                              .velocity = native::read<std::array<float, 3>>(subject.soldier, native::kVelocityOffset),
                                              .scope = scope,
                                              .delta = delta,
                                              .state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
                                              .energy = native::read<float>(subject.soldier, native::kEnergyOffset),
                                              .energy_flags = native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset),
                                              .state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
                                              .input_time = native::read<float>(subject.weapon, native::kInputTimeOffset),
                                              .notified_state_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset),
                                              .collision_flags = native::read<std::uint8_t>(subject.soldier, 0x9D4),
                                              .movement_flags = native::read<std::uint8_t>(subject.soldier, native::kMovementFlagsOffset),
                                              .stage = stage,
                                              .result = static_cast<std::uint8_t>(result)};
        record.parent_scope = subject.simulation_scope;
        record.controls = native::read<std::array<float, 4>>(subject.owner, native::kControlOffset);
        record.combo_time = native::read<float>(subject.weapon, native::kStateTimeOffset);
        record.mode_time = native::read<float>(subject.weapon, native::kModeTimeOffset);
        write_subject(trace::RecordKind::SimulationSummary, record, subject);
        return;
    }
    trace::SimulationRecord record{.turn = turn_context(),
                                   .position = native::read<std::array<float, 3>>(subject.soldier, native::kPositionOffset),
                                   .velocity = native::read<std::array<float, 3>>(subject.soldier, native::kVelocityOffset),
                                   .normal = native::read<std::array<float, 3>>(subject.soldier, 0x44C),
                                   .controls = native::read<std::array<float, 4>>(subject.owner, native::kControlOffset),
                                   .scope = scope,
                                   .soldier = native::pointer_id(subject.soldier),
                                   .delta = delta,
                                   .state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
                                   .energy = native::read<float>(subject.soldier, native::kEnergyOffset),
                                   .energy_flags = native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset),
                                   .soldier_state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
                                   .sprint_trigger = native::read<std::uint32_t>(subject.owner, native::kSprintTriggerOffset),
                                   .secondary_fire_trigger = native::read<std::uint32_t>(subject.owner, native::kSecondaryTriggerOffset),
                                   .posture = native::read<std::uint32_t>(subject.weapon, native::kPostureOffset),
                                   .notified_state_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset),
                                   .collision_flags = native::read<std::uint8_t>(subject.soldier, 0x9D4),
                                   .stage = stage,
                                   .result = static_cast<std::uint8_t>(result)};
    record.parent_scope = subject.simulation_scope;
    if (stage < 2) {
        record.basis_present = 1;
        for (std::size_t index{}; index < record.basis.size(); ++index) {
            record.basis[index] = native::read<std::array<float, 3>>(subject.soldier, 0xF0 + index * 0x10);
        }
    }
    write_subject(trace::RecordKind::Simulation, record, subject);
}

void Handler::record_orientation(HeroSubject &subject, const void *matrix, const void *quaternion, const void *velocity,
                                 std::uint32_t stream, std::uint32_t scope, std::uint8_t stage) noexcept {
    trace::OrientationRecord record{.turn = turn_context(),
                                    .position = native::read<std::array<float, 3>>(matrix, 0x30),
                                    .velocity = native::read<std::array<float, 3>>(velocity),
                                    .scope = scope,
                                    .stream = stream,
                                    .bit_offset = stream != 0 && network_diagnostics_ready_.load(std::memory_order_acquire)
                                                      ? network_diagnostics_.codec_position(stream, stage == 0 ? 1 : 0)
                                                      : UINT32_MAX,
                                    .stage = stage};
    for (std::size_t index{}; index < record.basis.size(); ++index) {
        record.basis[index] = native::read<std::array<float, 3>>(matrix, index * 0x10);
    }
    if (quaternion != nullptr) {
        record.quaternion = native::read<std::array<float, 4>>(quaternion);
    }
    write_subject(trace::RecordKind::Orientation, record, subject);
}

void Handler::record_motion_operands(HeroSubject &subject, const fc::CpuContext &context, std::uint32_t scope,
                                     std::uint8_t stage) noexcept {
    trace::MotionOperandsRecord record{.turn = turn_context(),
                                       .scope = scope,
                                       .basis_f0 = native::read<std::array<float, 3>>(subject.soldier, 0xF0),
                                       .basis_110 = native::read<std::array<float, 3>>(subject.soldier, 0x110),
                                       .words = {native::pointer_id(native::read<void *>(subject.soldier, 0x43C)),
                                                 native::read<std::uint8_t>(subject.soldier, 0x9D4), context.eflags,
                                                 native::read<std::uint16_t>(subject.soldier, native::kPackedSelectionOffset)},
                                       .stage = stage};
    for (std::size_t item{}; item < record.values.size(); ++item) {
        record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
    }
    if (stage == 2) {
        record.basis = {native::read<std::array<float, 3>>(fc::stack_pointer(context), 0x14),
                        native::read<std::array<float, 3>>(fc::stack_pointer(context), 0x24)};
    } else if (stage >= 6) {
        const auto support = native::read<std::uint32_t>(context.ebp, -8);
        const auto type = native::read<std::uint32_t>(support, 0x12C);
        record.basis = {type == 2 ? native::read<std::array<float, 3>>(support, 0xFC) : std::array<float, 3>{},
                        native::read<std::array<float, 3>>(context.ebp, -0x30)};
        record.words = {support, type, type == 2 ? native::read<std::uint32_t>(native::read<void *>(support, 0x30), 0x11C) : 0,
                        native::read<std::uint32_t>(native::read<void *>(subject.owner), 0x84)};
        if (stage == 7 && type == 2) {
            const auto output = native::read<std::array<float, 3>>(context.ebp, -0x14);
            std::copy(output.begin(), output.end(), record.values.begin());
        }
    }
    if (stage == 4 || stage == 5 || stage == 7) {
        record.outcome = 1;
        record.position = native::read<std::array<float, 3>>(subject.soldier, native::kPositionOffset);
        record.velocity = native::read<std::array<float, 3>>(subject.soldier, native::kVelocityOffset);
        record.normal = native::read<std::array<float, 3>>(subject.soldier, 0x44C);
    }
    write_subject(trace::RecordKind::MotionOperands, record, subject);
}

void Handler::plan_simulation(fc::Plan &plan, const DetailLayout &layout) {
    if (settings_.capture == CaptureMode::Full) {
        for (std::size_t index{}; index < 2; ++index) {
            plan.observe_instruction(layout.rotation_producers[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         // Ground movement has already adjusted EDI from the soldier to its transform subobject.
                                         auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(context.edi - 0x0C));
                                         if (subject == nullptr) {
                                             return;
                                         }

                                         const auto stack = fc::stack_pointer(context);
                                         trace::MotionOperandsRecord record{
                                             .turn = turn_context(),
                                             .scope = subject->simulation_scope,
                                             .basis_f0 = native::read<std::array<float, 3>>(subject->soldier, 0xF0),
                                             .basis_110 = native::read<std::array<float, 3>>(subject->soldier, 0x110),
                                             .stage = static_cast<std::uint8_t>(8 + index)};
                                         record.values[0] = native::read<float>(stack, 8);
                                         record.words[0] = native::read<std::uint32_t>(stack);
                                         record.words[1] = native::read<std::uint32_t>(stack, 4);
                                         if (index == 1) {
                                             // Read the helper's initialized outputs only after the native call returns.
                                             record.values[1] = native::read<float>(record.words[0]);
                                             record.values[2] = native::read<float>(record.words[1]);
                                         }
                                         write_subject(trace::RecordKind::MotionOperands, record, *subject);
                                     }));
        }

        // Separate native sites bracket the setter; EDI still addresses the transform before the epilogue restores it.
        for (std::size_t index{2}; index < layout.rotation_producers.size(); ++index) {
            plan.observe_instruction(layout.rotation_producers[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         if (auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(context.edi - 0x0C))) {
                                             const auto matrix = index == 2 ? fc::stack_pointer(context) + 0x90
                                                                            : native::pointer_id(subject->soldier) + 0xF0;
                                             record_orientation(*subject, reinterpret_cast<const void *>(matrix), nullptr,
                                                                static_cast<const std::byte *>(subject->soldier) + native::kVelocityOffset,
                                                                0, subject->simulation_scope, static_cast<std::uint8_t>(index + 3));
                                         }
                                     }));
        }

        for (std::size_t index{}; index < layout.motion_operands.size(); ++index) {
            plan.observe_instruction(layout.motion_operands[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         const auto soldier = index < 3 ? context.edi : context.esi;
                                         if (auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(soldier))) {
                                             record_motion_operands(*subject, context, subject->simulation_scope,
                                                                    static_cast<std::uint8_t>(index + 2));
                                         }
                                     }));
        }
    }
    // PostCollisionUpdate consumes the accumulated contacts before clearing them and publishing the ground normal.
    plan.observe<SimulationInvocation>(layout.post_collision,
                                       fc::before([this](void *soldier, float delta, SimulationInvocation &invocation) noexcept {
                                           invocation.previous = thread_state_.collision;
                                           thread_state_.collision = &invocation;
                                           invocation.soldier = soldier;
                                           invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
                                           invocation.subject = snapshot_subject(subjects_.find_soldier(soldier));
                                           invocation.delta = delta;
                                           if (invocation.subject) {
                                               record_simulation(*invocation.subject, invocation.scope, delta, 20);
                                           }
                                       }),
                                       fc::after([this](void *, float, bool result, const SimulationInvocation &invocation) noexcept {
                                           if (thread_state_.collision == &invocation) {
                                               thread_state_.collision = invocation.previous;
                                           }
                                           if (invocation.subject && subjects_.live(*invocation.subject)) {
                                               record_simulation(*invocation.subject, invocation.scope, invocation.delta, 21, result);
                                           }
                                       }));
    if (settings_.capture == CaptureMode::Full) {
        for (std::size_t index{}; index < layout.collision_decisions.size(); ++index) {
            std::uint32_t collision_threshold_address{};
            if (index < 2) {
                const auto branch = layout.collision_decisions[index].rva;
                const auto target = layout.collision_thresholds[index];
                plan.check(branch - 7, 3, fc::exact_bytes({0x0F, 0x2F, index == 0 ? std::uint8_t{0x15} : std::uint8_t{0x05}}));
                const auto operand = plan.read_bytes(branch - 4, 4, fc::points_to(plan.at(target)));
                if (!operand) {
                    return;
                }
                // Retain the verified original operand; instruction callbacks report a trampoline instruction address.
                std::memcpy(&collision_threshold_address, operand->data(), sizeof(collision_threshold_address));
            }
            plan.observe_instruction(layout.collision_decisions[index],
                                     fc::before([this, index, collision_threshold_address](const fc::CpuContext &context) noexcept {
                                         const auto *invocation = thread_state_.collision;
                                         if (invocation == nullptr || !invocation->subject || !subjects_.live(*invocation->subject)) {
                                             return;
                                         }
                                         auto &subject = *invocation->subject;
                                         trace::IntegrationRecord record{.turn = turn_context(),
                                                                         .scope = invocation->scope,
                                                                         .soldier = native::pointer_id(subject.soldier),
                                                                         .eflags = context.eflags,
                                                                         .stage = static_cast<std::uint8_t>(16 + index)};
                                         const auto accumulator = native::read<std::array<float, 3>>(subject.soldier, 0x464);
                                         std::copy(accumulator.begin(), accumulator.end(), record.values.begin());
                                         record.values[3] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
                                         record.values[4] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[2].low));
                                         record.values[5] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low));
                                         record.values[6] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low >> 32U));
                                         record.values[7] = index < 2 ? native::read<float>(collision_threshold_address)
                                                                      : std::bit_cast<float>(context.ecx);
                                         record.words[0] = native::read<std::uint8_t>(subject.soldier, 0x9D4);
                                         record.words[1] = native::read<std::uint8_t>(subject.soldier, 0x470);
                                         record.words[2] = native::read<std::uint8_t>(subject.soldier, 0x472);
                                         record.words[3] = static_cast<std::uint32_t>((context.eflags & 0x41U) != 0);
                                         write_subject(trace::RecordKind::Integration, record, subject);
                                     }));
        }
    }
    plan.observe<SimulationInvocation>(
        layout.simulation, fc::before([this](void *controllable, float delta, SimulationInvocation &invocation) noexcept {
            invocation.previous = thread_state_.simulation;
            thread_state_.simulation = &invocation;
            invocation.soldier = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(controllable) - 0x240);
            invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
            auto *subject = subjects_.find_soldier(invocation.soldier);
            if (subject != nullptr) {
                subject->simulation_scope = invocation.scope;
            }
            invocation.subject = snapshot_subject(subject);
            invocation.delta = delta;
            if (invocation.subject) {
                record_simulation(*invocation.subject, invocation.scope, delta, 0);
            }
        }),
        fc::after([this](void *, float, bool result, const SimulationInvocation &invocation) noexcept {
            if (thread_state_.simulation == &invocation) {
                thread_state_.simulation = invocation.previous;
            }
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                record_simulation(*invocation.subject, invocation.scope, invocation.delta, 1, result);
            }
        }));

    if (settings_.capture != CaptureMode::Full) {
        return;
    }
    for (std::size_t index{}; index < layout.integration.size(); ++index) {
        plan.observe_instruction(
            layout.integration[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                auto address = index < 2 ? context.ecx : index == 2 ? context.esi : index >= 4 && index <= 7 ? context.edi : context.esi;
                if (index == 3) {
                    const auto *invocation = thread_state_.simulation;
                    address = invocation != nullptr ? native::pointer_id(invocation->soldier) : 0;
                } else if (index == 7 || index == 13) {
                    address -= 0xC;
                }
                auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(address));
                if (subject == nullptr) {
                    return;
                }
                const auto *invocation = thread_state_.simulation;
                const auto scope = invocation != nullptr && invocation->soldier == subject->soldier ? invocation->scope : 0;
                trace::IntegrationRecord record{.turn = turn_context(),
                                                .scope = scope,
                                                .soldier = address,
                                                .eflags = context.eflags,
                                                .stage = static_cast<std::uint8_t>(index)};
                for (std::size_t item{}; item < 4; ++item) {
                    record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
                }
                record.words = {native::read<std::uint8_t>(subject->soldier, 0x9D4),
                                native::pointer_id(native::read<void *>(subject->soldier, 0x43C)),
                                native::pointer_id(native::read<void *>(subject->soldier, 0x440)),
                                native::read<std::uint32_t>(subject->soldier, native::kEnergyFlagsOffset),
                                native::read<std::uint32_t>(subject->weapon, 0xAC),
                                native::read<std::uint8_t>(subject->soldier, 0x3A0)};
                if (index < 2) {
                    record.values[4] = native::read<float>(fc::stack_pointer(context), 4);
                    record.values[5] = index == 0 ? native::read<float>(fc::stack_pointer(context), 8) : 0.0F;
                } else if (index == 4 || index == 8) {
                    record.values[4] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[4].low));
                } else if (index == 6 || index == 12) {
                    record.values[4] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[index == 6 ? 5 : 4].low));
                }
                if (index < 2 || index == 5 || index == 7 || index == 9 || index == 11 || index == 13) {
                    record.outcome = 1;
                    record.position = native::read<std::array<float, 3>>(subject->soldier, native::kPositionOffset);
                    record.velocity = native::read<std::array<float, 3>>(subject->soldier, native::kVelocityOffset);
                    record.normal = native::read<std::array<float, 3>>(subject->soldier, 0x44C);
                }
                write_subject(trace::RecordKind::Integration, record, *subject);
                if (index < 2) {
                    record_motion_operands(*subject, context, scope, static_cast<std::uint8_t>(index));
                }
            }));
    }
}

trace::SoldierDecisionRecord Handler::soldier_decision(const HeroSubject &subject, std::uint8_t stage) const noexcept {
    const auto *simulation = thread_state_.simulation;
    const auto scope =
        simulation != nullptr && simulation->subject && simulation->subject->soldier == subject.soldier ? simulation->scope : 0;
    trace::SoldierDecisionRecord record{.turn = turn_context(),
                                        .scope = scope,
                                        .state = native::read<std::int32_t>(subject.soldier, native::kLocomotionStateOffset),
                                        .state_time = native::read<float>(subject.soldier, native::kLocomotionStateTimeOffset),
                                        .energy = native::read<float>(subject.soldier, native::kEnergyOffset),
                                        .energy_flags = native::read<std::uint32_t>(subject.soldier, native::kEnergyFlagsOffset),
                                        .stage = stage};
    if (settings_.capture == CaptureMode::Full) {
        record.posture = native::read<std::uint32_t>(subject.weapon, native::kPostureOffset);
        record.notified_time = native::read<float>(subject.weapon, native::kNotifiedStateTimeOffset);
        record.sprint_trigger = native::read<std::uint32_t>(subject.owner, native::kSprintTriggerOffset);
        record.collision_flags = native::read<std::uint8_t>(subject.soldier, 0x9D4);
    }
    return record;
}

void Handler::write_soldier_decision(HeroSubject &subject, const trace::SoldierDecisionRecord &record, std::uint16_t flags) noexcept {
    if (settings_.capture == CaptureMode::Full) {
        write_subject(trace::RecordKind::SoldierDecision, record, subject, flags);
        return;
    }
    const trace::SoldierOutcomeRecord outcome{.turn = record.turn,
                                              .scope = record.scope,
                                              .state = record.state,
                                              .state_time = record.state_time,
                                              .energy = record.energy,
                                              .energy_flags = record.energy_flags,
                                              .arguments = record.values,
                                              .requested = {record.words[0], record.words[1], record.words[2]},
                                              .stage = record.stage,
                                              .result = record.result};
    write_subject(trace::RecordKind::SoldierOutcome, outcome, subject, flags);
}

void Handler::plan_gameplay_decisions(fc::Plan &plan, const DetailLayout &layout) {
    plan.observe<SoldierDecisionInvocation>(
        layout.soldier_state,
        fc::before(
            [this](const fc::NativeContext &context, void *soldier, std::int32_t state, SoldierDecisionInvocation &invocation) noexcept {
                invocation.subject = snapshot_subject(subjects_.find_soldier(soldier));
                if (invocation.subject) {
                    invocation.record = soldier_decision(*invocation.subject, 0);
                    invocation.record.caller = static_cast<std::uint32_t>(context.return_address());
                    invocation.record.words[0] = static_cast<std::uint32_t>(state);
                    write_soldier_decision(*invocation.subject, invocation.record, trace::Begin);
                }
            }),
        fc::after([this](void *, std::int32_t, bool result, const SoldierDecisionInvocation &invocation) noexcept {
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }
            auto record = soldier_decision(*invocation.subject, 1);
            record.caller = invocation.record.caller;
            record.scope = invocation.record.scope;
            record.words = invocation.record.words;
            record.result = static_cast<std::uint8_t>(result);
            write_soldier_decision(*invocation.subject, record, trace::End | trace::NativeCalled);
        }));
    plan.observe<SoldierDecisionInvocation>(
        layout.notify,
        fc::before([this](const fc::NativeContext &context, void *weapon, std::int32_t state, float elapsed, bool locked, bool aiming,
                          const float *velocity, SoldierDecisionInvocation &invocation) noexcept {
            invocation.subject = snapshot_subject(subjects_.find_weapon(weapon));
            if (invocation.subject) {
                invocation.record = soldier_decision(*invocation.subject, 2);
                invocation.record.caller = static_cast<std::uint32_t>(context.return_address());
                invocation.record.values = {elapsed, native::read<float>(velocity), native::read<float>(velocity, 4),
                                            native::read<float>(velocity, 8)};
                invocation.record.words = {static_cast<std::uint32_t>(state), static_cast<std::uint32_t>(locked),
                                           static_cast<std::uint32_t>(aiming)};
                write_soldier_decision(*invocation.subject, invocation.record, trace::Begin);
            }
        }),
        fc::after([this](void *, std::int32_t, float, bool, bool, const float *, const SoldierDecisionInvocation &invocation) noexcept {
            if (!invocation.subject || !subjects_.live(*invocation.subject)) {
                return;
            }
            auto record = soldier_decision(*invocation.subject, 3);
            record.caller = invocation.record.caller;
            record.scope = invocation.record.scope;
            record.values = invocation.record.values;
            record.words = invocation.record.words;
            write_soldier_decision(*invocation.subject, record, trace::End | trace::NativeCalled);
        }));
    plan.observe<SoldierDecisionInvocation>(
        layout.sprint_admission,
        fc::before([this](const fc::NativeContext &context, void *soldier, SoldierDecisionInvocation &invocation) noexcept {
            invocation.subject = snapshot_subject(subjects_.find_soldier(soldier));
            if (invocation.subject) {
                invocation.record = soldier_decision(*invocation.subject, 4);
                invocation.record.caller = static_cast<std::uint32_t>(context.return_address());
                invocation.record.words[0] = native::read<std::uint32_t>(native::read<void *>(invocation.subject->owner), 0xAC);
                write_soldier_decision(*invocation.subject, invocation.record, trace::Begin);
            }
        }),
        fc::after([this](void *, bool result, const SoldierDecisionInvocation &invocation) noexcept {
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                auto record = soldier_decision(*invocation.subject, 5);
                record.caller = invocation.record.caller;
                record.words = invocation.record.words;
                record.result = static_cast<std::uint8_t>(result);
                write_soldier_decision(*invocation.subject, record, trace::End | trace::NativeCalled);
            }
        }));

    if (settings_.capture == CaptureMode::Full) {
        // Capture verified absolute comparison operands before relocation moves instructions into a trampoline.
        for (std::size_t index{}; index < layout.locomotion_decisions.size(); ++index) {
            std::uint32_t threshold{};
            if (index >= 5 && index != 6) {
                const auto target = layout.locomotion_thresholds[index == 5 ? 0 : 1];
                const auto operand = plan.read_bytes(layout.locomotion_decisions[index].rva - 4, 4, fc::points_to(plan.at(target)));
                if (!operand) {
                    return;
                }
                std::memcpy(&threshold, operand->data(), sizeof(threshold));
            }
            plan.observe_instruction(
                layout.locomotion_decisions[index], fc::before([this, index, threshold](const fc::CpuContext &context) noexcept {
                    const auto *invocation = thread_state_.simulation;
                    if (invocation == nullptr || !invocation->subject || !subjects_.live(*invocation->subject)) {
                        return;
                    }
                    auto &subject = *invocation->subject;
                    auto record = soldier_decision(subject, static_cast<std::uint8_t>(10 + index));
                    for (std::size_t item{}; item < record.values.size(); ++item) {
                        record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
                    }
                    if (threshold != 0) {
                        record.values[2] = native::read<float>(threshold);
                    }
                    record.values[3] = native::read<float>(native::read<void *>(subject.soldier, 0x440), 0x69C);
                    record.words = {context.eax, context.eflags, native::read<std::uint32_t>(subject.owner, native::kJumpTriggerOffset),
                                    native::read<std::uint32_t>(subject.owner, 0x48)};
                    // Only these sites follow actual calls; other records carry comparison operands or trigger words.
                    if (index == 1 || index == 4 || index == 6) {
                        record.result = static_cast<std::uint8_t>((context.eax & 0xFFU) != 0);
                    }
                    write_subject(trace::RecordKind::SoldierDecision, record, subject);
                }));
        }
        for (std::size_t index{}; index < layout.sprint_cone.size(); ++index) {
            plan.observe_instruction(layout.sprint_cone[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                                         auto *subject = subjects_.find_soldier(reinterpret_cast<void *>(context.ecx));
                                         if (subject == nullptr) {
                                             return;
                                         }
                                         auto record = soldier_decision(*subject, static_cast<std::uint8_t>(20 + index));
                                         for (std::size_t item{}; item < record.values.size(); ++item) {
                                             record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
                                         }
                                         // This helper has instruction observers only, so its native return remains on the original stack.
                                         record.caller = native::read<std::uint32_t>(fc::stack_pointer(context));
                                         record.words[0] = context.eflags;
                                         write_subject(trace::RecordKind::SoldierDecision, record, *subject);
                                     }));
        }
    }
    if (role_ == fc::Role::Client && settings_.capture == CaptureMode::Full) {
        plan.observe_instruction(layout.animation_producer, fc::before([this](const fc::CpuContext &context) noexcept {
                                     if (auto *subject = subjects_.find_animator(reinterpret_cast<void *>(context.esi))) {
                                         trace::AnimationDecisionRecord record{
                                             .turn = turn_context(),
                                             .scope = thread_state_.pose_scope,
                                             .caller = native::read<std::uint32_t>(context.ebp, 4),
                                             .arguments = native::read<std::array<std::uint32_t, 4>>(context.ebp, 8),
                                             .upper_clip = native::pointer_id(native::read<void *>(subject->animator, 0x15E4)),
                                             .lower_clip = native::pointer_id(native::read<void *>(subject->animator, 0x1F94)),
                                             .action = native::read<std::uint32_t>(subject->animator, 0x1FEC),
                                             .weapon_animation = native::read<std::uint32_t>(subject->animator, 0x2014),
                                             .flags = native::read<std::uint16_t>(subject->animator, 0x201A),
                                             .pending_flags = native::read<std::uint16_t>(subject->animator, 0x201C)};
                                         for (std::size_t item{}; item < record.values.size(); ++item) {
                                             record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
                                         }
                                         write_subject(trace::RecordKind::AnimationDecision, record, *subject);
                                     }
                                 }));
        for (std::size_t index{}; index < layout.movement_animation.size(); ++index) {
            plan.observe_instruction(
                layout.movement_animation[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                    if (auto *subject = subjects_.find_animator(reinterpret_cast<void *>(context.esi))) {
                        trace::AnimationDecisionRecord record{
                            .turn = turn_context(),
                            .scope = thread_state_.pose_scope,
                            .caller = native::read<std::uint32_t>(context.ebp, 4),
                            .upper_clip = native::pointer_id(native::read<void *>(subject->animator, 0x15E4)),
                            .lower_clip = native::pointer_id(native::read<void *>(subject->animator, 0x1F94)),
                            .action = native::read<std::uint32_t>(subject->animator, 0x1FEC),
                            .weapon_animation = native::read<std::uint32_t>(subject->animator, 0x2014),
                            .flags = native::read<std::uint16_t>(subject->animator, 0x201A),
                            .pending_flags = native::read<std::uint16_t>(subject->animator, 0x201C),
                            .stage = static_cast<std::uint8_t>(index + 1)};
                        for (std::size_t item{}; item < record.values.size(); ++item) {
                            record.values[item] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[item].low));
                        }
                        if (index == 0) {
                            // The custom helper receives lateral/forward scalars in XMM1/2 and angle/delta output pointers on the stack.
                            record.arguments[0] = native::read<std::uint32_t>(fc::stack_pointer(context));
                            record.arguments[1] = native::read<std::uint32_t>(fc::stack_pointer(context), 4);
                        } else {
                            record.arguments = {index == 1 ? context.eax : context.edi,
                                                native::read<std::uint32_t>(fc::stack_pointer(context), 0x24),
                                                native::read<std::uint32_t>(fc::stack_pointer(context), index == 1 ? 0x20 : 0x14),
                                                native::read<std::uint32_t>(fc::stack_pointer(context), index == 1 ? 0x10 : 0x1C)};
                        }
                        write_subject(trace::RecordKind::AnimationDecision, record, *subject);
                    }
                }));
        }
    }
}

void Handler::plan_energy_codec(fc::Plan &plan, const DetailLayout &layout) {
    for (std::uint8_t encoding{}; encoding < 2; ++encoding) {
        plan.observe<EnergyInvocation>(
            encoding != 0 ? layout.energy_write : layout.energy_read,
            fc::before(
                [this, encoding](const fc::NativeContext &context, void *energy, void *stream, EnergyInvocation &invocation) noexcept {
                    invocation.previous = thread_state_.energy;
                    thread_state_.energy = &invocation;
                    const auto address = reinterpret_cast<std::uintptr_t>(energy);
                    invocation.subject =
                        snapshot_subject(address >= 0xA10 ? subjects_.find_soldier(reinterpret_cast<void *>(address - 0xA10)) : nullptr);
                    if (!invocation.subject) {
                        return;
                    }
                    const auto *parameters = native::read<void *>(energy);
                    const auto flags = native::read<std::uint32_t>(energy, 8);
                    invocation.record = {.turn = turn_context(),
                                         .scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1,
                                         .caller = static_cast<std::uint32_t>(context.return_address()),
                                         .stream = native::pointer_id(stream),
                                         .bit_offset = network_diagnostics_ready_.load(std::memory_order_acquire)
                                                           ? network_diagnostics_.codec_position(native::pointer_id(stream), encoding)
                                                           : UINT32_MAX,
                                         .energy = native::read<float>(energy, 4),
                                         .lower = (flags & 1U) != 0 ? native::read<float>(parameters, 4) : 0.0F,
                                         .upper = native::read<float>(parameters, (flags & 1U) != 0 ? 8 : 0),
                                         .energy_flags = flags,
                                         .encoding = encoding};
                    write_subject(trace::RecordKind::EnergyCodec, invocation.record, *invocation.subject, trace::Begin);
                }),
            fc::after([this](void *energy, void *, const EnergyInvocation &invocation) noexcept {
                thread_state_.energy = invocation.previous;
                if (invocation.subject && subjects_.live(*invocation.subject)) {
                    auto record = invocation.record;
                    record.energy = native::read<float>(energy, 4);
                    record.energy_flags = native::read<std::uint32_t>(energy, 8);
                    const auto *parameters = native::read<void *>(energy);
                    record.lower = (record.energy_flags & 1U) != 0 ? native::read<float>(parameters, 4) : 0.0F;
                    record.upper = native::read<float>(parameters, (record.energy_flags & 1U) != 0 ? 8 : 0);
                    record.stage = 3;
                    write_subject(trace::RecordKind::EnergyCodec, record, *invocation.subject, trace::End | trace::NativeCalled);
                }
            }));
    }
    // Nested bit calls expose actual raw/fixed representations, including the overheat bit before energy.
    const auto begin_bits = [this](void *stream, std::int32_t bits, std::uint8_t encoding, EnergyBitsInvocation &invocation) noexcept {
        auto *energy = thread_state_.energy;
        if (energy == nullptr || !energy->subject || energy->record.encoding != encoding ||
            energy->record.stream != native::pointer_id(stream) || !subjects_.live(*energy->subject)) {
            return;
        }
        invocation.subject = energy->subject;
        invocation.record = energy->record;
        auto &record = invocation.record;
        record.bits = bits;
        record.scalar = energy->scalars++;
        record.stage = 1;
        // Start before any nested primitive observers increment the network cursor; hook ordering cannot shift this offset.
        record.bit_offset = energy->record.bit_offset != UINT32_MAX ? energy->record.bit_offset + energy->bits : UINT32_MAX;
        energy->bits += static_cast<std::uint32_t>(bits);
        const auto *object = static_cast<const std::byte *>(invocation.subject->soldier) + 0xA10;
        const auto *parameters = native::read<void *>(object);
        record.energy = native::read<float>(object, 4);
        record.energy_flags = native::read<std::uint32_t>(object, 8);
        record.lower = (record.energy_flags & 1U) != 0 ? native::read<float>(parameters, 4) : 0.0F;
        record.upper = native::read<float>(parameters, (record.energy_flags & 1U) != 0 ? 8 : 0);
    };
    plan.observe<EnergyBitsInvocation>(
        layout.write_bits,
        fc::before([this, begin_bits](void *stream, std::uint32_t code, std::int32_t bits, EnergyBitsInvocation &invocation) noexcept {
            begin_bits(stream, bits, 1, invocation);
            if (invocation.subject) {
                invocation.record.code = code;
                write_subject(trace::RecordKind::EnergyCodec, invocation.record, *invocation.subject, trace::Begin);
            }
        }));
    plan.observe<EnergyBitsInvocation>(
        layout.read_bits,
        fc::before([begin_bits](void *stream, std::uint32_t *, std::int32_t bits, EnergyBitsInvocation &invocation) noexcept {
            begin_bits(stream, bits, 0, invocation);
        }),
        fc::after([this](void *, std::uint32_t *code, std::int32_t, const EnergyBitsInvocation &invocation) noexcept {
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                auto record = invocation.record;
                record.code = native::read<std::uint32_t>(code);
                record.stage = 2;
                write_subject(trace::RecordKind::EnergyCodec, record, *invocation.subject, trace::End | trace::NativeCalled);
            }
        }));
}

void Handler::plan_combat_decisions(fc::Plan &plan, const DetailLayout &layout) {
    plan.observe<CombatInvocation>(
        layout.combat_update, fc::before([this](void *weapon, float delta, CombatInvocation &invocation) noexcept {
            invocation.parent_scope = current_scope().operation_scope;
            invocation.previous = thread_state_.combat;
            thread_state_.combat = &invocation;
            invocation.subject = snapshot_subject(subjects_.find_weapon(weapon));
            invocation.scope = next_scope_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (invocation.subject) {
                auto &subject = *invocation.subject;
                const auto row = native::read<std::uint32_t>(subject.combo, native::kComboRowsOffset) +
                                 native::read<std::int32_t>(weapon, native::kStateOffset) * native::kComboRowSize;
                const trace::CombatDecisionRecord record{.turn = turn_context(),
                                                         .values = {delta, native::read<float>(weapon, native::kModeTimeOffset),
                                                                    native::read<float>(weapon, native::kStateTimeOffset)},
                                                         .words = native::read<std::array<std::uint32_t, 7>>(row),
                                                         .scope = invocation.scope,
                                                         .target = native::read<std::uint32_t>(row, 0x1C),
                                                         .native_target = invocation.parent_scope,
                                                         .row = row,
                                                         .cache = native::read<std::uint32_t>(native::read<void *>(weapon, 0x64), 0x2C4),
                                                         .stage = 0};
                write_subject(trace::RecordKind::CombatDecision, record, subject, trace::Begin);
            }
        }),
        fc::after([this](void *, float, bool result, const CombatInvocation &invocation) noexcept {
            if (thread_state_.combat == &invocation) {
                thread_state_.combat = invocation.previous;
            }
            if (invocation.subject && subjects_.live(*invocation.subject)) {
                const trace::CombatDecisionRecord record{.turn = turn_context(),
                                                         .scope = invocation.scope,
                                                         .native_target = invocation.parent_scope,
                                                         .stage = 12,
                                                         .result = static_cast<std::uint8_t>(result)};
                write_subject(trace::RecordKind::CombatDecision, record, *invocation.subject, trace::End | trace::NativeCalled);
            }
        }));

    for (std::size_t index{}; index < layout.combat_decisions.size(); ++index) {
        plan.observe_instruction(
            layout.combat_decisions[index], fc::before([this, index](const fc::CpuContext &context) noexcept {
                const auto *invocation = thread_state_.combat;
                if (invocation == nullptr || !invocation->subject || !subjects_.live(*invocation->subject)) {
                    return;
                }
                auto subject = *invocation->subject;
                const auto stack = fc::stack_pointer(context);
                trace::CombatDecisionRecord record{.turn = turn_context(),
                                                   .scope = invocation->scope,
                                                   .eflags = context.eflags,
                                                   .stage = static_cast<std::uint8_t>(index + 1)};
                if (index < 2) {
                    record.row = context.esi;
                    const auto packed = native::read<std::array<std::uint32_t, 4>>(context.esi);
                    std::copy(packed.begin(), packed.end(), record.words.begin());
                    record.values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low)),
                                     std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)),
                                     std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[2].low)),
                                     std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[4].low))};
                    record.result = static_cast<std::uint8_t>(index == 0 ? (context.eflags & 1U) != 0 : (context.eflags & 0x41U) != 0);
                } else {
                    const auto shift = index == 6 ? 0xC : index == 8 ? 0x10 : index == 10 ? 8 : 0;
                    record.row = native::read<std::uint32_t>(stack, 0x74 + shift);
                    record.cache = native::read<std::uint32_t>(stack, 0x68 + shift);
                    record.target = index == 2 || index == 3 || index >= 6 ? context.esi : native::read<std::uint32_t>(stack, 0xA8);
                    if (index == 2) {
                        record.native_target = context.eax;
                        record.result = static_cast<std::uint8_t>(context.eax != 0);
                    } else if (index == 3) {
                        record.words[0] = context.ecx;
                        record.words[1] = context.edi;
                        record.result =
                            static_cast<std::uint8_t>(static_cast<std::int32_t>(context.ecx) < static_cast<std::int32_t>(context.edi));
                    } else if (index == 4) {
                        record.values[0] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low));
                        record.values[1] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
                        record.words[0] = context.eax;
                        record.words[1] = native::read<std::uint32_t>(stack, 0x34);
                        record.result = static_cast<std::uint8_t>((context.eflags & 0x41U) == 0);
                    } else if (index == 5) {
                        record.words[0] = context.eax;
                        record.result = static_cast<std::uint8_t>(static_cast<std::int32_t>(context.eax) >= 8);
                    } else if (index == 6) {
                        record.native_target = native::read<std::uint32_t>(native::read<std::uint32_t>(context.esi), 0xD4);
                        const auto position = native::read<std::array<float, 3>>(native::read<std::uint32_t>(stack, 8));
                        const auto direction = native::read<std::array<float, 3>>(native::read<std::uint32_t>(stack, 4));
                        for (std::size_t item{}; item < 3; ++item) {
                            record.values[item] = position[item];
                            record.values[item + 3] = direction[item];
                        }
                        record.words[0] = native::read<std::uint32_t>(stack);
                    } else if (index == 7 || index == 9) {
                        record.result = static_cast<std::uint8_t>(context.eax & 0xFFU);
                    } else if (index == 8) {
                        record.native_target = context.ecx;
                        record.words = native::read<std::array<std::uint32_t, 7>>(stack + 0x198);
                        record.values[0] = native::read<float>(stack, 0x1B4);
                        record.values[1] = native::read<float>(stack, 0x80);
                        record.cache = native::read<std::uint32_t>(context.eax, 0x14);
                    } else if (index == 10) {
                        record.values[0] = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
                        record.values[1] = native::read<float>(stack, 0xD0);
                        record.values[2] = native::read<float>(stack, 0xD4);
                        record.values[3] = native::read<float>(stack, 0xD8);
                    }
                }
                const auto flags = index == 7 || index == 9 ? trace::End | trace::NativeCalled : trace::None;
                write_subject(trace::RecordKind::CombatDecision, record, subject, static_cast<std::uint16_t>(flags));
            }));
    }
}

} // namespace fc_plugins::hero_diagnostics
