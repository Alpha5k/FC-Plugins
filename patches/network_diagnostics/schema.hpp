#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fc_plugins::network_diagnostics::trace {

// This version describes the diagnostic record header and payload set carried by the runtime ETW events.
inline constexpr std::uint32_t kSchemaVersion = 21;

// The runtime queues a 24-byte QPC/sequence/thread/length envelope before each payload.
// This budget holds 8192 maximum-sized records, including that framing.
inline constexpr std::size_t kBufferBytes = 8192U * (128U + 24U);
inline constexpr std::uint32_t kMaximumRecordSize = 128;

enum class RecordKind : std::uint16_t {
    ClientFrame = 1,
    ServerFrame = 2,
    MoveSubmission = 3,
    ClientReceivePass = 4,
    TransportReceivePass = 5,
    ServerInputPass = 6,
    UpdateRead = 7,
    Prediction = 8,
    ClockAdjustment = 9,
    AuthorityTurn = 10,
    SendPass = 11,
    DestinationUpdate = 12,
    NativeSendGroup = 13,
    NativeReceiveGroup = 14,
    NativePacket = 15,
    Connection = 16,
    MoveEncoding = 17,
    MoveDecoding = 18,
    ReliableEventBatch = 21,
    OrdnanceEvent = 22,
    WeaponFire = 23,
    ControllerState = 24,
    ProjectileBuild = 25,
    ProjectileSimulation = 26,
    Damage = 27,
    ClientSoldierPose = 28,
    ServerSoldierPose = 29,
    NetworkPolicy = 30,
    DirectAssociation = 31,
    DirectReceive = 33,
    DirectPump = 34,
    DestinationGate = 35,
    HostLag = 36,
    ClockPhase = 37,
    InputHeader = 39,
    InputDisposition = 40,
    PreparedInput = 41,
    Readiness = 42,
    LateReplay = 43,
    DirectSend = 44,
    ReceiveWindow = 45,
    QueuePacket = 46,
    RecoveryDrain = 47,
    SchedulingDecision = 48,
    UpdateAck = 49,
    ReplicationEvent = 50,
    ReplicationObject = 51,
    VelocityCodec = 52,
    InputSample = 53,
    CreateResponse = 54,
    VelocityBounds = 55,
    SendStorage = 57,
    CreateFence = 58,
    FrameTiming = 56,
    SoldierPosture = 59,
    DirectDatagram = 61,
    DirectSocket = 62,
    PredictionProgress = 63,
    UpdateAcceptance = 64,
    ReadinessResult = 65,
};

enum class Carrier : std::uint8_t {
    Native = 1,
    Direct = 2,
    NotSent = 3,
    Unknown = 4
};

// Flags describe each observed boundary; Accepted and Rejected have record-specific meanings.
enum RecordFlags : std::uint16_t {
    None = 0,
    Begin = 1U << 0U,
    End = 1U << 1U,
    Accepted = 1U << 2U,
    Rejected = 1U << 3U,
    Anomaly = 1U << 4U,
    Combat = 1U << 5U,
    Complete = 1U << 6U,
    Empty = 1U << 7U,
    Partial = 1U << 8U,
};

enum class DirectAssociationPhase : std::uint8_t {
    Started = 1,
    Terminal = 2,
    Ended = 3
};

enum class DirectRoute : std::uint8_t {
    Unclassified = 1,
    Negotiating = 2,
    Native = 3,
    Direct = 4
};

enum class DirectRouteReason : std::uint8_t {
    None = 0,
    OfferTimeout = 1,
    ProofTimeout = 2,
    ReadyTimeout = 3,
    CommitTimeout = 4,
    SocketUnavailable = 5,
    EndpointUnavailable = 6,
    Commit = 7,
    ImplicitCommit = 8,
    RandomGenerationFailed = 9,
};

enum class AssociationEndReason : std::uint8_t {
    None = 0,
    Disconnected = 1,
    Replaced = 2,
    PolicyRemoval = 3,
    Shutdown = 4,
};

enum class ConnectionOperation : std::uint32_t {
    None = 0,
    DisconnectBegin = 1,
    DisconnectEnd = 2,
    CreateReset = 3,
    CreatePreserve = 4,
};

enum class MoveCodecOperation : std::uint32_t {
    None = 0,
    EncodeMove = 1,
    EncodeMiniMove = 2,
    DecodeMove = 3,
    DecodeMiniMove = 4,
};

// Authoritative follows soldier decoding; SmoothingHistory copies a matrix published for later interpolation.
enum class ClientPoseStage : std::uint32_t {
    None = 0,
    Authoritative = 1,
    SmoothingHistory = 2
};

enum class DamageKind : std::uint8_t {
    Direct = 1,
    Impact = 2
};

#pragma pack(push, 1)

// Event chronology comes from the runtime QPC/sequence/thread envelope.
// Payload durations and gaps use RDTSCP ticks unless explicitly documented as QPC; native float times use seconds.
struct RecordHeader {
    // Kind-specific subject, often an x86 pointer or native turn; addresses and player slots can be reused.
    std::uint32_t subject{};
    RecordKind kind{};
    std::uint16_t flags{};
    Carrier carrier{Carrier::Unknown};
};

// Existing communication identity; the sequence wraps at 16 bits and is scoped to a source, type and connection lifetime.
struct GroupIdentity {
    std::uint16_t sequence{};
    std::uint8_t packet_type{};
    std::uint8_t fragment_index{};
    std::uint8_t fragment_count{};
    std::uint8_t valid{};
    std::int32_t source_slot{-1};
};

