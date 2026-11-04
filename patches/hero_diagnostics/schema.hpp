#pragma once

#include "api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fc_plugins::hero_diagnostics::trace {

inline constexpr std::uint32_t kSchemaVersion = 17;

// The runtime queues a 24-byte QPC/sequence/thread/length envelope before each payload.
// The buffer holds 4096 maximum-sized records, including that framing.
inline constexpr std::uint32_t kMaximumRecordSize = 192;
inline constexpr std::size_t kBufferBytes = 4096U * (kMaximumRecordSize + 24U);

inline constexpr std::uint8_t kNoLocalPlayer = 0xFF;

// Record kinds separate identity, simulation, movement, presentation, and network evidence for trace decoders.
enum class RecordKind : std::uint16_t {
    Subject = 1,
    MeleeTick = 2,
    InputDecision = 3,
    NetworkState = 4,
    Transition = 5,
    MovementFrame = 6,
    NetworkMovement = 7,
    PresentationFrame = 8,
    MeleeContact = 9,
    AudioCue = 11,
    Locomotion = 12,
    LocalState = 13,
    DisplayMatrix = 14,
    NativeQueue = 15,
    Condition = 16,
    History = 17,
    Physics = 18,
    Dispatch = 19,
    Submission = 20,
    AnimationClock = 21,
    Asset = 22,
    Lifecycle = 23,
    Predicate = 24,
    AnimationAdvance = 25,
    AnimationSelection = 26,
    AssetName = 27,
    GenericTimer = 28,
    Simulation = 29,
    Integration = 30,
    CombatDecision = 31,
    SoldierDecision = 32,
    AnimationDecision = 33,
    EnergyCodec = 34,
    MotionOperands = 35,
    Orientation = 36,
    Cause = 37,
    Selection = 42,
    Boundary = 43,
    DisplayPosition = 44,
    SimulationSummary = 45,
    SoldierOutcome = 46,
    InputUse = 47,
    CauseSummary = 48,
    ActionFrame = 49,
    PreparedUse = 50,
    SimulationOutcome = 51,
    ConditionOutcome = 52,
    LocalStateBasis = 53,
};

// Flags label observation boundaries and reported decisions; pass labels alone do not identify host authority.
enum RecordFlags : std::uint16_t {
    None = 0,
    Begin = 1U << 0U,
    End = 1U << 1U,
    // Update/Apply derive this from the owner's decision; other producers set it from their observation boundary.
    NativeCalled = 1U << 2U,
    // Observed hook bool, including a true result from an omitted Update.
    NativeResult = 1U << 3U,
    StateChanged = 1U << 4U,
    Prediction = 1U << 5U,
    Authority = 1U << 6U,
    Local = 1U << 7U,
    Remote = 1U << 8U,
    Suppressed = 1U << 9U,
    Anomaly = 1U << 10U,
};

enum class MovementKind : std::uint8_t {
    Controls = 1,
    Velocity = 2,
    SoldierRead = 3
};

enum class PresentationKind : std::uint8_t {
    Combo = 1,
    Action = 2,
    Pose = 3
};

enum class TransitionKind : std::uint8_t {
    Enter = 1,
    Exit = 2,
    Prediction = 3
};

enum class NetworkMovementKind : std::uint8_t {
    Parsed = 1,
    ServerConsumed = 3
};

enum class NetworkStateOperation : std::uint8_t {
    Apply = 1,
    Read = 2,
    Write = 3
};

enum class LocomotionOperation : std::uint8_t {
    Jump = 2,
    Roll = 3
};

// Finished flags sample native animation state and can also mark loop completion.
enum PresentationStatus : std::uint8_t {
    PresentationNormal = 0,
    PresentationSuppressed = 1U << 0U,
    UpperAnimationFinished = 1U << 1U,
    LowerAnimationFinished = 1U << 2U,
};

#pragma pack(push, 1)

// Subject IDs refer to a Subject record accepted by the queue, which later output loss can still remove.
struct RecordHeader {
    std::uint32_t subject{};
    RecordKind kind{};
    std::uint16_t flags{};
};

// Samples native counters and pass flags; server records substitute host for update and omit client/replay globals.
struct TurnContext {
    // Signed counters can contain wrapped values as well as the unavailable sentinel -1.
    std::int32_t host_turn{-1};
    std::int32_t client_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t predict_turn{-1};
    std::int32_t acknowledged_turn{-1};

