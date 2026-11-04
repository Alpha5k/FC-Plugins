#pragma once

#include "api.hpp"

#include <cstdint>
#include <type_traits>

namespace fc_plugins::voice_diagnostics::trace {

inline constexpr std::uint32_t kSchemaVersion = 6;
inline constexpr std::uint32_t kChannelCapacity = 4096; // Multiplied by 80 for byte capacity; record and FC envelope sizes vary.
inline constexpr std::uint32_t kMaximumRecordSize = 192;

enum class RecordKind : std::uint16_t {
    WireIntake = 1,
    HeaderRead = 2,
    AudioIntake = 3,
    RelayBuild = 8,
    WireSend = 9,
    DecoderQueue = 10,
    JitterTarget = 11,
    FeatureState = 12,
    CodecOutcome = 13,
    RelayRecipients = 14,
    SelectedEnqueue = 15,
    StreamQueue = 16,
    NodeClosed = 17,
    QueueRetired = 18,
    PendingQueueFlush = 19,
    UiHealth = 20,
    UiQuery = 21,
    View = 22,
    HudStatus = 23,
    HudScan = 24,
    HudRow = 25,
    HudTime = 26,
    LocalActivity = 27,
    HudSubmit = 28,
    UiRender = 29,
    HudCreated = 31,
    HudActivity = 32,
    NetworkAdmission = 33,
    SequenceAdmission = 34,
    ReceiveOwner = 35,
};

// Classifies only size, data availability and the sampled magic prefix; FusionCutter does not imply frame validity.
enum class PayloadKind : std::uint8_t {
    Unavailable,
    Empty,
    Native,
    FusionCutter,
};

enum class RecoveryMode : std::uint8_t {
    None,
    ForwardErrorCorrection,
    Concealment,
    Invalid,
};

// Raw C++ records retain native alignment/padding; FC adds time, thread and accepted-record ordering in its envelope.
struct RecordHeader {
    std::uint32_t sequence{}; // Wrapping process-local attempt counter; concurrent calls can submit in a different order.
    RecordKind kind{};
    std::uint16_t reserved{};
    bool operator==(const RecordHeader &) const = default;
};

// Native voice dequeue, including local loopback; the packet has not yet passed header or audio validation.
struct WireIntakeRecord {
    RecordHeader header{};
    std::uint64_t packet{}; // Native pool packet address, which may be reused after release.
};

// Native header parsing precedes sequence checks and VoiceChat frame validation.
struct HeaderReadRecord {
    RecordHeader header{};
    std::uint64_t packet{}; // NetPacketAudio wrapper address, distinct from its underlying pool packet.
    std::uint8_t accepted{};
    std::uint8_t reserved[3]{};
};

// Summarizes the original wire descriptor after the local-delivery call and any VoiceChat recovery deliveries inside it.
struct AudioIntakeRecord {
    RecordHeader header{};
    std::uint64_t packet{}; // AudioPacket descriptor address; stack addresses can repeat across calls.
    std::uint32_t payload_bytes{};
    std::uint16_t missing_packets{};
    std::uint8_t sender{};
    std::uint8_t relay_requested{};      // Raw byte from an unused retail stack argument, with no established policy meaning.
    std::uint8_t remaining_recipients{}; // Zero can mean local destinations consumed the packet.
    PayloadKind payload_kind{};
    RecoveryMode recovery{};
    std::uint8_t discontinuity{}; // Raw descriptor flag byte; only bit zero has native discontinuity meaning.
    std::uint8_t frame_version{};
    std::uint8_t frame_flags{};
    std::uint32_t intake{};
    std::uint16_t voice_sequence{0xFFFF};
};

struct RelayBuildRecord {
    RecordHeader header{};
    std::uint32_t relay{};
    std::uint32_t payload_bytes{};
    std::uint8_t destination{};
    std::uint8_t built{}; // Native carrier was allocated and sending attempted; lower-level submission can still fail.
    PayloadKind payload_kind{};
    RecoveryMode recovery{};
};

struct WireSendRecord {
    RecordHeader header{};
    std::uint32_t relay{};        // Zero for sends outside an observed relay invocation.
    std::uint64_t packet{};       // Native pool packet address.
    std::uint32_t header_bytes{}; // Native routing header length in bytes.
    std::uint8_t destination{};   // 0xFF uses the host-facing native slot 64.
    std::uint8_t accepted{};      // Native local/network submission result; remote receipt and playback occur later.
    std::uint8_t reserved[2]{};
};

// Decoder acceptance can mean queuing borrowed audio or flushing an empty discontinuity marker.
struct DecoderQueueRecord {
    RecordHeader header{};
    std::uint64_t decoder{};
    std::uint64_t packet{}; // AudioPacket descriptor address.
    std::uint32_t stream{}; // Decoder-local stream assignment, independent of sender player ID.
    std::uint32_t payload_bytes{};
    std::uint8_t accepted{}; // Native intake/flush outcome; codec completion and audible output are separate.
    PayloadKind payload_kind{};
    RecoveryMode recovery{};
    std::uint8_t discontinuity{}; // Raw descriptor flag byte; bit zero marks native discontinuity.
    std::uint8_t frame_version{};
    std::uint8_t frame_flags{};
    std::uint8_t reserved[2]{};
};

// Reports target changes and repeats the first target after native node close invalidates the change cache.
struct JitterTargetRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint32_t target_packets{}; // Effective target queue depth after VoiceChat's clamp, independent of current occupancy.
    std::uint8_t stream{};
    std::uint8_t reserved[3]{};
};