// Counts final-send attempts nested in one group call; bytes sum requested native payload lengths.
struct NativeSendGroupRecord {
    std::int32_t destination{};

    std::uint32_t packets{};
    std::uint32_t bytes{};

    std::uint32_t group{};
    std::int32_t packet_type{};
    std::uint32_t host_turn{};
    std::uint32_t native_packets{};
    std::uint32_t direct_packets{};
    // Negative results count as failures; zero results can include an unavailable native transport.
    std::uint32_t failed_packets{};
    GroupIdentity identity{};

    std::uint64_t duration{};
    // QPC brackets the native group call, including fragmentation and nested receive work.
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
};

// Aggregates intake handoffs and authenticated Direct data submissions during one DoRecvs call.
struct TransportReceiveRecord {
    // Total attempted native intake; Direct intake is the subset below, never an additional carrier total.
    std::uint32_t total_packets{};
    std::uint32_t direct_packets{};
    std::uint32_t direct_data_packets{};

    // -1 means no Direct data; -2 means mixed associations, with peer and connection IDs cleared.
    std::int32_t direct_slot{-1};
    std::uint64_t direct_peer_id{};
    std::uint32_t direct_connection_id{};

    // First and last arrivals can belong to different associations and need not bound a contiguous sequence.
    std::uint32_t first_direct_sequence{};
    std::uint32_t last_direct_sequence{};
    // Native player slots 0-63 contribute bits; the host slot 64 is excluded.
    std::uint64_t direct_player_mask{};

    std::uint64_t duration{};
    // Consecutive DoRecvs entry gap and maximum gap since the last emitted pass, in RDTSCP ticks.
    std::uint64_t service_gap{};
    std::uint64_t maximum_service_gap{};
    std::uint32_t empty_calls{};
};

// Standalone send attempt or intake handoff; intake stores only the source pointer identity.
struct NativePacketRecord {
    std::uint32_t endpoint{};
    std::int32_t destination{};
    std::uint32_t bytes{};
    std::int32_t result{};
    std::uint32_t group{};
    GroupIdentity identity{};
};

// Marks requested Create policy and native Disconnect boundaries; pose sampling sequences continue.
struct ConnectionRecord {
    std::int32_t destination{};
    ConnectionOperation operation{};
};

// Relates SubmitMove's requested count to the observed input frontier and remaining accumulator.
struct MoveSubmissionRecord {
    // Native code can clamp this request; it does not count encoded or simulated moves.
    std::int32_t requested{};
    std::int32_t turn_before{};
    std::int32_t turn_after{};
    float accumulator{};
    std::uint64_t duration{};
};

// Counts actual ReadUpdate calls in one ReceiveClient pass; turn bounds follow parser order.
struct ClientReceivePassRecord {
    std::int32_t turn_before{};
    std::int32_t turn_after{};

    std::uint32_t groups{};
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    // Calls bypassing the client unsigned-turn guard, including early returns and the player-host path.
    std::uint32_t unclassified{};
    std::uint32_t first_turn{};
    std::uint32_t last_turn{};
    std::uint32_t turns_known{};
    std::uint32_t reserved{};

    // Sum and maximum_duration use RDTSCP ticks; QPC bounds/gap below cover the included calls.
    std::uint64_t duration{};
    // Entry ordinals can complete out of order for nested passes.
    std::uint32_t first_pass{};
    std::uint32_t last_pass{};
    std::uint32_t calls{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
    std::uint64_t maximum_gap{};
    std::uint64_t maximum_duration{};
};

// Client turn-guard outcome observed at its unsigned JA branch; acceptance does not prove complete decode validity.
enum class UpdateOutcome : std::uint32_t {
    GuardNotReached,
    NewerTurn,
    OldOrEqualTurn,
};

// Complete group passed to ReadUpdate, preserving identity before native decoding consumes its packets.
struct UpdateReadRecord {
    std::int32_t turn_before{};
    std::int32_t turn_after{};
    std::uint32_t group{};
    std::uint32_t advertised_turn{};
    UpdateOutcome outcome{};
    std::uint32_t first_packet{};
    std::uint32_t packets{};
    GroupIdentity identity{};
    std::uint64_t duration{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
};

// Actual group guard result in Standard; Full keeps packet-pool and parser timing details in UpdateRead.
struct UpdateAcceptanceRecord {
    std::int32_t turn_before{};
    std::int32_t turn_after{};
    std::uint32_t group{};
    std::uint32_t advertised_turn{};
    UpdateOutcome outcome{};
    GroupIdentity identity{};
    std::uint32_t receive_pass{};
};

// Prediction progress in Standard; Combat adds actual per-actor update work.
struct PredictionProgressRecord {
    std::int32_t update_turn{};
    std::int32_t predict_before{};
    std::int32_t predict_after{};
    std::int32_t local_turn{};
    std::uint32_t caught_up{};
    std::int32_t max_update_steps{};
    std::uint64_t duration{};
};

// Actual readiness result and masks in Standard; Combat adds exact branch operands and synthesis provenance.
struct ReadinessResultRecord {
    std::uint32_t candidate_turn{};
    std::uint32_t client_turn{};
    std::uint32_t lead_turns{};
    std::uint32_t grace_turns{};
    std::uint64_t required_mask{};
    std::uint64_t received_mask{};
    std::uint32_t result{};
    std::uint32_t waitlate{};
    std::uint32_t network_enabled{};
    float native_time{};
};

// Prediction frontiers and native caught-up status, sampled around one Predict call.
struct PredictionRecord {
    std::int32_t update_turn{};
    std::int32_t predict_before{};
    std::int32_t predict_after{};
    std::int32_t local_turn{};
    // Normal completion compares final prediction and client input frontiers; native bypasses also return true.
    // Neither value counts replay work.
    std::uint32_t caught_up{};
    std::uint32_t reserved{};