    // Current native destination selection, independent of transport peer identity.
    std::int32_t destination{-1};

    // Native pass flags can both be true or retain restored values between prediction passes.
    std::uint8_t local_turn{};
    std::uint8_t update_pass{};
    std::uint8_t rollback{};

    // Client: netEnabled && netOnClient; server: always one.
    std::uint8_t network_active{};
};

// Introduces a tracked melee weapon and the game objects used to correlate its later records.
struct SubjectRecord {
    std::uint32_t subject{};
    std::uint32_t generation{};

    // Borrowed x86 addresses; generation distinguishes observed lifetimes, object tuples and local-controller assignments.
    std::uint32_t weapon{};
    std::uint32_t owner{};
    std::uint32_t soldier{};
    std::uint32_t animator{};
    std::uint32_t combo{};

    // Native player slot, independent of Galaxy or other transport IDs.
    std::int16_t player{-1};

    // Zero and one identify local controllers; server subjects always retain the remote sentinel.
    std::uint8_t local_player{kNoLocalPlayer};

    // Startup process role; Client also covers offline and player-hosted games.
    std::uint8_t role{};
};

// Entry context and combo/input timers paired with exit state; the single timestamp records submission after return.
struct MeleeTickRecord {
    TurnContext turn;

    // Requested and effective update duration in seconds, as reported by the behavior owner.
    float delta{};
    float effective_delta{};

    float state_time_before{};
    float state_time_after{};
    float input_time_before{};
    float input_time_after{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};

    // Combo IDs index asset-specific rows and can recur across distinct attacks.
    std::int8_t state_before{-1};
    std::int8_t state_after{-1};
    std::int8_t previous_before{-1};
    std::int8_t previous_after{-1};

    std::uint8_t decision{};
    std::uint8_t native_called{};

    // Observed hook return; a suppressed update can return true without executing native Update.
    std::uint8_t result{};
    std::uint8_t reserved{};
    std::int32_t mode_before{-1};
    std::int32_t mode_after{-1};
    float mode_time_before{};
    float mode_time_after{};
    float notified_time_before{};
    float notified_time_after{};
    std::uint32_t scope{};
    // Bits 0/1 mark observed collision/effect bypass decisions in this update and which were applied.
    std::uint8_t bypass_known{};
    std::uint8_t bypass_applied{};
};

// Queue entry sample around FC instruction modifiers; native InputQueue::Update executes after both observers.
struct InputDecisionRecord {
    TurnContext turn;

    std::uint32_t state{};
    std::uint8_t buttons{};

    // Held-button masks before and after FC modifiers, followed by entry count and head.
    std::uint8_t down_before{};
    std::uint8_t down_after{};
    std::uint8_t queue_count{};
    std::uint8_t queue_head{};
};

// Apply/Read pair entry and exit combo state; Write emits a saved entry snapshot after serialization.
struct NetworkStateRecord {
    TurnContext turn;

    std::int32_t state_before{-1};
    std::int32_t requested_state{-1};
    std::int32_t state_after{-1};

    float state_time_before{};
    float state_time_after{};

    // Synchronous WeaponMelee::Read scope for nested setters; server writes leave it zero.
    std::uint32_t scope{};
    std::uint32_t stream{};

    // Packed trail, saber-on, previous-on, and sound masks: entry for Apply/Write, exit for Read.
    std::uint16_t action{};
    std::uint8_t state_flags{};
    std::uint8_t set_flag{};

    std::uint8_t decision{};
    std::uint8_t native_called{};
    NetworkStateOperation operation{};
    std::int32_t mode_before{-1};
    std::int32_t mode_after{-1};
    float mode_time_before{};
    float mode_time_after{};
    float notified_time_before{};
    float notified_time_after{};
    float input_time_before{};
    float input_time_after{};
};

// Enter/Exit sample resulting state; Prediction retains entry state before the displaced transition calls execute.
struct TransitionRecord {
    TurnContext turn;

    std::int32_t requested_state{-1};
    std::int32_t state_before{-1};
    std::int32_t state_after{-1};
    std::int32_t previous_state{-1};

    float state_time_before{};
    float state_time_after{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};

    TransitionKind kind{};
    std::array<std::uint8_t, 3> reserved{};

