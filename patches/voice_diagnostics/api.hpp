#pragma once

#include <fc/patch_api.hpp>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace fc_plugins::voice_diagnostics {

enum class StateResult : std::uint8_t {
    Available,
    Unavailable,
    Accepted,
    Rejected,
    SendAccepted,
    SendRefused,
    SnapshotRow,
    SentSnapshotRow
};

enum class CodecDirection : std::uint8_t {
    Encode,
    Decode
};

enum class CodecResult : std::uint8_t {
    Complete,
    Failed
};

// Reports one feature availability or control message outcome; availability events leave message fields at defaults.
struct StateObservation {
    std::uint64_t peer_id{};      // Full FC peer identity, independent of the native connection slot.
    std::uint32_t revision{};     // Control message revision when a message was supplied.
    std::uint8_t peer_slot{0xFF}; // Native connection slot; 0xFF means unavailable.
    std::uint8_t player{0xFF};    // Native voice player ID; host snapshots use 0xFF.
    std::uint8_t flags{};
    std::uint8_t message_kind{};
    StateResult result{};
    std::uint8_t send_result{0xFF}; // FC SendResult for outgoing attempts; 0xFF for other observations.
};

// Reports a completed or failed codec operation; pending six-block work produces no observation.
struct CodecObservation {
    std::uint64_t native_state{}; // Native codec-state pointer value; its address may be reused after destruction.
    CodecDirection direction{};
    CodecResult result{};
};

// Records only FC policy that native recipient selection actually reached; native header exclusions have no FC reason.
enum RouteExclusion : std::uint8_t {
    Mode = 1U << 0U,
    NoAdvertisedFlags = 1U << 1U, // Zero source or destination row; negotiation versus disabled devices is not distinguished.
    TalkDisabled = 1U << 2U,
    ListenDisabled = 1U << 3U,
    CaptureUnavailable = 1U << 4U,
    PlaybackUnavailable = 1U << 5U,
};

struct RecipientObservation {
    std::uint8_t destination{};
    std::uint8_t native_selected{};
    std::uint8_t policy_evaluated{};
    std::uint8_t exclusions{};
};

// A selected enqueue copies this borrowed descriptor synchronously; native queue storage has a separate identity.
struct EnqueueObservation {
    std::uint64_t node{};
    std::uint64_t packet{};
    std::uint32_t payload_bytes{};
    std::uint8_t sender{};
    std::uint8_t recovery{}; // Zero current, one FEC, two PLC; an empty discontinuity is a flush.
    std::uint8_t discontinuity{};
    std::uint8_t accepted{};
};

enum class UiQuery : std::uint8_t {
    Glyph,
    Sending,
    ReceivePermission,
    LocalSending,
    HudActivity,
    HudPermission
};

enum class UiReason : std::uint8_t {
    Complete,
    NoPlayer,
    Inactive,
    WrongReceiver,
    NativeLocal,
    MaskDisabled,
    MaskReadFailed,
    TransportMissing,
    TransportReadFailed,
    SendUnavailable,
    ReceiveUnavailable
};

// Records values used by one UI query. Boolean 2 means unavailable or not reached; reason and call_failed explain failures.
struct UiObservation {
    std::uint64_t context{}; // Lua state for a glyph; native transport for other queries.
    std::int32_t row{-1};
    UiQuery query{};
    UiReason reason{};
    std::uint8_t player{0xFF};
    std::uint8_t receiver{0xFF};
    std::uint8_t local_player{0xFF};
    std::uint8_t mode{};
    std::uint8_t local_flags{};
    std::uint8_t player_flags{};
    std::uint8_t host_flags{};
    std::uint8_t result{}; // Glyph 0/1/4/5, or the actual Boolean result.
    std::uint8_t native_local{2};
    std::uint8_t receive_mask{2};
    std::uint8_t call_failed{};
    bool operator==(const UiObservation &) const = default;
};

// Copies every accepted client row, including empty rows, on the control thread.
struct ViewObservation {
    std::uint64_t session{};
    std::uint64_t local_peer{};
    std::uint64_t host_peer{};
    std::uint32_t local_revision{};
    std::uint32_t host_revision{};
    std::uint8_t players[64]{};
    std::uint8_t local_player{0xFF};
    std::uint8_t local_slot{0xFF};
    std::uint8_t mode{};
    std::uint8_t local_flags{};
    std::uint8_t host_flags{};
    std::uint8_t dirty{};
};

enum class ActivityReason : std::uint8_t {
    Complete,
    NoClock,
    CountUnavailable,
    OutsideNodeRange,
    InvalidActivity,
    ClockBeforeActivity,
    Expired
};