    std::uint64_t duration{};
    // Sampled at Predict completion; update samples below run after Predict's modifiers.
    std::int32_t max_update_steps{};
    std::int32_t first_update_steps{};
    std::int32_t last_update_steps{};
    std::uint32_t soldier_updates{};
    std::uint32_t remote_updates{};
    float remote_minimum_delta{};
    float remote_maximum_delta{};
    std::int32_t remote_first_turn{-1};
    std::int32_t remote_last_turn{-1};
    std::int32_t remote_minimum_turn{-1};
    std::int32_t remote_maximum_turn{-1};
    // Consecutive runs of observed prediction turns, rather than a frontier difference.
    std::uint32_t remote_turn_runs{};
    std::uint64_t remote_players{};
};

// Completed receipt or native Soldier update; raw Trigger words retain state and duration bits.
struct SoldierPostureRecord {
    std::uint32_t soldier{};
    std::uint32_t stream{};
    // 1 completed ReadObject, 2 native Update; before fields equal after fields for a receipt.
    std::uint32_t stage{};
    std::int32_t player{-1};
    std::int32_t local_slot{-1};
    std::uint32_t crouch_before{};
    std::uint32_t crouch_after{};
    std::int32_t state_before{};
    std::int32_t state_after{};
    // Receipt uses the enclosing advertised turn; Update uses the current authoritative update frontier.
    std::int32_t authority_turn{-1};
    std::int32_t prediction_turn{-1};
    // Bytes from low to high: network enabled, client world, local pass, late pass.
    std::uint32_t world_pass{};
    float delta{};
    std::uint32_t result{};
};

// Clock adjustment turn, stable-update count, lag in turns, and dimensionless network time scale.
struct ClockAdjustmentRecord {
    std::int32_t adjust_before{};
    std::int32_t adjust_after{};

    std::int32_t stable_before{};
    std::int32_t stable_after{};

    std::int32_t lag_before{};
    std::int32_t lag_after{};

    float scale_before{};
    float scale_after{};
    std::uint32_t host_turn{};
    std::uint32_t last_host_turn{};
    std::uint32_t calls{};
    // Exact repeated decisions retain their first/last observation and host turn.
    std::uint64_t first_qpc{};
    std::uint64_t last_qpc{};
};

// Native estimator result and serialized eight-bit lag for this destination and observed host turn.
// Repeated equal policy/results are summarized every 20 writes.
struct HostLagRecord {
    std::int32_t destination{-1};
    std::uint32_t host_turn{};
    std::int32_t lagversion{};
    std::uint32_t drop_lag_jumps{};
    std::int32_t estimator_result{};
    std::int32_t transmitted_lag{};
    std::uint32_t wire_value{};
    std::uint32_t writes{};
    std::uint64_t first_timestamp{};
    std::uint64_t last_timestamp{};
};

enum class ClockPhase : std::uint32_t {
    ScaleBefore = 1,
    ScaleAfter,
    ResetBefore,
    ResetAfter,
    PresentationNext,
};

// Samples read on the native owner thread, selected by scale transitions or actual accumulator resets.
// Native clocks, deltas and accumulator use seconds; input/authority/prediction frontiers use turns.
struct ClockPhaseRecord {
    ClockPhase phase{};
    std::uint32_t episode{};
    std::int32_t received_host_turn{};
    std::int32_t acknowledged_turn{};
    std::int32_t input_turn{};
    std::int32_t update_turn{};
    std::int32_t predicted_turn{};
    std::int32_t local_predicted_turn{};
    float accumulator{};
    float presentation_clock{};
    float presentation_delta{};
    float interpolation{};
    float frame_delta{};
    float scale{};
    float additive_correction{};
    float native_time{};
    float decision_deadline{};
};

// GameLoop entry deltas and gap between entries on this thread, before the displaced entry instructions run.
struct ClientFrameRecord {
    float xmm0_delta{};
    float xmm1_delta{};
    std::uint64_t gap{};
};

// Copies a move at an encoder or decoder boundary and identifies aligned storage in a native history ring.
struct MoveCodecRecord {
    std::uint32_t packet{};
    std::uint32_t previous{};
    std::uint32_t move{};

    MoveCodecOperation operation{};

    std::int32_t player{-1};
    // Ring position and current global frontier; neither identifies the move's full encoded turn.
    std::int32_t turn_slot{-1};
    std::int32_t turn_reference{-1};
    std::uint32_t full_turn{};
    std::uint32_t full_turn_known{};

    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint8_t joystick{};
    std::array<std::uint8_t, 3> reserved{};

    std::uint64_t duration{};
};

// Expanded reliable sequence frontier around decoding; old batches can be parsed without replaying their actions.
struct ReliableEventBatchRecord {
    std::uint32_t packet{};
    std::uint32_t frontier_before{};
    std::uint32_t frontier_after{};
    std::uint32_t reserved{};

