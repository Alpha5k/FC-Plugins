#pragma once

#include "layout.hpp"
#include "patch.hpp"
#include "schema.hpp"
#include "subject.hpp"
#include "api.hpp"
#include "../network_diagnostics/api.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fc_plugins::hero_diagnostics {

// Tracks player-owned melee weapons and publishes native observations and synchronous behavior decisions.
class Handler {
  public:
    using Settings = hero_diagnostics::Settings;

    // Receives immutable settings and the role selected by the factory variant.
    Handler(const Settings &settings, fc::Role role) noexcept;

    // Declares observations and read-only native ranges for the selected role and executable profile.
    void plan(fc::Plan &plan);

    // Creates the bounded trace before native callbacks can run.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Reports records accepted by ETW and records dropped by the channel.
    void write_status(fc::StatusContext &context) const noexcept;

  private:
    // Copies an already computed selection outcome without enrolling a new Hero subject.
    void observe_selection_decision(void *soldier, const SelectionDecision *decision) noexcept;

    using DiagnosticsApi = HeroDiagnosticsApi;
    using MeleeMode = MeleeUpdateMode;
    using NetworkMode = NetworkStateMode;

    // Runtime-allocated invocation storage copies identity before nested calls can replace a player-slot entry.
    struct UpdateInvocation {
        UpdateInvocation *previous{};
        void *weapon{};
        mutable std::optional<HeroSubject> subject{};
        trace::MeleeTickRecord record{};
        std::uint16_t flags{};
        std::uint32_t scope{};
        std::uint32_t condition_ordinal{};
    };

    struct NetworkInvocation {
        NetworkInvocation *previous{};
        void *weapon{};
        mutable std::optional<HeroSubject> subject{};
        trace::NetworkStateRecord record{};
        std::uint16_t flags{};
    };

