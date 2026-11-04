#pragma once

#include "layout.hpp"
#include "patch.hpp"
#include "schema.hpp"
#include "api.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace fc_plugins::network_diagnostics {

// Tracks native observations, nested transactions, and bounded trace records for one game image.
class Handler {
  public:
    using Settings = network_diagnostics::Settings;

    // Receives immutable settings and the role selected by the factory variant.
    Handler(const Settings &settings, fc::Role role) noexcept;

    // Returns the startup capture tier for optional producers.
    std::uint32_t capture_mode() const noexcept {
        return static_cast<std::uint32_t>(settings_.capture);
    }

    // Declares non-owning observations for the selected role and executable profile.
    void plan(fc::Plan &plan);

    // Creates the bounded trace before native callbacks can run.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Reports records accepted by ETW and records dropped by the channel.
    void write_status(fc::StatusContext &context) const noexcept;

  private:
    using DiagnosticsApi = NetworkDiagnosticsApi;

    // Carries timing and selection state from before EntitySoldier::ReadObject to its matching after callback.
    struct SoldierReadState {
        std::uint64_t started;
        bool tracked;
    };

    static_assert(std::is_trivial_v<SoldierReadState>);

    // The SDK initializes each invocation separately; previous pointers restore aggregates after nested calls.
    struct GroupInvocation {
        GroupInvocation *previous{};
        trace::NativeSendGroupRecord record{};
        std::uint64_t start{};
    };

    struct SendInvocation {
        SendInvocation *previous{};
        bool transport_reported{};
        trace::GroupIdentity identity{};
    };

    struct ReceiveInvocation {
        ReceiveInvocation *previous{};
        trace::TransportReceiveRecord record{};
        std::uint64_t start{};
    };

    struct ReadinessInvocation {
        trace::ReadinessRecord record{};
        std::uint64_t start{};
    };

    struct ReplayInvocation {
        ReplayInvocation *previous{};
        trace::LateReplayRecord record{};
        std::uint64_t start{};
        std::uint64_t copied_players{};
    };

    struct QueueInvocation {
        trace::QueuePacketRecord record{};
    };

    struct CreateResponseInvocation {
        trace::CreateResponseRecord record{};
    };

    struct AckInvocation {
        trace::UpdateAckRecord record{};
        std::uint32_t before_count{};
    };

    struct ReplicationInvocation {
        trace::ReplicationObjectRecord record{};
    };

    struct ObjectInvocation {
        ObjectInvocation *previous{};
        trace::ReplicationObjectRecord record{};
    };

    struct EventInvocation {
        EventInvocation *previous{};
        trace::ReplicationEventRecord record{};
    };

    struct TimedInvocation {
        std::uint64_t start{};
    };

    struct ClientReceiveInvocation {
        ClientReceiveInvocation *previous{};
        trace::ClientReceivePassRecord record{};
        std::uint64_t start{};
    };

    struct MoveSubmissionInvocation {
        std::uint64_t start{};
        std::int32_t turn_before{-1};
    };

    struct ReadUpdateInvocation {
        ReadUpdateInvocation *previous{};
        trace::UpdateReadRecord record{};
        std::uint32_t bits{};
        std::uint32_t ordinary_ordinal{};
        std::uint64_t start{};
    };

    struct PredictionInvocation {
        PredictionInvocation *previous{};
        trace::PredictionRecord record{};
        std::uint64_t start{};
        std::int32_t turn_before{-1};
    };

    struct SoldierUpdateInvocation {
        trace::SoldierPostureRecord record{};
    };

    struct ClockInvocation {
        trace::ClockAdjustmentRecord record{};
        trace::ClockPhaseRecord phase{};
    };

    struct LagInvocation {
        trace::HostLagRecord record{};
    };

    struct ReliableInvocation {
        std::uint64_t start{};
        std::uint32_t frontier_before{};
    };

    struct ReceiveHostInvocation {
        std::uint64_t start{};
        std::int32_t turn{-1};
        std::uint64_t required{};
        std::uint64_t received_before{};
    };

    struct SaveLateInvocation {
        std::uint64_t start{};
    };

    struct SendHostInvocation {
        SendHostInvocation *previous{};
        std::uint64_t start{};
    };