    std::uint64_t duration{};
};

// Complete-group retrieval result, before any game update or event parser consumes the group.
struct NativeReceiveGroupRecord {
    std::uint32_t group{};
    std::int32_t packet_type{};
    std::uint32_t complete{};
    // Failed retrievals since the preceding record for this packet type; none identifies a missing group or loss.
    std::uint32_t empty_polls{};

    std::uint64_t duration{};
    std::uint32_t first_packet{};
    std::uint32_t packets{};
    GroupIdentity identity{};
};

// One rotating soldier sample after decoding or smoothing-history publication; the stages select independently.
struct ClientPoseRecord {
    std::uint32_t object{};
    std::uint32_t related{};
    ClientPoseStage stage{};
    std::int32_t player{-1};

    std::array<float, 16> matrix{};

    std::uint64_t duration{};
};

// One accepted authority iteration, opened at SaveLateState and emitted after FinishTurn.
struct AuthorityTurnRecord {
    std::int32_t candidate_turn{};
    std::int32_t client_turn{};
    std::int32_t host_turn_after{};
    std::int32_t requested_turns{};
    std::uint32_t catch_up_ordinal{};
    std::uint32_t reserved{};

    std::uint64_t required_mask{};
    std::uint64_t received_mask{};
    std::uint64_t missing_mask{};
    std::uint64_t new_input_mask{};

    std::uint64_t receive_duration{};
    std::uint64_t rollback_duration{};
    std::uint64_t save_duration{};
    // Interval from SaveLateState completion to FinishTurn entry, including nested work and recorder overhead.
    std::uint64_t simulation_duration{};
    std::uint64_t finish_duration{};
};

// Frame aggregate emitted after SendHost; early returns without SendHost leave no completed frame record.
struct ServerFrameRecord {
    std::int32_t host_turn_before{};
    std::int32_t host_turn_after{};
    std::int32_t client_turn{};
    std::uint16_t requested_turns{};
    // Counts SaveLateState calls reached after readiness succeeds, even when snapshot saving is disabled.
    std::uint16_t turns_advanced{};
    std::uint16_t receive_calls{};
    std::uint16_t write_updates{};

    std::uint64_t destination_mask{};

    std::uint64_t frame_gap{};
    std::uint64_t send_duration{};
    std::uint64_t write_update_duration{};

    std::uint64_t required_mask{};
    std::uint64_t missing_mask{};

    float accumulator{};
    float fixed_delta{};
    // SendHost completion sees the last simulation step, or the outer frame delta when no step ran.
    float simulation_delta{};
    float time_scale{};
    std::uint32_t policy{};
};

// Scheduling totals derived from the actual storage, CREATE and gate observations during the active frame.
struct SchedulingRecord {
    std::uint32_t ack_slots{};
    // Invalid native send-storage indices, distinct from rejected ACK packets.
    std::uint32_t invalid_slots{};
    std::uint32_t create_fences{};
    // Counts gate observations; the same destination can contribute repeatedly.
    std::uint32_t blocked_destinations{};
    std::uint32_t timed_out_fences{};

    std::uint64_t destination_mask{};
};

// ReadMove, ReadMiniMove, FillTurns, and reconstructed ACK observations nested in ReceivePrimary.
struct ServerInputRecord {
    std::uint32_t moves{};
    std::uint32_t mini_moves{};
    // Counts FillTurns calls with a positive delta; readiness synthesis bypasses FillTurns and is omitted.
    std::uint32_t filled_players{};
    std::uint32_t filled_turns{};

    // Reconstructed before native acceptance; zero, old, and duplicate ACKs can contribute.
    std::uint32_t acknowledgements{};
    std::int32_t oldest_acknowledged_turn{-1};
    std::int32_t newest_acknowledged_turn{-1};
    std::uint64_t acknowledged_players{};

    std::uint64_t duration{};
};

// Ordinary event cursor changes during one destination update; cursor movement is not proof of transmission.
struct DestinationUpdateRecord {
    std::int32_t destination{-1};

    // Ordinary event consumption includes culled or discarded events as well as encoded events.
    std::uint32_t event_cursor_before{};
    std::uint32_t event_cursor_after{};
    std::uint32_t event_head{};
    std::uint32_t reserved{};
};

// WeaponCannon::Fire's native result and aim basis; a true result can occur during client simulation.
struct WeaponFireRecord {
    std::uint32_t weapon{};
    std::uint32_t owner{};
    std::uint32_t aimer{};

    std::uint32_t accepted{};

    // Captured after GetFireMatrix and before spread transforms.
    std::array<float, 16> matrix{};

    std::uint64_t duration{};
    std::uint32_t pattern_before{};
    std::uint32_t pattern_after{};
    std::int32_t prediction_turn{-1};
    // Bytes from low to high: network enabled, client world, local pass, late pass, sampled at Fire entry.
    std::uint32_t world_pass{};
};

// OrdnanceBulletClass::Build result and initial state; accepted means the returned pointer was non-null.
struct ProjectileBuildRecord {
    std::uint32_t projectile{};
    std::uint32_t projectile_class{};
    std::uint32_t descriptor{};
    std::uint32_t accepted{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    float lifetime{};
    std::uint32_t reserved{};

    std::uint64_t duration{};
};

// OrdnanceBullet::Update state; End means the native result requested removal.
struct ProjectileSimulationRecord {
    std::uint32_t projectile{};
    float delta{};