    // EnterState uses the same invocation identity exposed to synchronous Sound requests; zero is unavailable.
    std::uint32_t scope{};
    // Prediction records retain the same-site automatic commit's winning candidate and evaluated condition.
    std::uint32_t condition_scope{};
    std::uint32_t candidate_ordinal{};
    std::uint32_t condition{};
    std::uint32_t eflags{};
    float candidate_time{};
    float winning_time{};
};

// Samples helper arguments or ReadObject state alongside soldier motion at that observation boundary.
struct MovementFrameRecord {
    TurnContext turn;

    // Controls: dimensionless thrust/strafe/turn/pitch. Velocity: forward/lateral speed, dimensionless blend, vertical speed.
    // Speeds use native distance units per second; SoldierRead leaves both argument arrays zero.
    std::array<float, 4> before{};
    std::array<float, 4> after{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};

    std::int32_t state{-1};
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::uint32_t read_scope{};
    MovementKind kind{};
    std::uint8_t result{};
    std::array<std::uint8_t, 2> reserved{};
};

// Distinguishes the qualified readiness paths; CurrentRow does not establish packet arrival.
enum class InputOrigin : std::uint8_t {
    Unknown = 0,
    CurrentRow = 1,
    ReadinessRetained = 2,
    ReadinessDefault = 3,
    PredictCopied = 4,
};

enum class LocalStage : std::uint8_t {
    AuthorityRead = 1,
    PredictionReturn = 2,
    BodySubmission = 3,
    CameraOutput = 4,
};

// One local subject at an actual native boundary, paired with separate display matrices when available.
struct LocalStateRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::int32_t mode{-1};
    float mode_time{};
    std::int32_t combo{-1};
    float combo_time{};
    float upper_time{};
    float lower_time{};
    float upper_timer{};
    float lower_timer{};
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};
    std::uint32_t animation_mode{};
    // Native soldier state and elapsed seconds, raw trigger edges, and the weapon's last notified posture/time.
    std::int32_t soldier_state{-1};
    float soldier_state_time{};
    std::uint32_t sprint_trigger{};
    std::uint32_t secondary_fire_trigger{};
    std::uint32_t posture{};
    float notified_state_time{};
    // A per-observation correlation value shared with DisplayMatrix, or the native read scope.
    std::uint32_t scope{};
    LocalStage stage{};
    std::uint8_t result{};
    std::uint8_t state_flags{};
    std::uint8_t animation_flags{};
};

// Full authority/prediction boundaries merge their actual orientation basis with the private-state outcome.
struct LocalStateBasisRecord {
    LocalStateRecord state;
    std::array<std::array<float, 3>, 3> basis{};
};

// Root supplied to the native body submission, or RedCamera matrix after interpolation and its follow-up.
// These outputs precede GPU presentation and cover only the qualified paths.
struct DisplayMatrixRecord {
    TurnContext turn;
    std::array<float, 16> matrix{};
    std::uint32_t scope{};
    std::uint32_t source{};
    LocalStage stage{};
    std::array<std::uint8_t, 3> reserved{};
};

// Parsed rows or active prepared server input, accompanied by current triggers and soldier motion.
struct NetworkMovementRecord {
    TurnContext turn;

    std::array<float, 4> axes{};
    std::uint32_t input_bits{};

    // Raw Controllable Trigger words carry state and edges.
    std::uint32_t primary_fire_trigger{};
    std::uint32_t secondary_fire_trigger{};
    std::uint32_t sprint_trigger{};
    std::uint32_t jump_trigger{};

    std::array<float, 3> position{};
    std::array<float, 3> velocity{};

    std::int32_t state{-1};
    std::uint8_t joystick{};

    // ClientContext is the latest remote row before Update, including omitted updates. ServerConsumed is the active native source.
    NetworkMovementKind kind{};
    // Compares the complete move row, including remaining bytes; this is independent of packet sequence.
    std::uint8_t changed{};
    std::uint8_t reserved{};
    // Exact readiness publication turn; -1 when replay or another unqualified path supplied active input.
    std::int32_t input_turn{-1};
    InputOrigin origin{};
    std::array<std::uint8_t, 3> provenance_reserved{};
};

// Combo emits a saved entry sample; Action emits entry/exit and Pose emits its completed sample.
struct PresentationFrameRecord {
    TurnContext turn;

    // Combo elapsed time in seconds.
    float state_time{};