    struct InputInvocation {
        InputInvocation *previous{};
        trace::ServerInputRecord record{};
        std::uint64_t start{};
    };

    struct DestinationInvocation {
        DestinationInvocation *previous{};
        trace::DestinationUpdateRecord record{};
        std::uint32_t packet{};
        std::uint32_t bits{};
        trace::HostLagRecord lag{};
        bool lag_known{};
    };

    struct WriteUpdateInvocation {
        DestinationInvocation destination_state{};
        std::uint64_t start{};
        std::int32_t destination{-1};
    };

    struct FillTurnsInvocation {
        const std::uint32_t *counter{};
        std::uint32_t before{};
    };

    struct OrdnanceInvocation {
        std::uint32_t event_head{};
    };

    struct ControllerInvocation {
        trace::ControllerStateRecord record{};
        void *controllable{};
        std::uint64_t start{};
    };

    struct FireInvocation {
        FireInvocation *previous{};
        trace::WeaponFireRecord record{};
        std::uint64_t start{};
    };

    struct ProjectileInvocation {
        ProjectileInvocation *previous{};
        trace::ProjectileSimulationRecord record{};
        std::uint32_t ray_depth{};
        std::uint64_t start{};
    };

    struct DamageInvocation {
        trace::DamageRecord record{};
        std::uint64_t start{};
    };

    // Selection rotates through the previous pass's pose count; changing counts can repeat or skip objects.
    struct PoseSelection {
        std::uint32_t observed{};
        std::uint32_t previous_count{};
        std::uint32_t selected{};
        std::uint64_t sequence{};
    };

    // Authoritative samples retain their payload until ReadUpdate completes.
    struct PoseSampler : PoseSelection {
        trace::ClientPoseRecord record{};
        bool captured{};
    };

    // Current masks and policy sampled after SendHost, independent of the actual IsTurnReady result.
    struct ReadinessSnapshot {
        std::int32_t host_turn{-1};

        std::uint64_t required_mask{};

        std::uint64_t missing_mask{};

        std::uint32_t grace_turns{};
        bool waitlate{};
        bool network_enabled{};
    };

    // One server frame owns its turn and scheduling aggregates; nested calls keep their own records.
    struct ServerState {
        trace::ServerFrameRecord frame{};
        trace::AuthorityTurnRecord turn{};
        trace::SchedulingRecord scheduling{};

        std::uint64_t frame_start{};
        std::uint64_t save_end{};

        std::uint64_t last_policy{};
        bool has_policy{};
        bool frame_active{};
        bool turn_active{};
    };