    // Counts all nested queries; detail fields retain the last completed outer query.
    std::uint32_t rays{};
    std::uint32_t ray_target{};
    std::uint32_t ray_filter{};
    std::uint32_t ray_filter_secondary{};
    std::uint32_t ray_filter_word{};
    std::uint32_t ray_flags{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};

    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
    float ray_length{};
    float ray_result{};

    std::uint64_t duration{};
};

// One base ApplyDamage call, with health and shield changes and the active bullet context when present.
struct DamageRecord {
    std::uint32_t damageable{};
    // Character pointer from the damage descriptor.
    std::uint32_t source{};
    std::uint32_t descriptor{};
    std::uint32_t projectile{};
    std::uint32_t flags{};

    float health_before{};
    float health_after{};
    float shield_before{};
    float shield_after{};

    DamageKind kind{};
    // Native mIsAlive after the call; a kill can return false and unchanged health can return true.
    std::uint8_t result{};
    std::uint16_t reserved{};

    std::uint64_t duration{};
};

// PlayerController inputs and native trigger switch state around one Update call.
struct ControllerStateRecord {
    std::uint32_t controller{};
    std::uint32_t controllable{};
    std::int32_t player{};

    float delta{};

    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint8_t joystick{};
    std::array<std::uint8_t, 3> reserved{};

    std::uint32_t primary_switch_before{};
    std::uint32_t primary_switch_after{};
    std::uint32_t secondary_switch_before{};
    std::uint32_t secondary_switch_after{};

    std::uint64_t duration{};
};

// Ordnance event queue insertion; head movement alone establishes no delivery outcome.
struct OrdnanceEventRecord {
    std::uint32_t projectile_class{};
    std::uint32_t descriptor{};
    std::uint32_t head_before{};
    std::uint32_t head_after{};
    std::uint32_t event{};
    std::uint32_t type{};
    std::uint32_t source_turn{};
    std::uint32_t generation{};

    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
};

// Soldier entity state copied from a remembered player-controller Controllable subobject.
struct SoldierPose {
    std::int32_t player{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
};

// Up to three remembered soldier poses sampled after FinishTurn; native objects remain borrowed.
struct ServerPoseBatch {
    std::int32_t turn{};
    std::uint32_t count{};
    std::array<SoldierPose, 3> soldiers{};
};

// Native waitlate/network flags, optional grace in turns, and fixed step in seconds when policy values change.
struct NetworkPolicyRecord {
    std::uint8_t waitlate{};
    std::uint8_t network_enabled{};
    std::uint16_t reserved{};
    std::uint32_t grace_turns{};
    float fixed_delta{};
};

// Copies the Direct producer's association transition, protocol counters, and elapsed milliseconds.
struct DirectAssociationRecord {
    std::int32_t slot{};
    std::uint64_t peer_id{};
    std::uint32_t connection_id{};

    DirectAssociationPhase phase{};
    DirectRoute route{};
    DirectRouteReason route_reason{};
    AssociationEndReason end_reason{};

    // Negotiation attempts at route selection; Started and Ended report zero.
    std::uint32_t attempts{};
    std::uint32_t tx_datagrams{};
    std::uint32_t rx_datagrams{};
    std::uint32_t send_failures{};
    std::uint32_t endpoint_rejects{};
    std::uint32_t authentication_rejects{};
    std::uint32_t replay_rejects{};
    std::uint32_t invalid_rejects{};