// Copies the HUD predicate's operands; lookup 0/1/2/3 means missing/found/not reached/call failed.
struct ActivityObservation {
    std::uint64_t transport{}, last_node{}, valid_node{};
    float now{}, last{-2.0F};
    std::uint16_t local_sequence{};
    std::uint8_t player{0xFF}, local_player{0xFF}, node_count{}, local_source{};
    std::uint8_t clock_available{}, count_evaluated{}, count_call_failed{};
    std::uint8_t last_lookup{2}, valid_lookup{2}, metrics_available{}, active{}, valid{}, result{};
    ActivityReason reason{};
    bool operator==(const ActivityObservation &) const = default;
};

enum class AdmissionReason : std::uint8_t {
    Accepted,
    NoPacket,
    ReadFailed,
    ShortPacket,
    SendUnavailable,
    Inactive,
    WrongSource,
    HeaderRejected,
    InvalidSpeaker,
    WrongSpeaker,
    WrongMode,
    ZeroIdentity,
    StaleIdentity
};

// Records the authenticated carrier and only identity checks reached by the network intake decision.
struct AdmissionObservation {
    std::uint64_t packet{}, received_identity{}, expected_identity{};
    std::uint32_t type{}, priority{};
    std::uint16_t payload_bytes{};
    std::uint8_t source{0xFF}, mode{}, source_flags{}, active{2}, parsed{2}, speaker{0xFF};
    std::uint8_t expected_evaluated{}, accepted{};
    AdmissionReason reason{};
    bool operator==(const AdmissionObservation &) const = default;
};

// Records the ten-bit sequence verdict before native receive progress changes.
struct SequenceObservation {
    std::uint64_t metrics{};
    float last_activity{};
    std::uint16_t incoming{}, prior{}, distance{};
    std::uint8_t player{}, active{}, accepted{};
};

enum class OwnerOutcome : std::uint8_t {
    MetricsReset,
    MissingNode,
    EchoSkipped,
    ClosedNode,
    Flushed,
    FlushCallFailed
};

// One reset row is followed by outcomes for each affected local node; native flush acceptance can be false.
struct OwnerObservation {
    std::uint64_t old_host{}, new_host{}, old_identity{}, new_identity{}, metrics{}, node{}, queues{};
    float old_last{}, new_last{};
    std::uint8_t player{}, old_active{}, new_active{}, flush_accepted{};
    OwnerOutcome outcome{};
};

// Optional passive recorder. Calls copy borrowed records synchronously and preserve the originating thread's last error.
struct VoiceDiagnosticsApi {
    static constexpr std::string_view id = "VoiceDiagnosticsApi.v5";
    // Returns 0 while recording is off, otherwise Standard=1 or Full=3.
    fc::Function<std::uint8_t() noexcept> capture;
    fc::Function<void(const StateObservation *) noexcept> state; // The producer owns the observation until this call returns.
    fc::Function<void(const CodecObservation *) noexcept> codec;
    fc::Function<void(const RecipientObservation *) noexcept> recipient;
    fc::Function<void(const EnqueueObservation *) noexcept> enqueue;
    fc::Function<void(const UiObservation *) noexcept> ui;
    fc::Function<void(const ViewObservation *) noexcept> view;
    fc::Function<void(const ActivityObservation *) noexcept> activity;
    fc::Function<void(const AdmissionObservation *) noexcept> admission;
    fc::Function<void(const SequenceObservation *) noexcept> sequence;
    fc::Function<void(const OwnerObservation *) noexcept> owner;
};

// Describes all fields copied during a control-state observation.
inline fc::RecordDescription fc_description(std::type_identity<StateObservation>) {
    return fc::describe<StateObservation>("VoiceStateObservation.v2", &StateObservation::peer_id, &StateObservation::revision,
                                          &StateObservation::peer_slot, &StateObservation::player, &StateObservation::flags,
                                          &StateObservation::message_kind, &StateObservation::result, &StateObservation::send_result);
}

// Describes the codec outcome independently of the native packet representation.
inline fc::RecordDescription fc_description(std::type_identity<CodecObservation>) {
    return fc::describe<CodecObservation>("VoiceCodecObservation.v1", &CodecObservation::native_state, &CodecObservation::direction,
                                          &CodecObservation::result);
}

// Describes a relay decision without reevaluating native or FC policy.
inline fc::RecordDescription fc_description(std::type_identity<RecipientObservation>) {
    return fc::describe<RecipientObservation>("VoiceRecipientObservation.v1", &RecipientObservation::destination,
                                              &RecipientObservation::native_selected, &RecipientObservation::policy_evaluated,
                                              &RecipientObservation::exclusions);
}

// Describes the actual selected-node enqueue result, independently of retained queue slots.
inline fc::RecordDescription fc_description(std::type_identity<EnqueueObservation>) {
    return fc::describe<EnqueueObservation>("VoiceEnqueueObservation.v1", &EnqueueObservation::node, &EnqueueObservation::packet,
                                            &EnqueueObservation::payload_bytes, &EnqueueObservation::sender, &EnqueueObservation::recovery,
                                            &EnqueueObservation::discontinuity, &EnqueueObservation::accepted);
}