// Copies VoiceChat's feature/control outcome with FC peer identity and separate native slot/player IDs.
struct FeatureStateRecord {
    RecordHeader header{};
    std::uint64_t peer_id{};
    std::uint32_t revision{};
    std::uint8_t peer_slot{};
    std::uint8_t player{};
    std::uint8_t flags{};
    std::uint8_t message_kind{};
    std::uint8_t result{};
    std::uint8_t send_result{0xFF}; // FC SendResult for publication attempts.
    std::uint8_t reserved[2]{};
};

// Copies completed/failed codec work; pending six-block work has no event and failure carries no detailed cause.
struct CodecOutcomeRecord {
    RecordHeader header{};
    std::uint64_t native_state{}; // Native codec-state address; object lifetime cannot be inferred from equality alone.
    std::uint8_t direction{};
    std::uint8_t result{};
    std::uint8_t reserved[2]{};
    std::uint64_t first_ms{}, last_ms{}; // Callback range, not a codec lifetime or wire-frame identity.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const CodecOutcomeRecord &) const = default;
};

// FC policy is evaluated only for native-selected destinations; untested FC policy has no inferred result.
struct RelayRecipientsRecord {
    RecordHeader header{};
    std::uint64_t tested{};
    std::uint64_t native_selected{};
    std::uint64_t policy_evaluated{};
    std::uint64_t fc_permitted{};
    std::uint64_t selected{};
    std::uint64_t exclusions[6]{}; // Mode, zero advertised row, talk, listen, capture readiness, playback readiness.
    std::uint32_t relay{};
    std::int32_t method{-1};
    std::uint16_t voice_sequence{};
    std::uint8_t sender{};
    std::uint8_t node_count{};
    std::uint64_t packet{}; // Entry descriptor, independently of native carrier pool identity.
    std::uint32_t payload_bytes{};
    PayloadKind payload_kind{};
    RecoveryMode recovery{};
    std::uint8_t discontinuity{}, frame_version{}, frame_flags{};
};

// Identity comes from the admitted native header, before VoiceChat creates recovery descriptors.
struct SelectedEnqueueRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint64_t packet{}; // Borrowed enqueue descriptor; it is not the native retained slot.
    std::uint32_t intake{}; // Zero means no observed admitted-wire scope.
    std::uint32_t payload_bytes{};
    std::uint16_t voice_sequence{}; // 0xFFFF means unavailable.
    std::uint8_t sender{};
    std::uint8_t recovery{};
    std::uint8_t discontinuity{};
    std::uint8_t accepted{};
};