    std::uint64_t elapsed_ms{};
    std::uint64_t direct_ms{};
};

// Authenticated, replay-admitted Data and its attempted native injection; native acceptance remains unknown.
struct DirectReceiveRecord {
    std::int32_t slot{};
    std::uint64_t peer_id{};
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    // QPC recvfrom completion and native intake entry on this machine; no kernel-arrival or one-way latency claim.
    std::uint64_t received_qpc{};
    std::uint64_t injected_qpc{};
    std::uint32_t native_packet{};
    std::uint32_t rebuild_result{};
    GroupIdentity identity{};
};

// One nonempty pump or up to 128 empty pumps; IDs bound the included entries and need not be contiguous.
struct DirectPumpRecord {
    std::uint64_t socket_id{};
    std::uint32_t first_pump_id{};
    std::uint32_t last_pump_id{};
    // All timestamps, gaps and durations in this record use QPC ticks.
    std::uint64_t started_qpc{};
    std::uint64_t finished_qpc{};
    std::uint64_t maximum_service_gap{};
    // Entry timestamp of the pump that ended the longest gap.
    std::uint64_t maximum_service_gap_qpc{};
    std::uint64_t first_empty_qpc{};
    std::uint64_t last_empty_qpc{};
    std::uint64_t maximum_receive_duration{};
    std::uint32_t calls{};
    std::uint32_t empty_calls{};
    std::uint32_t attempts{};
    std::uint32_t datagrams{};
    std::uint32_t oversized{};
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    std::int32_t error{};
    std::uint32_t budget_reached{};
    // 0 empty socket, 1 attempt budget, 2 socket error; empty aggregates always have reason 0.
    std::uint8_t terminal_reason{};
};

// Arrival and terminal disposition share socket/pump/attempt; header fields are untrusted wire values.
struct DirectDatagramRecord {
    std::uint64_t socket_id{};
    std::uint32_t pump_id{};
    std::uint32_t attempt{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
    std::uint64_t peer_id{};
    std::int32_t slot{-1};
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    std::uint32_t source_ipv4_network_order{};
    std::uint16_t source_port{};
    std::uint16_t bound_port{};
    std::uint32_t bytes{};
    // 0 arrived; 1 malformed; 2 association; 3 endpoint; 4 state; 5 authentication; 6 replay; 7 control; 8 data; 9 rebuild.
    std::uint8_t disposition{};
    std::uint8_t kind{};
    std::uint8_t header_available{};
    // ParseStatus for malformed, ReplayResult for replay, DirectReceive rebuild_result for rebuild; otherwise zero.
    std::uint8_t detail{};
};

// Winsock startup set errors and effective buffer readback cached once at the first Full service.
struct DirectSocketRecord {
    std::uint64_t socket_id{};
    std::int32_t requested_send_bytes{};
    std::int32_t requested_receive_bytes{};
    std::int32_t actual_send_bytes{};
    std::int32_t actual_receive_bytes{};
    std::int32_t send_set_error{};
    std::int32_t receive_set_error{};
    std::int32_t send_read_error{};
    std::int32_t receive_read_error{};
    std::uint16_t bound_port{};
};

// Changed gate or bounded repeat aggregate; network time and RTT estimates use native seconds.
struct DestinationGateRecord {
    std::int32_t destination{-1};
    std::uint64_t peer_id{};
    std::uint32_t reason{};
    // FC ring count and capacity; capacity is zero when the native window or CREATE fence determines the gate.
    std::uint32_t outstanding{};
    std::uint32_t capacity{};
    std::uint32_t fence_map{};
    std::int32_t fence_turn{};
    // Latest repeated check's native time; RTT estimates below retain the initial decision's values.
    float network_time{};
    float fence_sent_time{};
    float average_rtt{};
    float deviation_rtt{};
    std::uint32_t native_window_open{};
    std::uint32_t timed_out{};
    std::uint32_t checks{};
    std::uint64_t first_timestamp{};
    std::uint64_t last_timestamp{};
};

// Native input outcomes distinguish history admission from controls used by authority simulation.
enum class InputDisposition : std::uint32_t {
    CurrentOrFuture = 1,
    Duplicate,
    UnchangedLate,
    ChangedLate,
    ObjectUnavailable,
    TooOld,
    Terminator,
};

enum class InputSource : std::uint32_t {
    Received = 1,
    Retained,
    Default,
    Unknown
};

// The header age gate uses unsigned native arithmetic; frontier publication precedes this decision.
struct InputHeaderRecord {
    GroupIdentity identity{};
    std::int32_t player{-1};
    std::uint32_t advertised_turn{};
    std::uint32_t host_turn{};
    std::uint32_t frontier{};
    std::uint32_t reason{}; // 0 passed, 1 too old, 2 too far ahead.
};

// One completed row follows the actual native comparison, mask writes and optional history replacement.
struct InputDispositionRecord {
    GroupIdentity identity{};
    std::int32_t player{-1};
    std::uint32_t input_turn{};
    std::uint32_t host_turn{};
    InputDisposition disposition{};
    std::uint64_t received_before{};
    std::uint64_t received_after{};
    std::uint64_t late_before{};
    std::uint64_t late_after{};
    std::uint32_t old_hash{};
    std::uint32_t history_hash{};
    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint32_t joystick{};
};

// Active controls copied for every native player, independent of actor class or controller execution.
struct PreparedInputRecord {
    std::uint32_t candidate_turn{};
    std::int32_t player{-1};
    std::uint32_t active_input{};
    std::uint32_t source_turn{};
    InputSource source{InputSource::Unknown};
    std::uint32_t source_turn_known{};
    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint32_t joystick{};
    std::uint32_t history_copied{};
};

// The actual IsTurnReady result, peer gate and fallback path retain the native operands in their own turn domains.
struct ReadinessRecord {
    std::uint32_t candidate_turn{};
    std::uint32_t client_turn{};
    std::uint32_t lead_turns{};
    std::uint32_t grace_turns{};
    std::uint64_t required_mask{};
    std::uint64_t received_mask{};
    std::uint32_t result{};
    std::uint32_t peer_ready{};
    std::uint32_t forced{};
    std::uint32_t waitlate{};
    float native_time{};
    std::uint32_t network_enabled{};
    std::uint32_t host_turn{};
    std::uint64_t duration{};
    std::uint32_t force_block{UINT32_MAX};
};

// Replay body entry is distinct from ApplyLateMoves entry; steps count native simulation loop iterations.
struct LateReplayRecord {
    std::uint32_t host_turn{};
    std::uint32_t network_enabled{};
    std::uint32_t waitlate{};
    std::uint32_t body_entered{};
    std::uint32_t first_turn{};
    std::uint32_t last_turn{};
    std::uint32_t steps{};
    std::uint64_t duration{};
    std::uint64_t required_mask{};
    std::array<std::uint64_t, 3> late_masks{};
    std::int32_t cutoff_player{-1};
    std::uint32_t cutoff_turn{};
};

// Actual sendto boundary, with Direct connection/data and native fragment identity in one local QPC domain.
struct DirectSendRecord {
    std::int32_t slot{};
    std::uint64_t peer_id{};
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    GroupIdentity identity{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
    std::uint32_t requested_bytes{};
    std::int32_t returned_bytes{};
    std::int32_t error{};
    std::uint32_t phase{};
    std::uint32_t rejection{};
    std::uint64_t socket_id{};
    // Entry to Direct serialization precedes the actual sendto boundary above.
    std::uint64_t encoding_qpc{};
    std::uint32_t destination_ipv4_network_order{};
    std::uint16_t destination_port{};
    std::uint16_t bound_port{};
};

// A native input frontier delimits receipt windows, including windows with no admitted or parsed authority updates.
struct ReceiveWindowRecord {
    std::uint32_t window{};
    std::int32_t input_turn{};
    std::int32_t host_turn{};
    std::uint32_t received_fragments{};
    std::uint32_t retrieved_groups{};
    std::uint32_t parsed_groups{};
    std::uint32_t accepted_groups{};
    std::uint32_t rejected_groups{};
    std::uint32_t receive_passes{};
    std::uint32_t empty_windows_before{};
    std::uint64_t opened_qpc{};
    std::uint64_t closed_qpc{};
    std::uint32_t recovery_calls{};
    std::uint64_t first_drain_qpc{};
    std::uint64_t last_drain_qpc{};
    std::uint64_t maximum_drain_gap{};
};

// Append establishes native queue admission; GetGroup establishes complete-group eligibility.
struct QueuePacketRecord {
    std::uint32_t packet{};
    GroupIdentity identity{};
    std::uint32_t queue{};
    std::uint32_t count_before{};
    std::uint32_t count_after{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
};

// Nonempty recovery drains carry group ranges; receive-window aggregates count empty opportunities.
struct RecoveryDrainRecord {
    std::uint32_t ordinal{};
    std::uint32_t receive_pass{};
    std::uint32_t groups{};
    std::uint32_t oldest_turn{};
    std::uint32_t newest_turn{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
};

// Final candidate refusal is observed at the branch that skips it; values are native turns/seconds/bits.
struct SchedulingDecisionRecord {
    std::int32_t destination{-1};
    std::uint32_t host_turn{};
    std::uint32_t reason{};
    std::uint32_t next_turn{};
    std::int32_t pipe{-1};
    std::uint32_t ticket{};
    std::uint32_t pipe_full{};
    std::uint32_t should_send{};
    float pipe_bits{};
    std::uint32_t bandwidth{};
    std::uint32_t allowance{};
    float native_time{};
    std::uint32_t checks{};
    std::uint64_t first_timestamp{};
    std::uint64_t last_timestamp{};
    std::uint32_t last_host_turn{};
};

// Every ACK retains exact native/extended admission and the estimator before/after values.
struct UpdateAckRecord {
    std::int32_t destination{-1};
    std::uint32_t turn{};
    std::uint32_t native_slot{};
    std::uint32_t accepted{};
    float sample{};
    float mean_before{};
    float mean_after{};
    float deviation_before{};
    float deviation_after{};
    std::uint32_t samples{};
    std::uint32_t outstanding{};
    std::array<std::uint32_t, 2> sent_turns{};
    std::array<float, 2> sent_times{};
};

// Event storage is native ring storage; sequence and creation turn disambiguate reuse.
struct ReplicationEventRecord {
    std::uint32_t event{};
    std::uint32_t sequence{};
    std::uint32_t type{};
    std::uint32_t source_turn{};
    std::int32_t destination{-1};
    std::uint32_t outcome{};
    std::uint32_t cursor{};
    std::uint32_t head{};
    std::uint32_t bits_before{};
    std::uint32_t bits_after{};
    float relevance{};
    std::uint32_t packet{};
    std::uint32_t generation{};
    std::uint32_t ordinal{};
    std::uint64_t recipients{};
    std::uint32_t related_object{};
    std::uint32_t related_class{};
    std::uint32_t gate_visited{};
    std::uint32_t gate_results{};
    std::uint32_t target_object{};
    std::uint32_t target_lifetime{};
    std::uint32_t expected_lifetime{};
    std::uint32_t built_object{};
};

// Full object selection and serializer outcomes carry native lifetime IDs and packet bit boundaries.
struct ReplicationObjectRecord {
    std::uint32_t object{};
    std::uint32_t native_id{};
    std::uint32_t object_type{};
    std::uint32_t source_turn{};
    std::int32_t destination{-1};
    std::uint32_t outcome{};
    std::uint32_t packet{};
    std::uint32_t bits_before{};
    std::uint32_t bits_after{};
    std::uint32_t budget{};
    std::uint32_t last_turn{};
    std::uint32_t priority{};
    std::uint32_t wire_id{};
    std::uint32_t map{};
    std::uint32_t age_before{};
    std::uint32_t age_after{};
    std::uint32_t native_class{};
    float relevance{};
};

// Velocity codec operands and actual scalar/direction codes, scoped to the enclosing soldier packet.
struct VelocityBoundsRecord {
    std::uint32_t object{};
    std::uint32_t packet{};
    std::uint32_t native_class{};
    std::uint32_t operation{};
    std::array<float, 3> class_fields{};
    float factor{};
    float maximum{};
    std::uint32_t scalar_raw{};
    std::uint32_t direction_setting{};
};

// CREATE's native response disposition precedes the resulting map replacement.
struct CreateResponseRecord {
    std::uint32_t player{};
    std::int32_t destination{-1};
    std::uint32_t packet{};
    std::uint32_t response_turn{};
    std::uint32_t success{};
    std::uint32_t decision{};
    std::uint32_t active_before{};
    std::uint32_t pending_before{};
    std::uint32_t active_turn_before{};
    std::uint32_t pending_turn_before{};
    std::uint32_t active_after{};
    std::uint32_t pending_after{};
    std::uint32_t active_turn_after{};
    std::uint32_t pending_turn_after{};
};

struct VelocityCodecRecord {
    std::uint32_t object{};
    std::uint32_t packet{};
    std::uint32_t input_turn{};
    std::uint32_t bit_offset{};
    std::uint32_t operation{};
    std::uint32_t raw{};
    float maximum{};
    float magnitude{};
    std::uint32_t magnitude_bits{};
    std::uint32_t magnitude_code{};
    std::uint32_t saturated{};
    std::array<std::uint32_t, 3> direction_codes{};
    std::array<float, 3> velocity{};
    std::uint32_t bits_after{};
    std::uint32_t bit_offset_known{};
    std::uint32_t present{};
    std::array<std::uint32_t, 3> direction_bits{};
};

// One main-loop frame with coarse game, pacing, background wait and network flush times in QPC ticks.
struct FrameTimingRecord {
    std::uint64_t begin_qpc{};
    std::uint64_t end_qpc{};
    std::uint64_t pacing_ticks{};
    std::uint64_t game_ticks{};
    std::uint64_t background_sleep_ticks{};
    // Actual QPC interval computed by native code; zero with target_known == 0 means the division was bypassed.
    std::uint64_t pacing_target_ticks{};
    std::uint64_t network_flush_ticks{};
    std::uint32_t background_sleep_ms{};
    std::uint32_t target_known{};
};

struct SendStorageRecord {
    std::int32_t destination{-1};
    std::uint32_t turn{};
    std::int32_t native_slot{-1};
    std::uint32_t phase{};
    std::uint64_t peer_id{};
    float network_time{};
    std::uint32_t before_count{};
    std::uint32_t after_count{};
    std::uint32_t extended_recorded{};
    std::uint32_t native_write_skipped{};
};

struct CreateFenceRecord {
    std::int32_t destination{-1};
    std::uint32_t phase{};
    std::uint32_t map_before{};
    std::uint32_t map_after{};
    std::int32_t turn_before{};
    std::int32_t turn_after{};
    float sent_time{};
    float network_time{};
    float average_rtt{};
    float deviation_rtt{};
};

// Each channel write submits one complete diagnostic record.
template <class Payload> struct Record {
    RecordHeader header;
    Payload payload;
};

#pragma pack(pop)

template <class Payload>
inline constexpr bool kValidRecord = std::is_trivially_copyable_v<Record<Payload>> && std::is_standard_layout_v<Record<Payload>> &&
                                     sizeof(Record<Payload>) <= kMaximumRecordSize;

static_assert(kValidRecord<PredictionProgressRecord>);
static_assert(kValidRecord<UpdateAcceptanceRecord>);
static_assert(kValidRecord<ReadinessResultRecord>);
static_assert(kValidRecord<ClientReceivePassRecord>);
static_assert(kValidRecord<ClockAdjustmentRecord>);
static_assert(kValidRecord<NativeSendGroupRecord>);
static_assert(kValidRecord<TransportReceiveRecord>);
static_assert(kValidRecord<NativePacketRecord>);
static_assert(kValidRecord<MoveSubmissionRecord>);
static_assert(kValidRecord<ClientReceivePassRecord>);
static_assert(kValidRecord<ClientPoseRecord>);
static_assert(kValidRecord<AuthorityTurnRecord>);
static_assert(kValidRecord<ServerFrameRecord>);
static_assert(kValidRecord<ProjectileSimulationRecord>);
static_assert(kValidRecord<DirectAssociationRecord>);
static_assert(kValidRecord<DirectReceiveRecord>);
static_assert(kValidRecord<DirectPumpRecord>);
static_assert(kValidRecord<DirectDatagramRecord>);
static_assert(kValidRecord<DirectSocketRecord>);
static_assert(kValidRecord<DestinationGateRecord>);
static_assert(kValidRecord<UpdateReadRecord>);
static_assert(kValidRecord<HostLagRecord>);
static_assert(kValidRecord<ClockPhaseRecord>);
static_assert(kValidRecord<InputDispositionRecord>);
static_assert(kValidRecord<InputHeaderRecord>);
static_assert(kValidRecord<PreparedInputRecord>);
static_assert(kValidRecord<ReadinessRecord>);
static_assert(kValidRecord<LateReplayRecord>);
static_assert(kValidRecord<DirectSendRecord>);
static_assert(kValidRecord<ReceiveWindowRecord>);
static_assert(kValidRecord<QueuePacketRecord>);
static_assert(kValidRecord<RecoveryDrainRecord>);
static_assert(kValidRecord<SchedulingDecisionRecord>);
static_assert(kValidRecord<UpdateAckRecord>);
static_assert(kValidRecord<ReplicationEventRecord>);
static_assert(kValidRecord<ReplicationObjectRecord>);
static_assert(kValidRecord<VelocityCodecRecord>);
static_assert(kValidRecord<VelocityBoundsRecord>);
static_assert(kValidRecord<SendStorageRecord>);
static_assert(kValidRecord<CreateFenceRecord>);
static_assert(kValidRecord<CreateResponseRecord>);
static_assert(kValidRecord<FrameTimingRecord>);
static_assert(kValidRecord<PredictionRecord>);
static_assert(kValidRecord<WeaponFireRecord>);
static_assert(kValidRecord<SoldierPostureRecord>);

} // namespace fc_plugins::network_diagnostics::trace