    // Each thread owns its observations; invocation pointers refer to SDK state active on that thread.
    struct ThreadState {
        GroupInvocation *group{};
        std::uint32_t intake_depth{};
        AckInvocation *ack{};
        CreateResponseInvocation *create_response{};
        PredictionInvocation *prediction{};
        trace::VelocityCodecRecord velocity{};
        trace::VelocityBoundsRecord velocity_bounds{};
        std::uint32_t velocity_pointer{};
        std::uint32_t velocity_primitive{};
        bool velocity_active{};
        trace::SchedulingDecisionRecord candidate{};
        bool candidate_pending{};
        std::array<std::uint32_t, 2> pipe_full{};
        std::array<trace::SchedulingDecisionRecord, native::kPlayerCapacity> schedule_last{};
        std::array<bool, native::kPlayerCapacity> schedule_seen{};
        ReadinessInvocation *readiness{};
        ReplayInvocation *replay{};
        trace::InputDispositionRecord input_row{};
        bool input_row_pending{};
        trace::GroupIdentity input_group{};
        std::uint32_t encoded_turn{};
        bool encoded_turn_known{};
        trace::ReceiveWindowRecord receive_window{};
        bool receive_window_open{};
        std::uint32_t empty_windows{};
        std::uint32_t drain_ordinal{};
        std::uint32_t receive_pass_ordinal{};
        trace::ClientReceivePassRecord empty_client_receive{};
        std::uint64_t last_empty_client_entry{};
        trace::ClockAdjustmentRecord clock_adjustment{};
        bool clock_adjustment_seen{};
        std::array<std::array<std::uint32_t, native::kPlayerCapacity>, native::kTurnCapacity> source_turns{};
        std::array<std::array<bool, native::kPlayerCapacity>, native::kTurnCapacity> source_known{};
        std::array<std::array<std::uint32_t, native::kPlayerCapacity>, native::kTurnCapacity> source_generation{};
        std::array<std::array<trace::InputSource, native::kPlayerCapacity>, native::kTurnCapacity> source_types{};
        std::array<trace::ReplicationEventRecord, 128> ordinary_events{};
        std::array<std::uint32_t, 128> ordinary_generations{};
        std::array<std::uint32_t, 128> ordinary_created_turns{};
        std::array<std::uint32_t, 128> reliable_generations{};
        std::array<std::uint32_t, 128> reliable_created_turns{};
        std::uint32_t ordinary_first{};
        std::uint32_t ordinary_head{};
        bool ordinary_active{};
        std::uint32_t ordinary_ordinal{};
        trace::ReplicationEventRecord reliable_event{};
        EventInvocation *decoded_event{};
        ReplicationInvocation *object_serializer{};
        ObjectInvocation *object_reader{};
        ObjectInvocation *object_selection{};
        std::array<std::uint32_t, native::kPlayerCapacity> active_source_turns{};
        std::array<bool, native::kPlayerCapacity> active_source_known{};
        const void *selected_objects{};
        std::uint32_t selected_object_count{};
        std::uint32_t object_wire_id{};
        std::uint32_t object_map{};
        std::uint32_t object_type{};
        SendInvocation *send{};
        ReceiveInvocation *receive{};
        ClientReceiveInvocation *client_receive{};
        ReadUpdateInvocation *read_update{};
        SendHostInvocation *send_host{};
        InputInvocation *input{};
        DestinationInvocation *destination{};
        FireInvocation *fire{};
        ProjectileInvocation *projectile{};

        PoseSampler authority_pose;
        PoseSelection presentation_pose;

        ServerState server;

        // Borrowed Controllable subobjects seen in this frame; delayed pose reads require them to remain alive.
        std::array<void *, native::kPlayerCapacity> soldiers{};

        std::uint64_t last_frame{};
        std::uint64_t last_receive_service{};
        std::uint64_t receive_maximum_gap{};
        std::uint32_t empty_receive_calls{};
        std::array<std::uint32_t, 30> empty_group_polls{};
        trace::DirectPumpRecord direct_pump{};
        // Existing native destinations bound repeat aggregates; peer replacement changes the comparison key.
        std::array<trace::DestinationGateRecord, native::kPlayerCapacity> gates{};
        std::array<bool, native::kPlayerCapacity> gate_seen{};
        std::array<trace::HostLagRecord, native::kPlayerCapacity> lag_summaries{};
        std::array<bool, native::kPlayerCapacity> lag_seen{};

        std::uint32_t clock_episode{};
        bool reset_pending{};
        bool presentation_pending{};
    };

    // Declares qualified native input, queue, scheduling and replication decision observations.
    void plan_pipeline(fc::Plan &plan);

    // Separates coarse pacing, game work, background wait and network flush in each outer frame.
    void plan_frame_timing(fc::Plan &plan);
    // Observes native Soldier velocity operands and actual primitive codes within update bodies.
    void plan_velocity(fc::Plan &plan);
    void plan_replication(fc::Plan &plan);
    std::uint32_t selected_object_priority(std::uint32_t object) const noexcept;
    void observe_send_storage(const SendStorageObservation *observation) noexcept;
    std::uint32_t codec_position(std::uint32_t packet, std::uint32_t encoding) noexcept;
    void observe_create_transition(const CreateFenceObservation *observation) noexcept;
    void observe_read_object(const fc::CpuContext &context, bool special) noexcept;
    void observe_event_gate(std::uint32_t gate, const fc::CpuContext &context) noexcept;
    void begin_velocity(const fc::CpuContext &context, bool encoding) noexcept;
    void finish_velocity() noexcept;
    // Copies ordinary Soldier posture and turn/pass state without enrolling Hero subjects.
    trace::SoldierPostureRecord soldier_posture(void *soldier, std::uint32_t stage) const noexcept;
    // Packs the four qualified native world/pass bytes in their declared order.
    std::uint32_t world_pass() const noexcept;
    void observe_velocity_bits(std::uint32_t value, std::uint32_t bits) noexcept;
    // Records one native candidate's final refusal or eligibility and coalesces unchanged checks per output pass.
    void finish_scheduling_candidate() noexcept;