// A retained slot can be retried until retirement. Close ends its identity; slot reuse after retirement starts another packet.
struct StreamQueueRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint64_t queue_storage{}; // Sender queue's descriptor array; zero for a temporary flush descriptor.
    std::uint64_t packet{};
    std::uint64_t decoder{};
    std::uint32_t payload_bytes{};
    std::uint32_t stream{0xFFFFFFFF};
    std::uint16_t slot{0xFFFF};
    std::uint8_t sender{};
    std::uint8_t accepted{}; // Node::DecodeStream result, distinct from decoder acceptance.
    std::uint8_t decoder_attempts{};
    std::uint8_t decoder_accepted{};
    std::uint8_t discontinuity{};
    std::uint8_t recovery{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const StreamQueueRecord &) const = default;
};

// Emitted after Node::Close with numeric addresses saved before teardown; closes the native node's queue identity.
struct NodeClosedRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint64_t decoder{};
    std::uint64_t queues{};
};

// Native FlushStreams is about to clear this occupied slot and advance the sender queue, including policy discards.
struct QueueRetiredRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint64_t queue_storage{};
    std::uint64_t packet{};
    std::uint16_t slot{};
    std::uint8_t sender{};
    std::uint8_t receive_allowed{}; // Native effective receive policy; true follows successful DecodeStream.
    std::uint8_t force_flush{};     // Raw FlushStreams argument, independent of how many slots retire.
};

// FlushPacket clears the occupied run at this sender queue's read head, then submits its marker to DecodeStream.
struct PendingQueueFlushRecord {
    RecordHeader header{};
    std::uint64_t node{};
    std::uint64_t queue_storage{};
    std::uint16_t first_slot{};
    std::uint16_t next_slot{};
    std::uint16_t capacity{};
    std::uint8_t sender{0xFF};
    std::uint8_t marker_accepted{}; // DecodeStream result after clearing; false does not undo native queue retirement.
};

// Producer decisions copy scalar operands and keep native addresses as numeric identities.
struct HudActivityRecord {
    RecordHeader header{};
    std::uint32_t hud_frame{};
    ActivityObservation value{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const HudActivityRecord &) const = default;
};

struct NetworkAdmissionRecord {
    RecordHeader header{};
    AdmissionObservation value{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const NetworkAdmissionRecord &) const = default;
};

struct SequenceAdmissionRecord {
    RecordHeader header{};
    SequenceObservation value{};
};

struct ReceiveOwnerRecord {
    RecordHeader header{};
    OwnerObservation value{};
};

template <class Record>
inline constexpr bool kValidRecord =
    std::is_standard_layout_v<Record> && std::is_trivially_copyable_v<Record> && sizeof(Record) <= kMaximumRecordSize;

static_assert(kValidRecord<WireIntakeRecord>);
static_assert(kValidRecord<HeaderReadRecord>);
static_assert(kValidRecord<AudioIntakeRecord>);
static_assert(kValidRecord<RelayBuildRecord>);
static_assert(kValidRecord<WireSendRecord>);
static_assert(kValidRecord<DecoderQueueRecord>);
static_assert(kValidRecord<JitterTargetRecord>);
static_assert(kValidRecord<FeatureStateRecord>);
static_assert(kValidRecord<CodecOutcomeRecord>);
static_assert(kValidRecord<RelayRecipientsRecord>);
static_assert(kValidRecord<SelectedEnqueueRecord>);
static_assert(kValidRecord<StreamQueueRecord>);
static_assert(kValidRecord<NodeClosedRecord>);
static_assert(kValidRecord<QueueRetiredRecord>);
static_assert(kValidRecord<PendingQueueFlushRecord>);
static_assert(kValidRecord<HudActivityRecord>);
static_assert(kValidRecord<NetworkAdmissionRecord>);
static_assert(kValidRecord<SequenceAdmissionRecord>);
static_assert(kValidRecord<ReceiveOwnerRecord>);
static_assert(sizeof(AudioIntakeRecord) == 40);
static_assert(sizeof(RelayRecipientsRecord) == 136);
static_assert(sizeof(SelectedEnqueueRecord) == 40);
static_assert(sizeof(StreamQueueRecord) == 80);
static_assert(sizeof(NodeClosedRecord) == 32);
static_assert(sizeof(QueueRetiredRecord) == 40);
static_assert(sizeof(PendingQueueFlushRecord) == 32);

} // namespace fc_plugins::voice_diagnostics::trace