    struct AnimatorInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::PresentationFrameRecord record{};
    };

    struct InputInvocation {
        void *weapon{};
        mutable std::optional<HeroSubject> subject{};
        trace::InputDecisionRecord record{};
        trace::NativeQueueRecord queue{};
        std::uint16_t flags{};
    };

    struct ClearInputInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::NativeQueueRecord queue{};
    };

    struct PredictionInvocation {
        void *weapon{};
        mutable std::optional<HeroSubject> subject{};
        std::uintptr_t instruction{};
        trace::TransitionRecord record{};
    };

    struct TransitionInvocation;
    struct AnimationInvocation;
    struct SimulationInvocation;

    struct SoldierDecisionInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::SoldierDecisionRecord record{};
    };

    struct EnergyInvocation {
        EnergyInvocation *previous{};
        mutable std::optional<HeroSubject> subject{};
        trace::EnergyCodecRecord record{};
        std::uint8_t scalars{};
        std::uint32_t bits{};
    };

    struct EnergyBitsInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::EnergyCodecRecord record{};
    };
    struct CombatInvocation;
    struct ContactInvocation;
    struct RenderInvocation;
    struct ControllerInvocation;
    struct ControlsInvocation;

    // Thread-local heads receive synchronous decision reports only when the current topmost invocation's weapon matches.
    struct ThreadState {
        UpdateInvocation *update{};
        NetworkInvocation *network_state{};
        TransitionInvocation *transition{};

        // TestCondition has no recursive calls; its instruction observers copy one evaluated native call.
        std::optional<HeroSubject> condition_subject{};
        trace::ConditionRecord condition{};
        trace::PredicateRecord predicate{};
        SimulationInvocation *simulation{};
        EnergyInvocation *energy{};
        SimulationInvocation *collision{};
        CombatInvocation *combat{};
        ContactInvocation *contact{};
        RenderInvocation *render{};
        trace::AnimationSelectionRecord animation_selection{};
        std::uint32_t animation_selection_subject{};
        std::uint32_t pose_scope{};

        // Nested WeaponMelee::Read calls save and restore this correlation scope.
        std::uint32_t read_scope{};
        std::uint32_t predict_scope{};
        ControllerInvocation *controller{};
        ControlsInvocation *controls{};

        struct TriggerCall {
            mutable std::optional<HeroSubject> subject;
            OwnerDecision decision;
            std::uint32_t stack{};
            std::uint32_t scope{};
        };

        std::array<TriggerCall, 8> triggers{};
        std::uint8_t trigger_depth{};
        std::array<std::optional<HeroSubject>, 2> selector_subjects{};
        std::array<std::uint32_t, 2> selector_stack{};
        std::array<std::uint32_t, 2> selector_scope{};
        std::array<std::uint32_t, 2> selector_descriptor{};
        std::array<std::uint32_t, 2> selector_caller{};
        std::uint8_t sprint_route{};
    };

    // A read retains its entry identity and scope through nested setters.
    struct ReadInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::NetworkStateRecord record{};
        std::uint32_t previous_scope{};
    };

    struct SoldierReadInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::TurnContext turn{};
        std::uint32_t scope{};
        trace::BoundaryRecord boundary{};
    };

    struct PredictionReturnInvocation {
        std::uint32_t previous_scope{};
        mutable std::array<std::optional<HeroSubject>, 2> subjects{};
        std::uint32_t scope{};
    };

    struct CameraInvocation {
        mutable std::optional<HeroSubject> subject{};
        void *red_camera{};
        std::uint32_t scope{};
    };

    struct TransitionInvocation {
        TransitionInvocation *previous{};
        std::uint32_t scope{};
        mutable std::optional<HeroSubject> subject{};
        trace::TransitionRecord record{};
        std::uint16_t flags{};
    };

    struct PresentationInvocation {
        mutable std::optional<HeroSubject> subject{};
        std::uint32_t argument0{};
        std::uint32_t argument1{};
        std::uint32_t scope{};
        std::uint32_t previous_scope{};
        std::array<float, 3> requested_position{};
    };

    struct AnimationInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::AnimationAdvanceRecord record{};
    };

    struct SimulationInvocation {
        SimulationInvocation *previous{};
        mutable std::optional<HeroSubject> subject{};
        std::uint32_t scope{};
        void *soldier{};
        float delta{};
    };

    struct CombatInvocation {
        CombatInvocation *previous{};
        std::uint32_t parent_scope{};
        mutable std::optional<HeroSubject> subject{};
        std::uint32_t scope{};
    };

    struct RenderInvocation {
        RenderInvocation *previous{};
        std::uint32_t scope{};
        mutable std::optional<HeroSubject> subject{};
        std::array<trace::SubmissionRecord, 2> submissions{};
        std::uint8_t pending{};
    };

    struct ControllerInvocation {
        ControllerInvocation *previous{};
        mutable std::optional<HeroSubject> subject;
        void *controller{};
        std::uint32_t scope{};
        std::uint32_t parent{};
        float delta{};
        trace::InputUseRecord input{};
    };

    struct ControlsInvocation {
        ControlsInvocation *previous{};
        mutable std::optional<HeroSubject> subject;
        trace::MovementFrameRecord record;
        std::uint32_t scope{};
        std::uint32_t parent{};
        // The next branch destination supplies the native comparison flags before another instruction changes them.
        bool lock_compared{};
    };

    struct MovementInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::MovementFrameRecord record{};
    };

    struct ContactInvocation {
        ContactInvocation *previous{};
        std::uint32_t scope{};
        mutable std::optional<HeroSubject> subject{};
        trace::MeleeContactRecord record{};
    };

    struct LocomotionInvocation {
        mutable std::optional<HeroSubject> subject{};
        trace::LocomotionState before{};
    };

    // Copies a current subject into invocation-owned storage; null remains untracked.
    static std::optional<HeroSubject> snapshot_subject(HeroSubject *subject) noexcept;

    // Retires identities before native reset and destruction can release or reuse their storage.
    void plan_lifetime(fc::Plan &plan, const LifecycleLayout &layout);

    // Role plans declare every native boundary observed by this plugin.
    void plan_client(fc::Plan &plan, const ClientLayout &layout);

    // Declares dedicated server simulation, serialization, and optional deflection observations.
    void plan_server(fc::Plan &plan, const ServerLayout &layout);

    // Observes four scalar velocity-override arguments, adding Authority for the dedicated server.
    void plan_velocity(fc::Plan &plan, const fc::FunctionLocation<VelocityFunction> &location);

    // Observes projectile deflection and samples the same position operand before and after the call.
    void plan_contact(fc::Plan &plan, const fc::FunctionLocation<ContactFunction> &location);

    // Binds the weapon's current player-slot identity and attempts to publish its subject mapping.
    HeroSubject *bind(void *weapon) noexcept;

    // Submits a subject mapping once and returns whether the channel has accepted it.
    bool announce(HeroSubject &subject) noexcept;

    // Creates a payload header with the subject identity and observation flags; FC supplies time and thread.
    trace::RecordHeader record_header(trace::RecordKind kind, std::uint32_t subject, std::uint16_t flags) const noexcept;

    // Submits a complete diagnostic record and returns the runtime queue's acceptance result.
    template <class Payload>
    bool write(trace::RecordKind kind, const Payload &payload, std::uint32_t subject, std::uint16_t flags = trace::None) noexcept {
        static_assert(trace::kValidRecord<Payload>);

        const trace::Record<Payload> record{
            .header = record_header(kind, subject, flags),
            .payload = payload,
        };
        return trace_.write(record);
    }

    // Submits a dependent record after the channel has accepted its subject identity mapping.
    template <class Payload>
    void write_subject(trace::RecordKind kind, const Payload &payload, HeroSubject &subject, std::uint16_t flags = trace::None) noexcept {
        // Under channel pressure, omit dependent records until their subject mapping is accepted.
        if (!announce(subject)) {
            return;
        }

        const auto identity = subject.local_player == trace::kNoLocalPlayer ? trace::Remote : trace::Local;
        write(kind, payload, subject.id, static_cast<std::uint16_t>(flags | identity));
    }

    // Samples native turn counters and pass flags; server context omits client and replay globals.
    trace::TurnContext turn_context() const noexcept;

    // Labels transitions Authority inside a matching topmost setter, otherwise using native pass flags.
    std::uint16_t transition_flags(void *weapon, const trace::TurnContext &turn) noexcept;

    // Copies the soldier's movement flags, energy, and locomotion classification.
    trace::LocomotionState locomotion_state(const void *soldier) const noexcept;

    // Writes before/after states around an energy-gated jump or roll attempt.
    void record_locomotion(HeroSubject &subject, trace::LocomotionOperation operation, const trace::LocomotionState &before,
                           const trace::LocomotionState &after, bool result) noexcept;

    // Starts a nested update and samples its requested timing, combo state and private clocks.
    void begin_update(void *weapon, float requested_delta, float effective_delta, MeleeMode mode, UpdateInvocation &invocation) noexcept;

    // Restores the enclosing update and records its reported decision, observed hook return, and exit state.
    void finish_update(void *weapon, bool result, const UpdateInvocation &invocation) noexcept;

    // Updates the active matching melee invocation with the behavior owner's timing and mode.
    void observe_update_decision(void *weapon, float delta, float effective_delta, MeleeMode mode) noexcept;

    // Starts a nested network setter and captures entry state, masks, flags, and enclosing read scope.
    void begin_network_state(void *weapon, std::int32_t state, bool flag, NetworkMode mode, NetworkInvocation &invocation) noexcept;

    // Restores the enclosing request and writes the applied or suppressed state outcome.
    void finish_network_state(void *weapon, const NetworkInvocation &invocation) noexcept;

    // Updates the active matching state request with the behavior owner's state, flag, and mode.
    void observe_network_state_decision(void *weapon, std::int32_t state, std::uint32_t flag, NetworkMode mode) noexcept;

    // Writes copied owner decisions inside their current native context.
    void observe_history_decision(void *weapon, const HistoryDecision *decision) noexcept;
    void observe_physics_decision(void *object, std::uint8_t soldier, const PhysicsDecision *decision) noexcept;

    // Copies the active same-thread Hero identity without native lookup or mutation.
    HeroScope current_scope() const noexcept;

    CaptureMode capture_mode() const noexcept {
        return settings_.capture;
    }

    // Copies an owner branch and reuses existing queue records at save and restore boundaries.
    void observe_owner_decision(void *weapon, const OwnerDecision *decision) noexcept;

    // Captures one native selector entry shared by requested-phase and installed-lane records.
    void begin_animation_selection(const fc::CpuContext &context, std::size_t lane) noexcept;
    // Copies native action and installed lanes at a requested-action boundary.
    void record_action(HeroSubject &subject, const PresentationInvocation &invocation, std::uint16_t flags) noexcept;
    // Plans selected controller/trigger producers, controls gates and installed animation results.
    void plan_causes(fc::Plan &plan, const DetailLayout &layout);

    // Observes controls with one invocation on both client and server.
    void plan_controls(fc::Plan &plan, const fc::FunctionLocation<ControlsFunction> &location);

    // Returns the existing selected identity only on the thread that first enrolled native melee subjects.
    HeroScope weapon_scope(void *weapon) noexcept;

    // Observes qualified retail condition, queue, dispatch, physics and model boundaries.
    void plan_native_details(fc::Plan &plan, const DetailLayout &layout);
    // Observes actual Soldier simulation, collision and integration boundaries.
    void plan_simulation(fc::Plan &plan, const DetailLayout &layout);
    // Observes body output on every level and reserves detailed submission results for Combat and Full.
    void plan_presentation(fc::Plan &plan, const DetailLayout &layout);
    // Writes a compact Combat state/admission result or Full native operands.
    void write_soldier_decision(HeroSubject &subject, const trace::SoldierDecisionRecord &record,
                                std::uint16_t flags = trace::None) noexcept;
    // Copies qualified basis, hook and collision operands at one native integration cut.
    void record_motion_operands(HeroSubject &subject, const fc::CpuContext &context, std::uint32_t scope, std::uint8_t stage) noexcept;
    // Copies writer or restored orientation and velocity under its native stream or simulation scope.
    void record_orientation(HeroSubject &subject, const void *matrix, const void *quaternion, const void *velocity, std::uint32_t stream,
                            std::uint32_t scope, std::uint8_t stage) noexcept;
    // Observes actual state/admission decisions and scalar serialization without evaluating native predicates again.
    void plan_gameplay_decisions(fc::Plan &plan, const DetailLayout &layout);
    // Samples state clocks, posture and admission resources without reevaluating a predicate.
    trace::SoldierDecisionRecord soldier_decision(const HeroSubject &subject, std::uint8_t stage) const noexcept;
    // Joins native EnergyBar bounds and values to their actual nested bit-codec calls.
    void plan_energy_codec(fc::Plan &plan, const DetailLayout &layout);
    void plan_combat_decisions(fc::Plan &plan, const DetailLayout &layout);
    void record_simulation(HeroSubject &subject, std::uint32_t scope, float delta, std::uint8_t stage, bool result = false) noexcept;
    // Copies occupied rows and omits an identical paired sample's rows with an explicit unchanged flag.
    trace::NativeQueueRecord record_queue(HeroSubject &subject, std::uint8_t stage, std::uint32_t scope, float threshold = 0.0F,
                                          const trace::NativeQueueRecord *previous = nullptr) noexcept;
    void record_dispatch(HeroSubject &subject, std::uint8_t stage, std::uint32_t scope) noexcept;

    // Captures the combo ID and pass or nested-setter label before state entry.
    void begin_enter_state(void *weapon, std::int32_t state, TransitionInvocation &invocation) noexcept;

    // Writes the resulting state, combo clocks and movement after state entry or exit.
    void finish_transition(void *weapon, const TransitionInvocation &invocation) noexcept;

    // Captures entry presentation state and requested combo animation IDs; blend is the full-body argument and is omitted.
    void begin_animator_state(void *animator, std::int32_t state, bool active, std::uint8_t primary, std::uint8_t secondary, bool blend,
                              AnimatorInvocation &invocation) noexcept;

    // Emits saved entry state after return with requested animation IDs, without resampling the animator.
    void finish_animator_state(const AnimatorInvocation &invocation) noexcept;

    // Records a prepared transition sound request for the enclosing EnterState, without a playback result.
    void observe_transition_sound(void *sound, const void *position, const void *velocity, std::uint32_t reserved, bool force) noexcept;

    // Captures the embedded melee input queue and requested buttons at its native entry.
    void begin_input_queue(const fc::CpuContext &context, InputInvocation &invocation) noexcept;

    // Samples held buttons after FC modifiers and before native InputQueue::Update runs.
    void finish_input_queue(const fc::CpuContext &context, const InputInvocation &invocation) noexcept;

    // Captures the proposed combo ID and instruction address before Update's native transition calls.
    void begin_prediction_transition(const fc::CpuContext &context, PredictionInvocation &invocation) noexcept;

    // Emits the unchanged entry state and labels a redirected instruction pointer Suppressed.
    void finish_prediction_transition(const fc::CpuContext &context, const PredictionInvocation &invocation) noexcept;

    // Writes authority entry motion and private clocks and saves the compact comparison sample.
    void before_soldier_read(void *soldier, SoldierReadInvocation &invocation) noexcept;

    // Samples position and velocity after ReadObject only while its saved generation remains live.
    void after_soldier_read(const SoldierReadInvocation &invocation) noexcept;

    // Copies current combo, animation, and timer fields for a requested presentation record kind.
    trace::PresentationFrameRecord presentation_sample(const HeroSubject &subject, trace::PresentationKind kind) const noexcept;

    // Writes the completed parser row for an already tracked remote subject alongside current object samples.
    void observe_parsed_move(const fc::CpuContext &context) noexcept;

    // Writes one selected local subject's motion, dispatch/combo state and relevant animation at a native boundary.
    trace::BoundaryRecord record_local_state(HeroSubject &subject, trace::LocalStage stage, std::uint32_t scope,
                                             const trace::TurnContext &turn, std::uint16_t flags, bool result = false,
                                             const trace::BoundaryRecord *previous = nullptr) noexcept;

    // Copies the qualified native presentation output with the same scope and context as its LocalState sample.
    void record_display(HeroSubject &subject, trace::LocalStage stage, std::uint32_t scope, const float *matrix,
                        const void *source) noexcept;

    // Samples active native processed input; exact turn/origin is unavailable during unqualified replay paths.
    void record_server_move(HeroSubject &subject) noexcept;

    // Access reservations remain owned by the handler; views are borrowed from the mapped game image.
    fc::Memory<const std::int32_t> host_turn_memory_;
    fc::Memory<const std::int32_t> client_turn_memory_;
    fc::Memory<const std::int32_t> update_turn_memory_;
    fc::Memory<const std::int32_t> prediction_turn_memory_;
    fc::Memory<const std::int32_t> acknowledged_turn_memory_;
    fc::Memory<const std::int32_t> destination_memory_;

    fc::Memory<const std::uint8_t> local_prediction_pass_memory_;
    fc::Memory<const std::uint8_t> authoritative_update_pass_memory_;
    fc::Memory<const std::uint8_t> rollback_memory_;

    fc::Memory<const std::uint8_t> network_enabled_memory_;
    fc::Memory<const std::uint8_t> network_client_active_memory_;

    fc::Memory<const float> frame_delta_memory_;

    fc::Memory<const native::PlayerMove> remote_moves_memory_;
    fc::Memory<const native::PlayerMove *const> active_inputs_memory_;

    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;

    Settings settings_;
    fc::Role role_{};

    // Callers must serialize subject-table access and keep borrowed native objects alive during sampling.
    SubjectTable subjects_;
    std::atomic_uint32_t subject_thread_{};

    AnimatorOffsets animator_{};

    fc::TraceChannel trace_;
    network_diagnostics::NetworkDiagnosticsApi network_diagnostics_{};
    std::atomic_bool network_diagnostics_ready_{};

    // Handlers in this module share invocation chains on each OS thread; this storage does not synchronize the subject table.
    static thread_local ThreadState thread_state_;
    std::atomic_uint32_t next_scope_{};
    std::vector<trace::AssetNameRecord> asset_names_;

    // Planned pointers borrow game image storage for the installed patch lifetime.
    const std::int32_t *host_turn_{};
    const std::int32_t *client_turn_{};
    const std::int32_t *update_turn_{};
    const std::int32_t *prediction_turn_{};
    const std::int32_t *acknowledged_turn_{};
    const std::int32_t *destination_{};

    const std::uint8_t *local_prediction_pass_{};
    const std::uint8_t *authoritative_update_pass_{};
    const std::uint8_t *rollback_{};

    const std::uint8_t *network_enabled_{};
    const std::uint8_t *network_client_active_{};

    const float *frame_delta_{};

    const native::PlayerMove *remote_moves_{};
    const native::PlayerMove *const *active_inputs_{};
};

} // namespace fc_plugins::hero_diagnostics