    // Outer game-loop delta in seconds; SetupPose can use a cached pose or a different delta.
    float frame_delta{};

    // Accumulated animation delta in seconds.
    float extra_delta{};

    // Normalized animation phases.
    float upper_time{};
    float lower_time{};

    // Native animation timers in seconds.
    float upper_timer{};
    float lower_timer{};

    // Dimensionless blend factors and weapon-animation rate.
    float upper_blend{};
    float lower_blend{};
    float animation_rate{};

    // Combo: requested current/next animation IDs. Action/Pose: borrowed ZephyrAnim pointers.
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};

    // ActionAnimation, WeaponAnimation, and generic WeaponState enum values.
    std::uint32_t action{};
    std::uint32_t weapon_animation{};
    std::uint32_t weapon_state{};

    // Combo: WeaponState/restart; Action: SoldierState/animation hash; Pose: RedPose pointer/zero.
    std::uint32_t argument0{};
    std::uint32_t argument1{};

    std::int8_t state{-1};
    std::int8_t previous_state{-1};

    PresentationKind kind{};
    std::uint8_t status{};

    // High aim-state byte: alert, move-alert, restart-weapon, first-time, reset-upper/lower, animated-move, velocity-matched.
    std::uint8_t animation_flags{};
};

// WeaponMelee::Deflect operands and hook result describe a projectile deflection attempt.
struct MeleeContactRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t parent_scope{};

    // Ordnance projectile pointer, which native Deflect also accepts as null.
    std::uint32_t target{};

    // Direction pointer encoded as an x86 word.
    std::uint32_t filter{};

    // The same position operand sampled before and after Deflect.
    std::array<float, 3> input_position{};
    std::array<float, 3> output_position{};

    std::int32_t state{-1};

    std::uint8_t result{};
    std::array<std::uint8_t, 3> reserved{};
};

// Soldier locomotion classification and energy at one observation boundary.
struct LocomotionState {
    std::int32_t state{-1};

    // Native resource amount, which can fall below zero.
    float energy{};
    // Bit 0 is exhaustion/overheat; other bits retain their native meanings.
    std::uint32_t energy_flags{};

    // Bit 5 is airborne.
    std::uint8_t movement_flags{};
    std::array<std::uint8_t, 3> reserved{};
};

// Jump/Roll pair samples around their complete energy-gated attempts.
struct LocomotionRecord {
    TurnContext turn;

    LocomotionState before;
    LocomotionState after;

    std::uint32_t soldier{};

    LocomotionOperation operation{};
    std::uint8_t result{};
    std::array<std::uint8_t, 2> reserved{};
};

// Prepared GameSound::Play request (Begin), before the behavior owner decides whether to play it; no playback result.
struct AudioCueRecord {
    TurnContext turn;
    std::uint32_t sound{};
    std::uint32_t scope{};
    std::uint8_t force{};
    std::int32_t state{-1};
};

// Occupied rows follow this metadata in logical order; unchanged pairs omit rows explicitly.
// Stages 3/4 surround FC modifiers before native Update; stages 5/6 surround native ClearInput.
struct NativeQueueRecord {
    TurnContext turn;
    float clock{};
    float threshold{};
    std::uint32_t scope{};
    std::uint8_t count{};
    std::uint8_t head{};
    std::uint8_t down{};
    std::uint8_t stage{};
    std::uint8_t unchanged{};
    // Only min(count, 8) rows are serialized, or zero rows when unchanged is set.
    std::array<std::uint32_t, 16> rows{};
};

// TestCondition's real XMM operands, packed row, native boolean and initialized output only on success.
struct ConditionRecord {
    TurnContext turn;
    std::array<std::uint32_t, 5> row{};
    float start{};
    float end{};
    float input_clock{};
    float output{};
    std::uint32_t scope{};
    std::uint32_t condition{};
    std::uint32_t parent_scope{};
    std::uint32_t candidate_ordinal{};
    std::uint32_t posture{};
    std::int32_t soldier_state{-1};
    std::uint32_t caller{};
    std::uint8_t result{};
    std::uint8_t stage{};
};

struct HistoryRecord {
    TurnContext turn;
    HistoryDecision decision;
    std::uint32_t scope{};
    std::uint32_t weapon{};
};