    // Captures a native row before ReadMove and emits it after the actual acceptance branches.
    void begin_input_row(const fc::CpuContext &context, bool mini) noexcept;
    void finish_input_row() noexcept;

    // Copies one exact codec turn from its native SendPrimary caller frame.
    void observe_encoded_turn(const fc::CpuContext &context, bool mini) noexcept;

    // Closes the preceding native turn window and opens the next one at an input-frontier change.
    void advance_receive_window() noexcept;
    void close_receive_window() noexcept;
    void finish_scheduling_summary(std::uint32_t destination) noexcept;

    // Copies a producer's actual socket send boundary and communication identity.
    void observe_direct_send(const DirectSendObservation *observation) noexcept;

    // Records a recovery opportunity synchronously without invoking native intake or parsing.
    void observe_recovery_drain(const RecoveryDrainObservation *observation) noexcept;

    // Declares client turn, prediction, codec, and pose observations for the selected image.
    void plan_client(fc::Plan &plan, const ClientLayout &layout);

    // Declares the dedicated server's authority and output observations.
    void plan_server(fc::Plan &plan, const ServerLayout &layout);

    // Declares actual reset and presentation observations for Full client capture.
    void plan_client_clock(fc::Plan &plan, const ClientClockLayout &layout);

    // Declares the native estimator result and serialized lag observations on the host.
    void plan_host_lag(fc::Plan &plan, const HostLagLayout &layout);

    // Aggregates empty receive opportunities below Full, retaining call bounds and timing extremes.
    void observe_client_receive(const trace::ClientReceivePassRecord &record) noexcept;
    // Records changed clock decisions and bounds exact repeats by count and QPC time.
    void observe_clock_adjustment(trace::ClockAdjustmentRecord record) noexcept;
    // Flushes pending client aggregates at the native connection/map boundary.
    void flush_client_progress() noexcept;

    // Copies current native clocks/frontiers without calling any native time getter again.
    trace::ClockPhaseRecord clock_phase(trace::ClockPhase phase) const noexcept;

    // Returns one prepared native clock value, in seconds except for the interpolation fraction.
    float clock_value(ClockField field) const noexcept;

    // Emits changed destination policy/results and summarizes unchanged writes at a bounded interval.
    void observe_serialized_lag(std::uint32_t biased_value) noexcept;

    // Declares weapon, projectile, ray-query, and damage observations for combat capture.
    void plan_combat(fc::Plan &plan, const CombatLayout &layout);

    // Returns module TLS for the single installed Handler, which remains alive until process exit.
    static ThreadState &thread_state() noexcept;

    // Reads ordered RDTSCP ticks for durations on this machine; QPC frequency does not convert them to seconds.
    std::uint64_t stamp() const noexcept;

    // Builds the record identity; the runtime envelope supplies QPC, sequence and thread.
    trace::RecordHeader record_header(trace::RecordKind kind, std::uint32_t subject, std::uint16_t flags,
                                      trace::Carrier carrier) const noexcept;

    // Submits one complete diagnostic record to the bounded runtime channel.
    template <class Payload>
    void write(trace::RecordKind kind, const Payload &payload, std::uint32_t subject = 0, std::uint16_t flags = trace::None,
               trace::Carrier carrier = trace::Carrier::Unknown) noexcept {
        static_assert(trace::kValidRecord<Payload>);

        const trace::Record<Payload> record{
            .header = record_header(kind, subject, flags, carrier),
            .payload = payload,
        };
        trace_.write(record);
    }

    // Starts a nested packet-group aggregate for the destination.
    void begin_group(std::int32_t destination, GroupInvocation &invocation) noexcept;

    // Restores the enclosing group and writes this group's elapsed time and send totals.
    void finish_group(std::int32_t destination, const GroupInvocation &invocation) noexcept;