// Describes the inputs and actual result of a voice UI query.
inline fc::RecordDescription fc_description(std::type_identity<UiObservation>) {
    return fc::describe<UiObservation>("VoiceUiObservation.v1", &UiObservation::context, &UiObservation::row, &UiObservation::query,
                                       &UiObservation::reason, &UiObservation::player, &UiObservation::receiver,
                                       &UiObservation::local_player, &UiObservation::mode, &UiObservation::local_flags,
                                       &UiObservation::player_flags, &UiObservation::host_flags, &UiObservation::result,
                                       &UiObservation::native_local, &UiObservation::receive_mask, &UiObservation::call_failed);
}

// Describes a complete FC voice view independently of native HUD storage.
inline fc::RecordDescription fc_description(std::type_identity<ViewObservation>) {
    return fc::describe<ViewObservation>("VoiceViewObservation.v1", &ViewObservation::session, &ViewObservation::local_peer,
                                         &ViewObservation::host_peer, &ViewObservation::local_revision, &ViewObservation::host_revision,
                                         &ViewObservation::players, &ViewObservation::local_player, &ViewObservation::local_slot,
                                         &ViewObservation::mode, &ViewObservation::local_flags, &ViewObservation::host_flags,
                                         &ViewObservation::dirty);
}

// Describes the sampled activity decision without repeating native calls.
inline fc::RecordDescription fc_description(std::type_identity<ActivityObservation>) {
    return fc::describe<ActivityObservation>(
        "VoiceActivityObservation.v1", &ActivityObservation::transport, &ActivityObservation::last_node, &ActivityObservation::valid_node,
        &ActivityObservation::now, &ActivityObservation::last, &ActivityObservation::local_sequence, &ActivityObservation::player,
        &ActivityObservation::local_player, &ActivityObservation::node_count, &ActivityObservation::local_source,
        &ActivityObservation::clock_available, &ActivityObservation::count_evaluated, &ActivityObservation::count_call_failed,
        &ActivityObservation::last_lookup, &ActivityObservation::valid_lookup, &ActivityObservation::metrics_available,
        &ActivityObservation::active, &ActivityObservation::valid, &ActivityObservation::result, &ActivityObservation::reason);
}

// Describes the sampled admission decision without repeating native calls.
inline fc::RecordDescription fc_description(std::type_identity<AdmissionObservation>) {
    return fc::describe<AdmissionObservation>(
        "VoiceAdmissionObservation.v1", &AdmissionObservation::packet, &AdmissionObservation::received_identity,
        &AdmissionObservation::expected_identity, &AdmissionObservation::type, &AdmissionObservation::priority,
        &AdmissionObservation::payload_bytes, &AdmissionObservation::source, &AdmissionObservation::mode,
        &AdmissionObservation::source_flags, &AdmissionObservation::active, &AdmissionObservation::parsed, &AdmissionObservation::speaker,
        &AdmissionObservation::expected_evaluated, &AdmissionObservation::accepted, &AdmissionObservation::reason);
}

// Describes the sampled sequence decision without repeating native calls.
inline fc::RecordDescription fc_description(std::type_identity<SequenceObservation>) {
    return fc::describe<SequenceObservation>("VoiceSequenceObservation.v1", &SequenceObservation::metrics,
                                             &SequenceObservation::last_activity, &SequenceObservation::incoming,
                                             &SequenceObservation::prior, &SequenceObservation::distance, &SequenceObservation::player,
                                             &SequenceObservation::active, &SequenceObservation::accepted);
}

// Describes the sampled owner decision without repeating native calls.
inline fc::RecordDescription fc_description(std::type_identity<OwnerObservation>) {
    return fc::describe<OwnerObservation>("VoiceOwnerObservation.v1", &OwnerObservation::old_host, &OwnerObservation::new_host,
                                          &OwnerObservation::old_identity, &OwnerObservation::new_identity, &OwnerObservation::metrics,
                                          &OwnerObservation::node, &OwnerObservation::queues, &OwnerObservation::old_last,
                                          &OwnerObservation::new_last, &OwnerObservation::player, &OwnerObservation::old_active,
                                          &OwnerObservation::new_active, &OwnerObservation::flush_accepted, &OwnerObservation::outcome);
}

// Qualifies the public synchronous observation table before a producer retains it.
inline fc::ApiContract<VoiceDiagnosticsApi> api_contract() {
    return fc::api<VoiceDiagnosticsApi>(VoiceDiagnosticsApi::id, &VoiceDiagnosticsApi::capture, &VoiceDiagnosticsApi::state,
                                        &VoiceDiagnosticsApi::codec, &VoiceDiagnosticsApi::recipient, &VoiceDiagnosticsApi::enqueue,
                                        &VoiceDiagnosticsApi::ui, &VoiceDiagnosticsApi::view, &VoiceDiagnosticsApi::activity,
                                        &VoiceDiagnosticsApi::admission, &VoiceDiagnosticsApi::sequence, &VoiceDiagnosticsApi::owner);
}

} // namespace fc_plugins::voice_diagnostics