struct PhysicsRecord {
    TurnContext turn;
    PhysicsDecision decision;
    std::uint32_t scope{};
    std::uint32_t object{};
    std::uint8_t soldier{};
};

// One actual evaluator call: XMM1/2/3 and stack operands at entry, then its native AL or comparison flags.
struct PredicateRecord {
    TurnContext turn;
    std::array<float, 3> values{};
    std::array<std::uint32_t, 2> words{};
    std::uint32_t scope{};
    std::uint32_t condition{};
    std::uint32_t eflags{};
    std::uint8_t kind{};
    std::uint8_t result{};
    std::uint8_t stage{};
};

// Base and derived read phases retain both generic dispatch and the separate melee clocks.
struct DispatchRecord {
    TurnContext turn;
    std::int32_t mode{-1};
    float mode_time{};
    std::int32_t combo{-1};
    float combo_time{};
    float input_time{};
    float movement_blend{};
    float energy{};
    std::uint32_t energy_flags{};
    std::uint32_t posture{};
    std::int32_t soldier_state{-1};
    float soldier_state_time{};
    std::uint32_t sprint_trigger{};
    std::uint32_t secondary_fire_trigger{};
    float notified_state_time{};
    std::uint32_t scope{};
    std::uint8_t stage{};
    std::uint8_t state_flags{};
};

// Prepared model arguments pair with a separate post-call record under one render invocation.
struct SubmissionRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t model{};
    std::uint32_t pose{};
    std::uint32_t root{};
    std::uint32_t tint{};
    std::uint32_t flags{};
    std::uint32_t parameters{};
    std::uint32_t simulation_scope{};
    std::uint32_t pose_scope{};
    std::uint8_t pass{};
};

// Branch records describe the actual pose cache and effective animation-clock path.
struct AnimationClockRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::int32_t cached_frame{};
    std::int32_t current_frame{};
    std::uint32_t cached_tick{};
    std::uint32_t current_tick{};
    float delta{};
    float accumulated{};
    std::uint16_t flags{};
    std::uint8_t stage{};
};

// Qualified asset words identify loaded definitions across processes without hashing their pointers.
struct AssetRecord {
    std::uint32_t combo{};
    std::uint32_t class_pointer{};
    std::uint32_t class_hash{};
    std::uint32_t combo_hash{};
};

struct LifecycleRecord {
    TurnContext turn;
    std::uint32_t object{};
    std::uint8_t reason{};
};

// Native name-table registration supplies a resolver for loaded class and combo hashes.
struct AssetNameRecord {
    std::uint32_t hash{};
    std::uint32_t pointer{};
    std::array<char, 80> name{};
    std::uint8_t length{};
    std::uint8_t truncated{};
};

// Generic Weapon::Read writes all 32 timer bits directly from the borrowed packet stream.
struct GenericTimerRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t stream{};
    std::int32_t mode{-1};
    std::uint32_t timer_bits{};
    std::uint32_t bit_offset{UINT32_MAX};
    std::uint8_t stage{};
};

// Native animation instance advance includes its real loop/reverse/finished operands and completed phase.
struct AnimationAdvanceRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t instance{};
    std::uint32_t clip{};
    float delta{};
    float rate{};
    float phase_before{};
    float phase_after{};
    std::uint16_t frames{};
    std::array<std::uint8_t, 3> flags_before{};
    std::array<std::uint8_t, 3> flags_after{};
    std::uint8_t lane{};
};

// Selected start phase and delta are copied immediately before native SetAnim, after its restart/carry/rescale choices.
struct AnimationSelectionRecord {
    TurnContext turn;
    std::array<std::uint32_t, 3> descriptor{};
    std::uint32_t scope{};
    std::uint32_t old_clip{};
    std::uint32_t caller{};
    float old_phase{};
    float selected_phase{};
    float requested_delta{};
    float effective_delta{};
    std::uint32_t old_metadata{};
    std::uint32_t opposite_metadata{};
    std::uint32_t opposite_clip{};
    float opposite_phase{};
    std::uint16_t old_frames{};
    std::uint16_t requested_frames{};
    std::uint16_t flags_before{};
    std::uint8_t lane{};
    std::uint8_t metadata_valid{};
    std::uint16_t pending_flags{};
    std::uint32_t action{};
    float leg_angle{};
};