    // Tracks one nested send attempt so a transport producer can report its selected carrier.
    void begin_send(const void *bytes, std::int32_t length, SendInvocation &invocation) noexcept;

    // Restores the enclosing send and reports the native outcome when no transport decision was supplied.
    void finish_send(std::int32_t destination, std::uint32_t size, std::int32_t result, const SendInvocation &invocation) noexcept;

    // Adds the send outcome to the active group, or writes a standalone packet record.
    void observe_send(std::int32_t destination, std::uint32_t size, PacketCarrier carrier, std::int32_t result) noexcept;

    // Starts a nested receive pass and collects native and direct packet totals.
    void begin_receive(ReceiveInvocation &invocation) noexcept;

    // Restores the enclosing receive pass and emits intake totals or a bounded empty-service aggregate.
    void finish_receive(const ReceiveInvocation &invocation) noexcept;

    // Counts native intake handoffs without testing acceptance, or records the source pointer separately.
    void observe_intake(void *endpoint) noexcept;

    // Counts Direct intake as a subset of the observed total; standalone detail comes from direct_receive.
    void observe_direct_intake(void *endpoint) noexcept;

    // Records the requested native destination before or after disconnect.
    void observe_disconnect(std::int32_t destination, std::uint32_t after) noexcept;

    // Records Create's policy, delimits clock phases on its time rebase, and clears route summaries when resetting.
    void observe_network_create(bool preserve) noexcept;

    // Writes a packet-group retrieval attempt with its packet type, completion result, and elapsed time.
    void finish_receive_group(void *group, std::int32_t packet_type, bool complete, std::uint64_t start) noexcept;

    // Copies the complete group's first pool packet and native communication identity before parsing.
    void identify_group(void *group, trace::UpdateReadRecord &record) noexcept;

    // Marks the current send attempt as reported and records the producer's carrier and result.
    void observe_transport_send(std::int32_t destination, std::uint32_t size, PacketCarrier carrier, std::int32_t result) noexcept;

    // Copies one borrowed association transition into a standalone trace record.
    void observe_direct_association(const DirectAssociationObservation *observation) noexcept;

    // Retains each Direct association and sequence in detailed capture and counts injected Data in a receive pass.
    void observe_direct_receive(const DirectReceiveObservation *observation) noexcept;

    // Emits nonempty socket servicing or up to 128 accumulated empty pumps.
    void observe_direct_pump(const DirectPumpObservation *observation) noexcept;

    // Copies raw arrivals and final dispositions in detailed capture without retaining payloads.
    void observe_direct_datagram(const DirectDatagramObservation *observation) noexcept;
    // Writes the socket's cached buffer configuration and option errors.
    void observe_direct_socket(const DirectSocketObservation *observation) noexcept;

    // Selects the authoritative soldier pose sample and starts its decode timer.
    void before_soldier_read(void *soldier, void *stream, void *object_state, SoldierReadState &state) noexcept;

    // Copies every completed crouch/state receipt and the selected authoritative pose.
    void after_soldier_read(void *soldier, void *stream, void *object_state, const SoldierReadState &state) noexcept;

    // Counts gate bookkeeping and emits changed or bounded repeated actual destination decisions.
    void observe_destination_gate(const DestinationGateObservation *observation) noexcept;

    // Stores the active policy's grace turns for subsequent native readiness samples.
    void observe_waitlate_grace(std::uint32_t turns) noexcept;

    // Chooses the next authoritative pose using the previous update's decoded soldier count.
    void begin_authoritative_update(ThreadState &state) noexcept;

    // Remembers the decoded soldier count and writes the selected authoritative pose.
    void finish_authoritative_update(ThreadState &state) noexcept;

    // Advances presentation pose selection using the previous frame's observed object count.
    void begin_presentation_frame(ThreadState &state) noexcept;

    // Counts an observed pose and returns whether it is this pass's selected sample.
    static bool select_pose(PoseSelection &sampler) noexcept;

    // Returns the player and turn-slot indices of a move in the selected role's history, or {-1, -1}.
    std::pair<std::int32_t, std::int32_t> identify_move(const void *move) const noexcept;

