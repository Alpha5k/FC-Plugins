#pragma once

#include <cstdint>
#include <type_traits>

namespace fc_plugins::sound_diagnostics::trace {

inline constexpr std::uint32_t kSchemaVersion = 11;

enum class RecordKind : std::uint16_t {
    MixerCreated = 1,
    MixerClosing = 2,
    RendererCreated = 3,
    RendererClosing = 4,
    RendererFormat = 5,
    RendererReset = 6,
    RendererUpdate = 8,
    OwnerIteration = 9,
    Request = 10,
    RequestResult = 11,
    VoiceBind = 12,
    VoiceStart = 13,
    VoiceStop = 14,
    VoiceInactive = 15,
    Controller = 16,
    StreamStart = 17,
    StreamStop = 18,
    Coverage = 19,
    LayeredBinding = 20,
    NativeApi = 21,
    StreamBinding = 22,
    StreamState = 23,
    VoiceAssignment = 24,
    StreamQueued = 25,
    StreamRoute = 27,
    VoiceAdmission = 28,
    SourceRoute = 29,
    PhysicalRoute = 33,
    UiEvent = 34,
    UiCallback = 35,
    PacketProduced = 36,
    VoiceDecision = 37,
    SaberDecision = 38,
    SourceEnd = 39,
    PacketGap = 40,
    SoundReset = 41,
    StopAll = 42,
    HealthDecision = 43,
    UiQueued = 44,
    AmbientBinding = 45,
    BindingMutation = 46,
    NativeProducer = 47,
    FootstepDecision = 50,
    FootMaintenance = 51,
    RequestRun = 52,
    SourceRouteRun = 53,
    OwnerProgress = 54,
};
enum class SourceKind : std::uint8_t {
    Unknown,
    Game,
    Ordinary,
    Layered,
    Stream,
    Controlled
};

// FC adds timing, thread and accepted-record ordering. Pointer identities can repeat after a closing record.
struct Header {
    std::uint32_t sequence{};
    RecordKind kind{};
    SourceKind source{SourceKind::Unknown};
    std::uint8_t reserved{};
    std::uint64_t object{};
    bool operator==(const Header &) const = default;
};

// Native renderer format codes remain native.
struct StreamRecord {
    Header header{};
    std::uint32_t frequency{};
    std::uint32_t channels{};
    std::uint32_t format{};
    std::uint32_t capacity_bytes{};
    std::int32_t gain{}; // Mixer Q15 gain, or renderer channel-zero interpolated gain before its upper-word extraction.
    std::uint8_t hardware_gain{};
    std::uint8_t accepted{}; // RendererFormat result; other lifetime records do not imply an open device.
    std::uint8_t reserved[2]{};
};

// start_ticks identifies update entry so consumers can derive service gaps within each object lifetime.
struct RendererUpdateRecord {
    Header header{};
    std::uint64_t start_ticks{};
    std::uint64_t duration_ticks{}; // Includes enabled diagnostic callbacks during the native update.
    std::uint64_t counter_frequency{};
    std::uint32_t requested_bytes{};
    std::uint32_t written_bytes{};   // Native WriteData results include silence fill.
    std::uint32_t zero_fill_bytes{}; // Observed native silence-fill branch, distinct from payload consumption.
    std::uint32_t consumed_bytes{};  // Native modulo play-cursor delta; complete buffer laps cannot be inferred.
    std::uint32_t capacity_bytes{};
    std::uint32_t play_cursor{};
    std::uint32_t write_cursor{};
    std::uint32_t next_lock_before{};
    std::uint32_t buffered_before{};
    std::uint32_t left_to_play_before{};
    std::uint32_t frequency{};
    std::uint32_t channels{};
    std::uint32_t format{};
    std::int32_t gain_current[2]{};
    std::int32_t position_result{};
    std::int32_t lock_result{};
    std::int32_t unlock_result{};
    std::uint32_t exhausted_writes{}; // Native queue-exhaustion path; this alone does not establish an underrun.
    std::uint32_t resets{};           // Native Flush invocations nested in this update.
    std::uint32_t cursor_resets{};    // Native next-lock cursor correction to the device write cursor.
    std::uint8_t position_observed{};
    std::uint8_t cursors_observed{};
    std::uint8_t lock_observed{};
    std::uint8_t unlock_observed{};
    std::uint8_t hardware_gain{};
    std::uint8_t reserved[7]{};
    std::uint64_t owner_iteration{};
    std::uint64_t device{}; // Existing DirectSound interface at +0xF8; no diagnostic query.
    std::uint32_t next_lock_after{};
    std::uint32_t buffered_after{};
    std::uint32_t left_to_play_after{};
    std::uint32_t position_before{};
    std::uint32_t position_after{};
    std::uint16_t transport_before{};
    std::uint16_t transport_after{};
    std::uint64_t input_node{};
    std::uint32_t input_channel{};
    std::int32_t gain_target[2]{};
    std::uint8_t raw_cursors_observed{};
    std::uint8_t reserved_tail[3]{};
    // These remain the device values even when native code later adjusts its logical cursors.
    std::uint32_t raw_play_cursor{};
    std::uint32_t raw_write_cursor{};
    std::uint64_t position_ticks{};
    std::uint64_t request_ticks{};
    std::uint64_t lock_ticks{};
    std::uint64_t unlock_ticks{};
    std::uint64_t callbacks{1}, last_start_ticks{}, last_end_ticks{}, last_owner_iteration{};
    std::uint64_t max_service_gap{}, gap_start_ticks{}, gap_end_ticks{};
    std::uint64_t max_duration{}, duration_start_ticks{};
    std::uint64_t gap_position_ticks{}, gap_request_ticks{}, gap_lock_ticks{}, gap_unlock_ticks{};
    std::uint64_t duration_position_ticks{}, duration_request_ticks{}, duration_lock_ticks{}, duration_unlock_ticks{};
    bool operator==(const RendererUpdateRecord &) const = default;
};

// Every iteration preserves exact local wait/sleep spans, including a skipped wait and the final sleep.
struct OwnerRecord {
    Header header{};
    std::uint64_t iteration{};
    std::uint64_t wait_start{};
    std::uint64_t wait_end{};
    std::uint64_t sleep_start{};
    std::uint64_t sleep_end{};
    std::uint64_t frequency{};
    std::uint64_t first_callback{};
    std::uint64_t last_callback{};
    std::uint32_t requested_sleep_ms{};
    std::uint32_t renderer_callbacks{};
    std::uint32_t idle_callbacks{};
    std::uint32_t nodes_visited{}; // Native owner loop counter includes paused nodes.
    std::uint8_t active{};
    std::uint8_t pause_requested{};
    std::uint8_t waited{};
    std::uint8_t reserved[1]{};
};

// Standard capture keeps callback counts and the largest wait/sleep spans in compact progress records.
struct OwnerProgressRecord {
    Header header{};
    std::uint64_t first_iteration{}, last_iteration{}, first_ticks{}, last_ticks{}, frequency{}, iterations{};
    std::uint64_t renderer_callbacks{}, idle_callbacks{}, nodes_visited{}, first_callback{}, last_callback{};
    std::uint64_t max_wait{}, wait_start{}, wait_end{}, max_sleep{}, sleep_start{}, sleep_end{};
    std::uint32_t requested_sleep_ms{};
    std::uint8_t active{}, pause_requested{}, waited{}, had_callback{};
};

// Parent requests and Hero operation scopes connect nested ordinary/layered/stream calls to one logical action.
struct RequestRecord {
    Header header{};
    std::uint64_t request{};
    std::uint64_t parent{};
    std::uint64_t caller{};
    std::uint64_t descriptor{};
    std::uint64_t position_pointer{};
    std::uint64_t callback{};
    std::uint64_t callback_data{};
    std::uint64_t subject{};
    std::uint64_t generation{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::uint64_t weapon{};
    std::uint64_t source_object{};
    std::int32_t prediction_turn{};
    std::int32_t update_turn{};
    std::int32_t acknowledged_turn{};
    float position[3]{};
    std::uint32_t starts{};
    std::uint8_t descriptor_type{};
    std::uint8_t phase{};
    std::uint8_t position_valid{};
    std::uint8_t reserved{};
    std::uint32_t rejection{}; // Native branch codes; zero means no observed rejection.
    std::uint32_t definition_id{};
    std::int32_t host_turn{-1};
    std::uint64_t ui_event{};
    std::uint64_t ui_callback{};
    std::uint64_t site{};          // Qualified original instruction address, independent of trampoline EIP.
    std::uint64_t control_cause{}; // Enclosing reset or manager-wide stop, when present.
    std::uint64_t entry_ticks{}, end_ticks{};
    std::uint16_t controller_before{}, controller_after{};
    std::uint8_t controller_flags_before{}, controller_flags_after{};
    std::uint8_t controller_reason{}, stop_argument{};
    std::uint8_t completed{};    // 0 immutable origin emitted before a nested effect; 1 completed outcome.
    std::uint8_t native_flags{}; // Actual native Boolean stack arguments, packed in declaration order.
    std::uint8_t reserved_tail[6]{};
    bool operator==(const RequestRecord &) const = default;
};

// Repeated refusals reference one completed outcome; interior poll times and IDs are omitted.
// Abrupt recording/process stop can omit the unfinished repeat tail before its next checkpoint.
struct RequestRunRecord {
    Header header{};
    std::uint64_t first_request{}, last_request{}, first_ticks{}, last_ticks{}, count{}; // Count includes first_request's exact outcome.
    std::uint64_t last_operation_scope{}, last_read_scope{};
    std::int32_t last_host_turn{}, last_prediction_turn{}, last_update_turn{}, last_acknowledged_turn{};
    float last_position[3]{};
    std::uint8_t last_position_valid{}; // Header reserved bit 0 marks an explicitly unattributed recurring poll.
};

// Completion carries only results and descriptor resolution after a materialized origin.
struct RequestResultRecord {
    Header header{};
    std::uint64_t request{};
    std::uint64_t descriptor{};
    std::uint32_t starts{};
    std::uint32_t rejection{};
    std::uint32_t definition_id{};
    std::uint8_t descriptor_type{};
    std::uint8_t reserved[7]{};
    std::uint16_t controller_before{}, controller_after{};
    std::uint8_t controller_flags_before{}, controller_flags_after{}, controller_reason{}, stop_argument{};
};

// A generation is assigned after native SetupVoiceVirtual replaces the old binding; restart preserves it.
struct VoiceRecord {
    Header header{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t sample{};
    std::uint64_t physical{};
    std::uint64_t physical_after{};
    std::uint64_t callback{};
    std::uint64_t callback_data{};
    std::uint64_t caller{};
    std::uint32_t position_before{};
    std::uint32_t position_after{};
    std::uint32_t inserted_before{};
    std::uint32_t inserted_after{};
    std::uint16_t transport_before{};
    std::uint16_t transport_after{};
    std::uint8_t restart{};
    std::uint8_t inside_stop{};
    std::uint8_t reserved[2]{};
    std::uint32_t sample_frequency{};
    std::uint32_t sample_length{};
    std::uint64_t control_cause{};
};

// Native API observations reuse existing calls; HRESULT is preserved before native bool conversion.
struct ApiRecord {
    Header header{};
    std::uint64_t device{};
    std::uint64_t renderer{};
    std::uint64_t request{};
    std::uint64_t caller{};
    std::int32_t result{};
    std::uint8_t operation{}; // 1 Play, 2 Stop, 3 SetCurrentPosition, 4 Flush Lock, 5 Flush Unlock.
    std::uint8_t reserved[3]{};
};

// Offline joins use the latest binding generation for an object; delayed requests name their task object.
struct BindingRecord {
    Header header{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t descriptor{};
    std::uint64_t sample{};
    std::uint64_t input_node{};
    std::uint64_t renderer{};
    std::uint32_t definition_id{};
    std::uint32_t channel{};
    float delay{};
    std::uint8_t accepted{};
    std::uint8_t reserved[3]{};
};

// Route repeats refer to an observed assignment generation and retain only the unchanged observation range.
struct SourceRouteRunRecord {
    Header header{};
    std::uint64_t generation{}, first_ticks{}, last_ticks{}, count{}; // Count includes the initial exact route.
};

// Controller indices/flags and stream transport are native values, never overloaded request start counts.
struct StateRecord {
    Header header{};
    std::uint64_t request{};
    std::uint64_t caller{};
    std::uint64_t sample{};
    std::uint32_t position{};
    std::uint16_t transport{};
    std::uint8_t reason{}; // Native control boundary; before/after observations remain distinct.
    std::uint8_t argument{};
};

// Coverage counts attempts, failed submissions and fixed diagnostic scope/table overflow explicitly.
struct CoverageRecord {
    Header header{};
    std::uint64_t attempted{};
    std::uint64_t bytes{};
    std::uint64_t rejected{};
    std::uint64_t scope_overflows{};
    std::uint64_t voice_overflows{};
    std::uint64_t accepted{};
    std::uint64_t written{};
    std::uint64_t dropped{};
    std::uint8_t output_failed{};
    std::uint8_t file_limit_reached{};
    std::uint8_t hero_available{};
    std::uint8_t capture{}, role{};
    std::uint8_t reserved[3]{};
    std::uint64_t suppressed_requests{}, suppressed_routes{}, suppressed_renderers{};
    std::uint64_t empty_packet_returns{}; // Exact matched ordinary null returns, including repeated empty polls.
};

// Admission distinguishes a free ordinary voice from native priority reclamation before rebinding.
struct AdmissionRecord {
    Header header{};
    std::uint64_t request{};
    std::uint64_t sample{};
    std::uint64_t voice{};
    std::uint8_t free_voice{};
    std::uint8_t reserved[7]{};
};

// Direct ordinary hardware output joins the physical voice to its native stream and renderer/device.
struct PhysicalRouteRecord {
    Header header{};
    std::uint64_t stream_node{};
    std::uint64_t renderer{};
    std::uint64_t device{};
    std::uint64_t input_node{};
    std::uint32_t channel{};
    std::uint32_t mode{};
};

// Native event IDs distinguish pooled event addresses and separate HandleEvents invocations.
struct UiEventRecord {
    Header header{};
    std::uint64_t event{};
    std::uint64_t dispatch{};
    std::uint64_t caller{}; // Original HandleEvents caller captured before its typed invoker.
    std::uint32_t type{};
    std::uint32_t value{};
    std::int32_t controller{};
    std::uint32_t reserved{};
    char screen[64]{};
    char popup[64]{};
    char error[64]{};
    char name[64]{};
    std::uint64_t native_event{}; // Joins the queued event to its dispatch before the pooled address is freed.
};

// Screen transitions retain the original producer and enclosing callback before the event enters the native queue.
struct UiQueuedRecord {
    Header header{}; // Native event address; joins UiEvent.native_event in queue/dispatch order.
    std::uint64_t manager{};
    std::uint64_t caller{};
    std::uint64_t parent_event{};
    std::uint64_t parent_callback{};
    std::uint32_t type{};
    std::uint32_t value{};
    std::int32_t controller{};
    std::uint32_t reserved{};
    char name[64]{};
};

// One cause record covers every voice stopped by the same native manager operation.
struct ControlCauseRecord {
    Header header{};
    std::uint64_t cause{};
    std::uint64_t parent{};
    std::uint64_t caller{};
    std::uint64_t ui_event{};
    std::uint64_t ui_callback{};
    std::uint64_t subject{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{};
    std::int32_t update_turn{};
    std::uint8_t phase{};
    std::uint8_t reserved[3]{};
};

// Soldier::Update emits only executed Play/Stop decisions; operands precede the native controller mutation.
struct HealthDecisionRecord {
    Header header{}; // Soldier root, with native EDI offset removed.
    std::uint64_t site{};
    std::uint64_t caller{};
    std::uint64_t controller{};
    std::uint64_t properties{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    float health{};
    float maximum_health{};
    float threshold{};             // Native threshold fraction multiplied by maximum_health before COMISS against health.
    std::uint32_t soldier_flags{}; // Bit 3 participates in the warning decision.
    std::uint16_t controller_index{};
    std::uint8_t controller_flags{};
    std::uint8_t play{};
    std::uint32_t reserved{};
};

// The selected Lua table and callback are copied at entry; sound requests, resets and queued transitions emit records.
struct UiCallbackRecord {
    Header header{};
    std::uint64_t callback{};
    std::uint64_t event{};
    std::uint64_t site{}; // Qualified original instruction address, independent of trampoline EIP.
    char scope[64]{};
    char function[64]{};
    std::uint8_t completed{};
    std::uint8_t result{};
    std::uint8_t truncated{};
    std::uint8_t reserved[5]{};
};

// A returned ordinary packet identifies the recovery that closes an observed empty-source gap.
struct PacketRecord {
    Header header{};
    std::uint64_t production{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t physical{};
    std::uint64_t sample{};
    std::uint64_t packet{};
    std::uint32_t position{};
    std::uint32_t bytes{};
    std::uint16_t flags{};
    std::uint16_t references{};
    std::uint32_t reserved{};
};

enum class VoiceDecision : std::uint8_t {
    VirtualSampleEnd = 1,
    DownstreamStopped
};

// Native end branches name the observed cause; Stop callers distinguish explicit requests and replacement.
struct VoiceDecisionRecord {
    Header header{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t physical{};
    std::uint64_t sample{};
    std::uint64_t site{}; // Qualified original instruction address, independent of trampoline EIP.
    std::uint32_t position{};
    std::uint32_t length{};
    std::uint16_t transport{};
    std::uint16_t physical_transport{};
    VoiceDecision decision{};
    std::uint8_t reserved[3]{};
};

// Native PacketComplete supplies separate end-of-sample and requested-transport flags.
struct SourceEndRecord {
    Header header{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t physical{};
    std::uint64_t sample{};
    std::uint64_t packet{};
    std::uint32_t bytes{};
    std::uint32_t position{};
    std::uint32_t segment_bytes{};
    std::uint32_t next_segment_bytes{};
    std::uint32_t desired_transport{};
    std::uint16_t transport{};
    std::uint8_t sample_end{};
    std::uint8_t transport_end{};
    std::uint64_t data{};
    std::uint64_t source_base{};
    std::uint32_t segment_start{};
    std::uint32_t next_segment_start{};
    std::uint32_t total_bytes{};
    std::uint32_t stride{};
    std::uint8_t loop{};
    std::uint8_t reserved_origin[7]{};
};

// A gap emits once on entry and once on recovery, native end, or binding retirement; counts retain every null return.
struct PacketGapRecord {
    Header header{};
    std::uint64_t generation{};
    std::uint64_t request{};
    std::uint64_t physical{};
    std::uint64_t sample{};
    std::uint64_t first_ticks{};
    std::uint64_t last_ticks{};
    std::uint64_t null_returns{};
    std::uint64_t pending_packet{};
    std::uint32_t source_position{};
    std::uint32_t source_bytes{};
    std::uint32_t segment_bytes{};
    std::uint16_t source_transport{};
    std::uint16_t resampler_references{};
    std::uint8_t stage{}; // 1 empty transition, 2 recovered, 3 binding retired, 4 binding replaced, 5 native source end.
    std::uint8_t reserved[7]{};
};

enum class SaberDecision : std::uint8_t {
    AmbientStart = 1,
    AmbientStop,
    AmbientPropertiesChanged,
    MaskAdded,
    MaskRemoved
};

// UpdateEffects emits these only at a native controller decision, before it changes the remembered state.
struct SaberDecisionRecord {
    Header header{};
    std::uint64_t site{}; // Qualified original instruction address, independent of trampoline EIP.
    std::uint64_t caller{};
    std::uint64_t controller{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::uint64_t old_properties{};
    std::uint64_t new_properties{};
    std::uint32_t posture{}; // Base weapon selection and notified soldier posture bits at +0xAC.
    float turn_on_time{};
    std::uint8_t state_flags{};  // +0x179: saber-on bit 1 and remembered ambient-sound bit 2.
    std::uint8_t action_masks{}; // +0x17C: current saber mask in the high nibble.
    std::uint8_t sound_masks{};  // +0x17D: remembered sound mask in the high nibble.
    std::uint8_t desired_mask{};
    std::uint8_t ambient_flags{};
    std::uint8_t on_off_flags{};
    SaberDecision decision{};
    std::uint8_t reserved{};
    std::uint64_t ambient_wrapper{};
    std::uint64_t ambient_properties{}; // Direct wrapper value at the decision, distinct from the earlier getter operand.
    std::uint64_t desired_wrapper{};    // Nonzero only for AmbientPropertiesChanged; EDI still names its source wrapper.
    std::uint16_t ambient_index{};
    std::uint8_t ambient_type{};
    std::uint8_t desired_type{};
    std::uint8_t reserved_tail[4]{};
    std::uint64_t subject{};
    std::uint64_t generation{};
    std::uint64_t property_object{};
    std::uint64_t control_cause{};
};

// The wrapper and controller are separate native bindings; remembered flags explain their surrounding saber state.
struct AmbientBindingState {
    std::uint64_t properties{};
    std::uint16_t controller_index{};
    std::uint8_t descriptor_type{};
    std::uint8_t controller_flags{};
    std::uint8_t state_flags{};
    std::uint8_t sound_masks{};
    std::uint8_t reserved[2]{};
};

enum class AmbientBoundary : std::uint8_t {
    Read = 1,
    Controls
};

// One record describes a binding change across a native call; unchanged calls emit no record.
struct AmbientBindingRecord {
    Header header{}; // Weapon owning the embedded ambient wrapper and controller.
    std::uint64_t site{};
    std::uint64_t caller{};
    std::uint64_t controller{};
    std::uint64_t wrapper{};
    std::uint64_t subject{};
    std::uint64_t generation{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    AmbientBindingState before{};
    AmbientBindingState after{};
    AmbientBoundary boundary{};
    std::uint8_t phase{};
    std::uint8_t reserved[6]{};
};

enum class BindingOperation : std::uint8_t {
    Assign = 4
};

// Each selected native writer emits even when both endpoints are empty or unchanged.
struct BindingMutationRecord {
    Header header{}; // Current registered weapon; wrapper is always weapon+0xD8.
    std::uint64_t site{};
    std::uint64_t caller{};
    std::uint64_t control_cause{};
    std::uint64_t subject{};
    std::uint64_t generation{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::uint64_t wrapper{};
    std::uint64_t property_object{};
    std::uint64_t source{};
    std::uint64_t source_before_pointer{};
    std::uint64_t source_after_pointer{};
    AmbientBindingState before{};
    AmbientBindingState after{};
    std::uint32_t source_offset{}; // Qualified property wrapper offsets only; zero means another source.
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    BindingOperation operation{};
    std::uint8_t source_before_type{};
    std::uint8_t source_after_type{};
    std::uint8_t phase{};
};

enum class NativeProducer : std::uint8_t {
    Effect = 1,
    GroundReturn,
    Toggle,
    FireAudio
};

// Reuses ControlCause IDs for entry, completion, commit and actual sound branch events.
struct ProducerRecord {
    Header header{}; // Native producer object, independent of the selected weapon.
    std::uint64_t cause{};
    std::uint64_t parent{};
    std::uint64_t site{};
    std::uint64_t caller{};
    std::uint64_t subject{};
    std::uint64_t generation{};
    std::uint64_t weapon{};
    std::uint64_t operation_scope{};
    std::uint64_t read_scope{};
    std::uint64_t wrapper{};
    std::uint64_t descriptor{};
    std::uint64_t property_object{};
    std::uint64_t ui_event{};
    std::uint64_t ui_callback{};
    std::uint32_t arguments[4]{}; // Actual stack inputs, retained before native code reuses its argument slots.
    std::uint32_t definition_id{};
    std::int32_t selected_index{-1};
    std::int32_t prior_state{-1};
    std::int32_t current_state{-1};
    std::int32_t previous_state{-1};
    float prior_clock{};
    float current_clock{};
    float position[3]{};
    float velocity[3]{};
    std::uint32_t native_flags{};
    std::uint32_t result{};
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    std::uint16_t branch{}; // SoundClass/property descriptor offset; zero before a sound branch executes.
    NativeProducer producer{};
    std::uint8_t stage{}; // 1 entry, 2 completion, 3 sound branch, 4 state commit result.
    std::uint8_t descriptor_type{};
    std::uint8_t phase{};
    std::uint8_t commit_observed{};
    std::uint8_t commit_result{};
};

// Clip context comes from the live animator at the native height gate; geometry is not recorded.
struct FootPose {
    std::uint32_t scope{};
    std::uint32_t animator{};
    std::uint32_t pose{};
    std::uint32_t hash{};
    std::uint32_t upper_clip{};
    std::uint32_t lower_clip{};
    float phase[2]{};
    float blend[2]{};
    std::int32_t soldier_state{};
    std::uint8_t movement_flags{};
    std::uint8_t animation_flags{};
    std::uint16_t aim_flags{};
};

// Full ushort flags bracket the real native mutation; comparisons bits mean descent, ascent, and old below threshold.
struct FootGate {
    FootPose pose{};
    std::uint32_t other_invocations{};
    std::uint32_t property_object{};
    float old_height{};
    float current_height{};
    float epsilon{};
    float threshold{};
    float descending_difference{};
    float ascending_difference{};
    std::uint16_t flags_before{};
    std::uint16_t flags_after{};
    std::uint8_t comparisons{};
    std::uint8_t observed{};
    std::uint8_t source{}; // 1 native height gate, 2 continuity maintenance.
    std::uint8_t reserved{};
};

// Emitted only at native foot notifications; cause joins Requests and their admitted voices.
struct FootstepRecord {
    Header header{};
    std::uint64_t cause{};
    std::uint64_t caller{};
    std::uint64_t site{};
    std::uint32_t subject{};
    std::uint32_t generation{};
    std::uint32_t weapon{};
    std::uint32_t render_scope{};
    std::uint32_t read_scope{};
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    float old_heights[2]{};
    float sampled_heights[2]{};
    FootGate current{};
    FootGate arm{};
    std::uint32_t writer_scope{};
    float writer_heights[2]{};
    std::uint16_t writer_flags{};
    std::uint16_t flags_after_notification{};
    std::uint32_t other_invocations{};
    std::uint32_t other_since_writer{};
    std::uint32_t other_since_arm{};
    std::uint32_t other_scope{};
    std::uint32_t animator{};
    std::int32_t speed_class{};
    float speed{};
    float speed_threshold{};
    std::uint32_t sound_class{};
    std::uint32_t wrapper{};
    std::uint16_t branch{};
    std::uint8_t foot{};
    std::uint8_t stage{}; // 1 before notification, 2 before selected Play, 3 completed notification.
    std::uint8_t phase{};
    std::uint8_t gate_observed{};
    std::uint8_t writer_seen{};
    std::uint8_t baseline_matches{};
    std::uint8_t fast{};
    std::uint8_t speed_observed{};
    std::uint8_t writer_source{};    // 1 native final writer, 2 continuity maintenance.
    std::uint8_t sampled_observed{}; // Bits mark which native per-foot scalars were reached in this invocation.
};

// The patch producer reports completed maintenance even when native height tracking will return early.
struct FootMaintenanceRecord {
    Header header{};
    std::uint32_t subject{};
    std::uint32_t generation{};
    std::uint32_t weapon{};
    std::uint32_t render_scope{};
    std::uint32_t read_scope{};
    std::int32_t host_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    std::uint32_t pose{};
    std::uint32_t bones[2]{};
    float old_heights[2]{};
    float sampled_heights[2]{};
    float result_heights[2]{};
    std::uint16_t flags_before{};
    std::uint16_t flags_after{};
    std::uint8_t support{};
    std::uint8_t directions[2]{};
    std::uint8_t reason{};
    std::uint8_t phase{};
    std::uint8_t reserved[7]{};
};

static_assert(std::is_trivially_copyable_v<FootMaintenanceRecord>);
static_assert(sizeof(FootMaintenanceRecord) == 104);
static_assert(std::is_trivially_copyable_v<StreamRecord>);
static_assert(std::is_trivially_copyable_v<RendererUpdateRecord>);
static_assert(sizeof(StreamRecord) == 40);
static_assert(sizeof(RendererUpdateRecord) == 376);
static_assert(sizeof(OwnerRecord) == 104);
static_assert(sizeof(RequestRecord) == 232);
static_assert(sizeof(RequestResultRecord) == 64);
static_assert(sizeof(VoiceRecord) == 120);
static_assert(sizeof(ApiRecord) == 56);
static_assert(sizeof(BindingRecord) == 80);
static_assert(sizeof(StateRecord) == 48);
static_assert(sizeof(CoverageRecord) == 120);
static_assert(sizeof(AdmissionRecord) == 48);
static_assert(sizeof(PhysicalRouteRecord) == 56);
static_assert(sizeof(UiEventRecord) == 320);
static_assert(sizeof(UiQueuedRecord) == 128);
static_assert(sizeof(ControlCauseRecord) == 96);
static_assert(sizeof(HealthDecisionRecord) == 88);
static_assert(std::is_trivially_copyable_v<UiQueuedRecord>);
static_assert(std::is_trivially_copyable_v<ControlCauseRecord>);
static_assert(std::is_trivially_copyable_v<HealthDecisionRecord>);
static_assert(sizeof(UiCallbackRecord) == 176);
static_assert(sizeof(PacketRecord) == 80);
static_assert(sizeof(VoiceDecisionRecord) == 72);
static_assert(sizeof(SaberDecisionRecord) == 152);
static_assert(sizeof(AmbientBindingState) == 16);
static_assert(sizeof(AmbientBindingRecord) == 136);
static_assert(sizeof(SourceEndRecord) == 120);
static_assert(sizeof(PacketGapRecord) == 104);
static_assert(std::is_trivially_copyable_v<PacketGapRecord>);
static_assert(std::is_trivially_copyable_v<SourceEndRecord>);
static_assert(std::is_trivially_copyable_v<SaberDecisionRecord>);
static_assert(std::is_trivially_copyable_v<AmbientBindingRecord>);
static_assert(sizeof(BindingMutationRecord) == 168);
static_assert(sizeof(ProducerRecord) == 232);
static_assert(std::is_trivially_copyable_v<BindingMutationRecord>);
static_assert(std::is_trivially_copyable_v<ProducerRecord>);
static_assert(std::is_trivially_copyable_v<UiEventRecord>);
static_assert(std::is_trivially_copyable_v<UiCallbackRecord>);
static_assert(std::is_trivially_copyable_v<PacketRecord>);
static_assert(std::is_trivially_copyable_v<VoiceDecisionRecord>);

} // namespace fc_plugins::sound_diagnostics::trace