// Native Soldier update and integrator phases share one scope and complete object identity.
struct SimulationRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::array<float, 3> normal{};
    std::array<float, 4> controls{};
    std::uint32_t scope{};
    std::uint32_t soldier{};
    float delta{};
    std::int32_t state{-1};
    float energy{};
    // Native exhaustion flags and trigger edges accompany soldier/weapon elapsed seconds and packed posture.
    std::uint32_t energy_flags{};
    float soldier_state_time{};
    std::uint32_t sprint_trigger{};
    std::uint32_t secondary_fire_trigger{};
    std::uint32_t posture{};
    float notified_state_time{};
    std::uint32_t collision_flags{};
    std::uint8_t stage{};
    std::uint8_t result{};
    std::uint32_t parent_scope{};
    // Whole simulation entry/return supply the actual basis; collision cuts leave basis_present zero.
    std::array<std::array<float, 3>, 3> basis{};
    std::uint8_t basis_present{};
};

// Custom XMM operands and applied scalar outputs at actual native integration decisions.
struct IntegrationRecord {
    TurnContext turn;
    std::array<float, 8> values{};
    std::array<std::uint32_t, 6> words{};
    std::uint32_t scope{};
    std::uint32_t soldier{};
    std::uint32_t eflags{};
    std::uint8_t stage{};
    // These fields replace the same-cut Simulation copy only when outcome is set.
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::array<float, 3> normal{};
    std::uint8_t outcome{};
};

// UpdateFire's evaluated admission, contact, block and damage boundaries; words retain operation-specific native data.
struct CombatDecisionRecord {
    TurnContext turn;
    std::array<float, 6> values{};
    std::array<std::uint32_t, 7> words{};
    std::uint32_t scope{};
    std::uint32_t target{};
    std::uint32_t native_target{};
    std::uint32_t row{};
    std::uint32_t cache{};
    std::uint32_t eflags{};
    std::uint8_t stage{};
    std::uint8_t result{};
};

// Native state commits, notifications and locomotion branches retain their actual operands in one simulation scope.
struct SoldierDecisionRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t caller{};
    std::int32_t state{-1};
    float state_time{};
    std::uint32_t posture{};
    float notified_time{};
    std::uint32_t sprint_trigger{};
    float energy{};
    std::uint32_t energy_flags{};
    std::uint32_t collision_flags{};
    std::array<float, 4> values{};
    std::array<std::uint32_t, 4> words{};
    std::uint8_t stage{};
    std::uint8_t result{};
};

// The action-animation producer retains direction arguments and reset flags before it changes either lane.
struct AnimationDecisionRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t caller{};
    std::array<float, 4> values{};
    std::array<std::uint32_t, 4> arguments{};
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};
    std::uint32_t action{};
    std::uint32_t weapon_animation{};
    std::uint16_t flags{};
    std::uint16_t pending_flags{};
    std::uint8_t stage{};
};

// Actual EnergyBar bit calls accompany native bounds and values; codes are the bits passed to or returned from the codec.
struct EnergyCodecRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t caller{};
    std::uint32_t stream{};
    std::uint32_t bit_offset{UINT32_MAX};
    std::uint32_t code{};
    std::int32_t bits{};
    float energy{};
    float lower{};
    float upper{};
    std::uint32_t energy_flags{};
    std::uint8_t encoding{};
    std::uint8_t scalar{};
    std::uint8_t stage{};
};

// Integration cuts retain the native movement basis; support cuts retain its velocity and matrix translation.
// Rotation stages 8/9 retain angle in values[0] and output pointers in words[0..1]; stage 9 adds outputs in values[1..2].
struct MotionOperandsRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::array<float, 3> basis_f0{};
    std::array<float, 3> basis_110{};
    std::array<std::array<float, 3>, 2> basis{};
    std::array<float, 8> values{};
    std::array<std::uint32_t, 4> words{};
    std::uint8_t stage{};
    // These fields replace the same-cut Simulation copy only when outcome is set.
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::array<float, 3> normal{};
    std::uint8_t outcome{};
};

// Serializer cuts retain actual writer quaternion and decoded matrix basis; later samples retain applied simulation orientation.
// Stages 0/1 mark write/decode, 2 follows Soldier Read, 3/4 surround simulation, and 5/6 surround the ground rotation setter.
struct OrientationRecord {
    TurnContext turn;
    std::array<std::array<float, 3>, 3> basis{};
    std::array<float, 4> quaternion{};
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::uint32_t scope{};
    std::uint32_t stream{};
    std::uint32_t bit_offset{UINT32_MAX};
    std::uint8_t stage{};
};