    // Writes move codec inputs, elapsed time, and history identity; server temporaries use the receiving destination.
    void record_move(trace::RecordKind kind, void *packet, const void *previous, const void *move, trace::MoveCodecOperation operation,
                     std::int32_t reference, std::uint64_t start) noexcept;

    // Resets server timing and scheduling aggregates for a new frame, retaining the last reported policy.
    void begin_server_frame(ThreadState &state, std::uint64_t now) noexcept;

    // Returns the player input mask for a turn retained in the server history.
    std::uint64_t received_mask(std::int32_t turn) const noexcept;

    // Snapshots server policy and the required and missing player inputs for the host turn.
    ReadinessSnapshot readiness() const noexcept;

    // Packs readiness policy inputs into a comparison key for change detection.
    static std::uint32_t policy_bits(const ReadinessSnapshot &snapshot) noexcept;

    // Writes the current server policy when its comparison key changes.
    void write_policy_if_changed(ThreadState &state, const ReadinessSnapshot &snapshot) noexcept;

    // Starts a nested destination update and snapshots its ordinary event cursor.
    void begin_destination_update(ThreadState &state, std::int32_t destination, DestinationInvocation &invocation) noexcept;

    // Restores the enclosing destination and writes ordinary event cursor changes.
    void finish_destination_update(ThreadState &state, const DestinationInvocation &invocation) noexcept;

    // Writes current server soldier poses in batches of three for the authority turn.
    void write_authoritative_poses(ThreadState &state, std::int32_t turn) noexcept;

    // Snapshots damage inputs and the active projectile before either native damage overload runs.
    void begin_damage(void *damageable, const void *descriptor, std::uint32_t flags, trace::DamageKind kind,
                      DamageInvocation &invocation) noexcept;

    // Writes the target's alive result and post-call health and shield state.
    void finish_damage(void *damageable, bool result, const DamageInvocation &invocation) noexcept;

    // Prepare resolves these access reservations to borrowed views in the mapped game image.
    fc::Memory<const std::byte> ordinary_ring_memory_;
    fc::Memory<const std::byte> reliable_ring_memory_;
    fc::Memory<const std::uint32_t> reliable_head_memory_;
    fc::Memory<const std::uint32_t> reliable_free_memory_;
    fc::Memory<const std::uint32_t> object_list_memory_;
    fc::Memory<const std::uint32_t> object_count_memory_;
    fc::Memory<const std::byte> readiness_roles_memory_;
    fc::Memory<const std::uint64_t> late_masks_memory_;
    fc::Memory<const std::uint32_t> active_inputs_memory_;
    fc::Memory<const std::byte> player_states_memory_;
    fc::Memory<const std::uint64_t> previous_required_memory_;
    fc::Memory<const float> pipeline_time_memory_;
    fc::Memory<const std::uint32_t> replay_turn_memory_;
    fc::Memory<const float> pipe_bits_memory_;
    fc::Memory<const std::uint32_t> bandwidth_memory_;
    fc::Memory<const std::uint32_t> allowance_memory_;
    fc::Memory<const std::uint8_t> scalar_raw_memory_;
    fc::Memory<const std::uint32_t> direction_mode_memory_;
    fc::Memory<const float> velocity_factor_memory_;
    fc::Memory<const std::int32_t> maximum_update_steps_memory_;
    fc::Memory<const std::int32_t> simulation_prediction_turn_memory_;
    std::array<fc::Memory<const std::uint8_t>, 4> world_pass_memory_;
    fc::Memory<const std::int32_t> host_turn_memory_;
    fc::Memory<const std::int32_t> client_turn_memory_;
    fc::Memory<const std::int32_t> update_turn_memory_;
    fc::Memory<const std::int32_t> predict_turn_memory_;
    fc::Memory<const std::int32_t> local_predict_turn_memory_;
    fc::Memory<const native::PlayerMove> move_ring_memory_;

    fc::Memory<const float> accumulator_memory_;

    fc::Memory<const std::int32_t> adjust_time_memory_;
    fc::Memory<const std::int32_t> stable_count_memory_;
    fc::Memory<const float> time_scale_memory_;
    fc::Memory<const std::int32_t> client_lag_memory_;
    fc::Memory<const std::uint32_t> reliable_frontier_memory_;