// Joins one native producer or FC owner branch to its enclosing native invocation.
struct CauseRecord {
    TurnContext turn;
    OwnerDecision decision;
    std::uint32_t weapon{};
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
};

// Lower levels copy only the documented operands for this owner kind/stage.
// value_count floats and word_count words follow this fixed prefix, in that order.
struct CauseSummaryRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::uint32_t weapon{};
    std::uint32_t occurrence{};
    CauseKind kind{};
    std::uint8_t stage{};
    std::uint8_t result{};
    std::uint8_t reason{};
    std::uint8_t value_count{};
    std::uint8_t word_count{};
};

// One action boundary carries requested/effective native action and both installed animation lanes.
struct ActionFrameRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::uint32_t pose_scope{};
    std::uint32_t caller{};
    std::uint32_t requested{};
    std::uint32_t argument{};
    std::uint32_t current{};
    std::uint32_t action{};
    std::uint32_t previous{};
    std::uint32_t weapon_animation{};
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};
    float timer{};
    float upper_phase{};
    float lower_phase{};
    std::array<float, 3> requested_position{};
    std::uint16_t flags{};
    std::uint16_t pending_flags{};
};

// Computed selection outcomes retain exact native turn counters and ledger IDs.
struct SelectionRecord {
    TurnContext turn;
    SelectionDecision decision;
    std::uint32_t scope{};
    std::uint32_t soldier{};
};

// Standard records private clocks and motion at authority and completed-prediction boundaries.
struct BoundaryRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::uint32_t scope{};
    std::int32_t mode{-1};
    float mode_time{};
    std::int32_t combo{-1};
    float combo_time{};
    float input_time{};
    float soldier_state_time{};
    float notified_state_time{};
    std::int32_t soldier_state{-1};
    LocalStage stage{};
    std::uint8_t result{};
    std::uint8_t state_flags{};
    // Authority return compares its private clocks and motion with the saved entry under this scope.
    std::uint8_t compared{};
    std::uint8_t changed{};
};

// Every observed body/camera sample retains cadence and an explicit changed or unchanged position.
struct DisplayPositionRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::uint32_t scope{};
    std::uint32_t source{};
    std::uint32_t simulation_scope{};
    std::uint32_t pose_scope{};
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};
    std::uint32_t action{};
    LocalStage stage{};
    std::uint8_t changed{};
};

// Standard retains the completed collision outcome and its enclosing simulation scope.
struct SimulationOutcomeRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::int32_t state{-1};
    float energy{};
    std::uint32_t energy_flags{};
    std::uint8_t collision_flags{};
    std::uint8_t movement_flags{};
    std::uint8_t stage{};
    std::uint8_t result{};
};

// Combat records each evaluated condition and its initialized successful output; raw row operands remain Full.
struct ConditionOutcomeRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::uint32_t condition{};
    std::uint32_t candidate_ordinal{};
    float start{};
    float end{};
    float input_clock{};
    float output{};
    std::uint8_t result{};
    std::uint8_t stage{};
};

// Combat retains simulation boundary pairs and selected controls without Full basis and integration operands.
struct SimulationSummaryRecord {
    TurnContext turn;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::uint32_t scope{};
    float delta{};
    std::int32_t state{-1};
    float energy{};
    std::uint32_t energy_flags{};
    float state_time{};
    float input_time{};
    float notified_state_time{};
    std::uint8_t collision_flags{};
    std::uint8_t movement_flags{};
    std::uint8_t stage{};
    std::uint8_t result{};
    std::uint32_t parent_scope{};
    std::array<float, 4> controls{};
    float combo_time{};
    float mode_time{};
};

// Combat retains requested state/admission and native outcomes; predicate operands remain Full.
struct SoldierOutcomeRecord {
    TurnContext turn;
    std::uint32_t scope{};
    std::int32_t state{-1};
    float state_time{};
    float energy{};
    std::uint32_t energy_flags{};
    std::array<float, 4> arguments{};
    std::array<std::uint32_t, 3> requested{};
    std::uint8_t stage{};
    std::uint8_t result{};
};