    std::array<fc::Memory<const float>, static_cast<std::size_t>(ClockField::Count)> clock_values_memory_;
    fc::Memory<const std::int32_t> acknowledged_turn_memory_;
    fc::Memory<const std::int32_t> lagversion_memory_;
    fc::Memory<const std::uint8_t> drop_lag_jumps_memory_;

    fc::Memory<const float> simulation_delta_memory_;
    fc::Memory<const float> fixed_delta_memory_;

    fc::Memory<const std::uint64_t> required_mask_memory_;
    fc::Memory<const std::uint64_t> received_masks_memory_;

    fc::Memory<const std::int32_t> current_destination_memory_;

    fc::Memory<const std::uint32_t> event_head_memory_;

    fc::Memory<const std::uint8_t> network_enabled_memory_;
    fc::Memory<const std::uint8_t> waitlate_memory_;
    fc::Memory<const std::uint32_t> lead_turns_memory_;

    std::array<fc::Memory<const std::uint32_t>, native::kPlayerCapacity> event_cursors_memory_;
    std::array<fc::Memory<const std::uint32_t>, native::kPlayerCapacity> filled_turn_counts_memory_;

    fc::Memory<const std::uint32_t> soldier_controllable_vtable_memory_;

    Settings settings_;
    fc::Role role_{};

    ClientNativeLayout client_native_{};
    ServerNativeLayout server_native_{};
    CombatNativeLayout combat_native_{};

    fc::TraceChannel trace_;
    fc::NativeFunction<GetPacket> get_packet_;
    fc::NativeFunction<native::GetJoystickIndex> get_joystick_index_;
    const std::int32_t *maximum_update_steps_{};
    const std::int32_t *simulation_prediction_turn_{};
    std::array<const std::uint8_t *, 4> world_pass_{};

    // Client and server fields below are populated only for the matching role.
    const std::byte *ordinary_ring_{};
    const std::byte *reliable_ring_{};
    const std::uint32_t *reliable_head_{};
    const std::uint32_t *reliable_free_{};
    const std::uint32_t *object_list_{};
    const std::uint32_t *object_count_{};
    const std::byte *readiness_roles_{};
    const std::uint64_t *late_masks_{};
    const std::uint32_t *active_inputs_{};
    const std::byte *player_states_{};
    const std::uint64_t *previous_required_{};
    const float *pipeline_time_{};
    const std::uint32_t *replay_turn_{};
    const float *pipe_bits_{};
    const std::uint32_t *bandwidth_{};
    const std::uint32_t *allowance_{};
    const std::uint8_t *scalar_raw_{};
    const std::uint32_t *direction_mode_{};
    const float *velocity_factor_{};
    const std::int32_t *host_turn_{};
    const std::int32_t *client_turn_{};
    const std::int32_t *update_turn_{};
    const std::int32_t *predict_turn_{};
    const std::int32_t *local_predict_turn_{};
    const native::PlayerMove *move_ring_{};

    const float *accumulator_{};

    const std::int32_t *adjust_time_{};
    const std::int32_t *stable_count_{};
    const float *time_scale_{};
    const std::int32_t *client_lag_{};
    const std::uint32_t *reliable_frontier_{};

    std::array<const float *, static_cast<std::size_t>(ClockField::Count)> clock_values_{};
    const std::int32_t *acknowledged_turn_{};
    const std::int32_t *lagversion_{};
    const std::uint8_t *drop_lag_jumps_{};

    const float *simulation_delta_{};
    const float *fixed_delta_{};

    const std::uint64_t *required_mask_{};
    const std::uint64_t *received_masks_{};

    const std::int32_t *current_destination_{};

    const std::uint32_t *event_head_{};
    std::array<const std::uint32_t *, native::kPlayerCapacity> event_cursors_{};

    std::array<const std::uint32_t *, native::kPlayerCapacity> filled_turn_counts_{};

    const std::uint8_t *network_enabled_{};
    const std::uint8_t *waitlate_{};
    std::atomic_uint32_t waitlate_grace_{3};
    const std::uint32_t *lead_turns_{};

    const void *soldier_controllable_vtable_{};
};

} // namespace fc_plugins::network_diagnostics