// Actual server-prepared row sampled at the melee update, including qualified source provenance.
struct PreparedUseRecord {
    TurnContext turn;
    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint32_t row{};
    std::uint32_t scope{};
    std::int32_t source_turn{-1};
    InputOrigin origin{};
    std::uint8_t joystick{};
    std::uint8_t changed{};
};

// One controller completion pairs its actual selected input with trigger words before and after processing.
struct InputUseRecord {
    TurnContext turn;
    std::array<float, 4> axes{};
    std::uint32_t input_bits{};
    std::uint32_t row{};
    std::uint32_t scope{};
    std::uint32_t parent_scope{};
    std::array<std::uint32_t, 4> triggers_before{};
    std::array<std::uint32_t, 4> triggers_after{};
    float delta{};
    std::int32_t source_turn{-1};
    InputOrigin origin{};
    std::uint8_t joystick{};
    std::uint8_t selected{};
};

template <class Payload> struct Record {
    RecordHeader header;
    Payload payload;
};

#pragma pack(pop)

template <class Payload>
inline constexpr bool kValidRecord = std::is_trivially_copyable_v<Record<Payload>> && std::is_standard_layout_v<Record<Payload>> &&
                                     sizeof(Record<Payload>) <= kMaximumRecordSize;

static_assert(kValidRecord<BoundaryRecord>);
static_assert(kValidRecord<SimulationOutcomeRecord>);
static_assert(kValidRecord<ConditionOutcomeRecord>);
static_assert(kValidRecord<DisplayPositionRecord>);
static_assert(kValidRecord<SimulationSummaryRecord>);
static_assert(kValidRecord<SoldierOutcomeRecord>);
static_assert(kValidRecord<InputUseRecord>);
static_assert(kValidRecord<PreparedUseRecord>);
static_assert(kValidRecord<CauseSummaryRecord>);
static_assert(sizeof(Record<CauseSummaryRecord>) + sizeof(OwnerDecision::values) + sizeof(OwnerDecision::words) <= kMaximumRecordSize);
static_assert(kValidRecord<ActionFrameRecord>);
static_assert(kValidRecord<CauseRecord>);
static_assert(kValidRecord<SelectionRecord>);
static_assert(kValidRecord<SubjectRecord>);
static_assert(kValidRecord<MeleeTickRecord>);
static_assert(kValidRecord<InputDecisionRecord>);
static_assert(kValidRecord<NetworkStateRecord>);
static_assert(kValidRecord<TransitionRecord>);
static_assert(kValidRecord<MovementFrameRecord>);
static_assert(kValidRecord<NetworkMovementRecord>);
static_assert(kValidRecord<PresentationFrameRecord>);
static_assert(kValidRecord<MeleeContactRecord>);
static_assert(kValidRecord<LocomotionRecord>);
static_assert(kValidRecord<AudioCueRecord>);
static_assert(kValidRecord<LocalStateRecord>);
static_assert(kValidRecord<LocalStateBasisRecord>);
static_assert(kValidRecord<DisplayMatrixRecord>);
static_assert(kValidRecord<NativeQueueRecord>);
static_assert(kValidRecord<ConditionRecord>);
static_assert(kValidRecord<HistoryRecord>);
static_assert(kValidRecord<PhysicsRecord>);
static_assert(kValidRecord<DispatchRecord>);
static_assert(kValidRecord<SubmissionRecord>);
static_assert(kValidRecord<AnimationClockRecord>);
static_assert(kValidRecord<AssetRecord>);
static_assert(kValidRecord<LifecycleRecord>);
static_assert(kValidRecord<PredicateRecord>);
static_assert(kValidRecord<AnimationAdvanceRecord>);
static_assert(kValidRecord<AnimationSelectionRecord>);
static_assert(kValidRecord<AssetNameRecord>);
static_assert(kValidRecord<GenericTimerRecord>);
static_assert(kValidRecord<SimulationRecord>);
static_assert(kValidRecord<IntegrationRecord>);
static_assert(kValidRecord<CombatDecisionRecord>);
static_assert(kValidRecord<SoldierDecisionRecord>);
static_assert(kValidRecord<AnimationDecisionRecord>);
static_assert(kValidRecord<EnergyCodecRecord>);
static_assert(kValidRecord<MotionOperandsRecord>);
static_assert(kValidRecord<OrientationRecord>);

} // namespace fc_plugins::hero_diagnostics::trace
