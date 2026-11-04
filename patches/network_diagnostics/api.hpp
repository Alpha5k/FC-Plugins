#pragma once

#include <fc/patch_api.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace fc_plugins::network_diagnostics {

enum class PacketCarrier : std::uint8_t {
    Native,
    Direct,
    NotSent,
};

enum class DirectAssociationPhase : std::uint8_t {
    Started,
    Terminal,
    Ended,
};

enum class DirectRoute : std::uint8_t {
    Unclassified,
    Negotiating,
    Native,
    Direct,
};

struct DirectAssociationObservation {
    // Native player slot, distinct from the FC peer and protocol connection IDs below.
    std::int32_t slot{};

    // Full FC peer identity; zero means the producer has no FC peer identity for this association.
    std::uint64_t peer_id{};
    // Direct protocol connection identity, distinct from the native slot and FC peer ID.
    std::uint32_t connection_id{};

    DirectAssociationPhase phase{};
    DirectRoute route{DirectRoute::Unclassified};

    // 0 is none; 1-4 are offer, proof, ready, and commit timeouts; 5-9 are socket unavailable, endpoint unavailable,
    // commit, implicit commit, and random value generation failure.
    std::uint8_t route_reason{};

    // 0-4 are none, disconnected, replaced, policy removal, and shutdown.
    std::uint8_t end_reason{};

    // Negotiation attempts at route selection; Started and Ended report zero.
    std::uint32_t attempts{};
    // Producer counters cover Direct datagrams for this association.
    std::uint32_t tx_datagrams{};
    std::uint32_t rx_datagrams{};
    std::uint32_t send_failures{};
    std::uint32_t endpoint_rejects{};
    std::uint32_t authentication_rejects{};
    std::uint32_t replay_rejects{};
    std::uint32_t invalid_rejects{};

    // Milliseconds since the association started and since the Direct route was selected; direct_ms is zero before selection.
    std::uint64_t elapsed_ms{};
    std::uint64_t direct_ms{};
};

struct DirectReceiveObservation {
    // Native player slot, distinct from the FC peer and protocol connection IDs below.
    std::int32_t slot{};

    // Full FC peer identity; zero means the producer has no FC peer identity for this association.
    std::uint64_t peer_id{};
    // Direct protocol connection identity and accepted data sequence.
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    // QPC ticks on this process: successful recvfrom completion and native intake entry; zero means no timestamp.
    std::uint64_t received_qpc{};
    std::uint64_t injected_qpc{};
    // Borrows the copied first five native payload bytes for this call: the existing communication header.
    // payload_bytes is the copied prefix length, not the complete serialized payload length.
    const std::uint8_t *payload{};
    std::uint32_t payload_bytes{};
    // Opaque pool packet identity captured before intake owns/frees it; zero means rebuilding failed.
    std::uint32_t native_packet{};
    // 0 Built, 1 RejectedPayload, 2 FactoryUnavailable, 3 InvalidPrefix, 4 AllocationFailed, 5 NativeSourceUnavailable.
    std::uint32_t rebuild_result{};
};

struct DirectPumpObservation {
    std::uint64_t socket_id{};
    // Increases at pump entry, including nested pumps on this socket.
    std::uint32_t pump_id{};
    // QPC ticks on this process, independent of native game time.
    std::uint64_t started_qpc{};
    std::uint64_t finished_qpc{};
    // Gap between pump entries, captured before nested native intake can service the socket again.
    std::uint64_t service_gap{};
    std::uint64_t first_empty_qpc{};
    std::uint64_t last_empty_qpc{};
    std::uint64_t maximum_receive_duration{};
    std::uint32_t attempts{};
    std::uint32_t datagrams{};
    std::uint32_t oversized{};
    // Control or Data dispositions count as accepted; all other terminal dispositions count as rejected.
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    std::int32_t error{};
    std::uint32_t budget_reached{};
    // 0 empty socket, 1 attempt budget, 2 socket error.
    std::uint8_t terminal_reason{};
};

// Data means native intake was attempted; Control means authenticated, replay-admitted setup traffic.
enum class DirectDisposition : std::uint8_t {
    Arrived,
    Malformed,
    Association,
    Endpoint,
    State,
    Authentication,
    Replay,
    Control,
    Data,
    Rebuild,
};

// One raw socket arrival followed by its terminal disposition, joined by socket_id, pump_id and attempt.
struct DirectDatagramObservation {
    std::uint64_t socket_id{};
    std::uint32_t pump_id{};
    std::uint32_t attempt{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
    std::uint64_t peer_id{};
    std::int32_t slot{-1};
    // Untrusted header fields are available when the datagram contains the complete 24-byte header.
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    // IPv4 remains in Winsock network order; both ports use host order.
    std::uint32_t source_ipv4_network_order{};
    std::uint16_t source_port{};
    std::uint16_t bound_port{};
    std::uint32_t bytes{};
    DirectDisposition disposition{};
    std::uint8_t kind{};
    std::uint8_t header_available{};
    // Malformed: ParseStatus; Replay: ReplayResult; Rebuild: DirectReceive rebuild_result; otherwise zero.
    std::uint8_t detail{};
};

// Startup settings and errors plus effective buffers read once by the native owner for Full capture.
struct DirectSocketObservation {
    // Startup QPC tick identifies this socket lifetime within the process.
    std::uint64_t socket_id{};
    std::int32_t requested_send_bytes{};
    std::int32_t requested_receive_bytes{};
    // Actual sizes are meaningful only when the corresponding read_error is zero.
    std::int32_t actual_send_bytes{};
    std::int32_t actual_receive_bytes{};
    std::int32_t send_set_error{};
    std::int32_t receive_set_error{};
    std::int32_t send_read_error{};
    std::int32_t receive_read_error{};
    std::uint16_t bound_port{};
};

// Borrows native header bytes while the final Direct send remains active.
struct DirectSendObservation {
    std::int32_t slot{};
    std::uint64_t peer_id{};
    std::uint32_t connection_id{};
    std::uint32_t sequence{};
    const std::uint8_t *payload{};
    std::uint32_t payload_bytes{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
    std::uint32_t requested_bytes{};
    std::int32_t returned_bytes{};
    std::int32_t error{};
    // 0 untimed failure summary; 1 socket call, 2 encoding rejection, 3 preflight rejection, 4 socket validation rejection.
    // Sequence is reserved only for phases 1,2,4; phase 3 has no data sequence. Zero entered_qpc means no timed syscall.
    std::uint32_t phase{};
    // Preflight: 1 wrong thread, 2 identity, 3 missing association, 4 stale group; 0x100 plus bits 0 state/1 endpoint/2 socket.
    std::uint32_t rejection{};
    std::uint64_t socket_id{};
    // QPC entry to Direct serialization, including attempts rejected before sendto.
    std::uint64_t encoding_qpc{};
    std::uint32_t destination_ipv4_network_order{};
    std::uint16_t destination_port{};
    std::uint16_t bound_port{};
};

// Reports the existing recovery call's timing and selected group range, including empty calls.
struct RecoveryDrainObservation {
    std::uint32_t groups{};
    std::uint32_t oldest_turn{};
    std::uint32_t newest_turn{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};
};

// Exact FC send-window storage or cumulative retirement; phase 1 stores and phase 2 retires.
struct SendStorageObservation {
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

// CREATE fence opens, observes a changed map, or expires; native response details have a separate record.
struct CreateFenceObservation {
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

enum class DestinationGateReason : std::uint32_t {
    Open,
    CreateFence,
    NativeWindow,
    ExtendedWindow,
};

struct DestinationGateObservation {
    std::int32_t destination{-1};
    std::uint64_t peer_id{};
    DestinationGateReason reason{};
    // FC ring count; capacity is populated only when that ring determines the gate, otherwise zero.
    std::uint32_t outstanding{};
    std::uint32_t capacity{};
    std::uint32_t fence_map{};
    std::int32_t fence_turn{};
    // Native netTime seconds and RTT estimates; meaningful for the FC window or CREATE fence.
    float network_time{};
    float fence_sent_time{};
    float average_rtt{};
    float deviation_rtt{};
    std::uint32_t native_window_open{};
    std::uint32_t timed_out{};
};

// Reports transport and scheduling decisions synchronously on the originating native thread. Pointers are borrowed for the call.
// Copied callbacks remain valid while the provider is installed; callers must preserve ambient Win32 and Winsock errors.
struct NetworkDiagnosticsApi {
    static constexpr std::string_view id = "NetworkDiagnosticsApi.v5";

    // Returns the immutable capture tier: 0 Standard, 1 Combat, 2 Full. Consumers cache it at publication.
    fc::Function<std::uint32_t() noexcept> capture_mode;

    // Reports one send attempt and its selected carrier; size is the requested native payload length.
    // result is the value returned to the native caller; negative values count as failures, and zero can mean no send occurred.
    fc::Function<void(std::int32_t destination, std::uint32_t size, PacketCarrier carrier, std::int32_t result) noexcept> transport_send;

    // Reports the Direct subset of total native intake, including packets native intake rejects.
    // endpoint is borrowed only as a source pointer identity.
    fc::Function<void(void *endpoint) noexcept> direct_intake;

    // Copies one association transition; the observation remains borrowed for this call.
    fc::Function<void(const DirectAssociationObservation *observation) noexcept> direct_association;

    // Reports one authenticated, replay-admitted Data receive and its reconstruction/injection result.
    fc::Function<void(const DirectReceiveObservation *observation) noexcept> direct_receive;

    // Reports socket servicing, including empty pumps; the provider aggregates empty calls.
    fc::Function<void(const DirectPumpObservation *observation) noexcept> direct_pump;

    // Reports an arrival before validation and its disposition after handling; no payload or keys are borrowed.
    fc::Function<void(const DirectDatagramObservation *observation) noexcept> direct_datagram;
    // Reports requested and actual socket buffers, each setsockopt/getsockopt error, and the bound port.
    fc::Function<void(const DirectSocketObservation *observation) noexcept> direct_socket;

    // Reports the actual destination gate and its current window/fence values; the provider aggregates repeats.
    fc::Function<void(const DestinationGateObservation *observation) noexcept> destination_gate;

    // Reports actual sendto timing/results with matching connection, data and native fragment identities.
    fc::Function<void(const DirectSendObservation *observation) noexcept> direct_send;

    // Reports every recovery opportunity without adding another socket pump or parser call.
    fc::Function<void(const RecoveryDrainObservation *observation) noexcept> recovery_drain;

    // Copies the actual send-storage and CREATE lifecycle decisions made by the scheduling producer.
    fc::Function<void(const SendStorageObservation *observation) noexcept> send_storage;
    fc::Function<void(const CreateFenceObservation *observation) noexcept> create_transition;

    // Returns the current payload bit position for this packet; UINT32_MAX means no active matching stream.
    fc::Function<std::uint32_t(std::uint32_t packet, std::uint32_t encoding) noexcept> codec_position;

    // Supplies the active server policy's grace duration in simulation turns.
    fc::Function<void(std::uint32_t turns) noexcept> waitlate_grace;
};

static_assert(std::is_standard_layout_v<DirectAssociationObservation> && std::is_trivially_copyable_v<DirectAssociationObservation>);
static_assert(std::is_standard_layout_v<DirectReceiveObservation> && std::is_trivially_copyable_v<DirectReceiveObservation>);
static_assert(std::is_standard_layout_v<DirectPumpObservation> && std::is_trivially_copyable_v<DirectPumpObservation>);
static_assert(std::is_standard_layout_v<DirectDatagramObservation> && std::is_trivially_copyable_v<DirectDatagramObservation>);
static_assert(std::is_standard_layout_v<DirectSocketObservation> && std::is_trivially_copyable_v<DirectSocketObservation>);
static_assert(std::is_standard_layout_v<DestinationGateObservation> && std::is_trivially_copyable_v<DestinationGateObservation>);

// Describes the association transition record that producers borrow for a synchronous call.
inline fc::RecordDescription fc_description(std::type_identity<DirectAssociationObservation>) {
    return fc::describe<DirectAssociationObservation>(
        "DirectAssociationObservation.v1", &DirectAssociationObservation::slot, &DirectAssociationObservation::peer_id,
        &DirectAssociationObservation::connection_id, &DirectAssociationObservation::phase, &DirectAssociationObservation::route,
        &DirectAssociationObservation::route_reason, &DirectAssociationObservation::end_reason, &DirectAssociationObservation::attempts,
        &DirectAssociationObservation::tx_datagrams, &DirectAssociationObservation::rx_datagrams,
        &DirectAssociationObservation::send_failures, &DirectAssociationObservation::endpoint_rejects,
        &DirectAssociationObservation::authentication_rejects, &DirectAssociationObservation::replay_rejects,
        &DirectAssociationObservation::invalid_rejects, &DirectAssociationObservation::elapsed_ms,
        &DirectAssociationObservation::direct_ms);
}

// Describes the Direct data submission record that producers borrow for a synchronous call.
inline fc::RecordDescription fc_description(std::type_identity<DirectReceiveObservation>) {
    return fc::describe<DirectReceiveObservation>(
        "DirectReceiveObservation.v2", &DirectReceiveObservation::slot, &DirectReceiveObservation::peer_id,
        &DirectReceiveObservation::connection_id, &DirectReceiveObservation::sequence, &DirectReceiveObservation::received_qpc,
        &DirectReceiveObservation::injected_qpc, &DirectReceiveObservation::payload, &DirectReceiveObservation::payload_bytes,
        &DirectReceiveObservation::native_packet, &DirectReceiveObservation::rebuild_result);
}

// Describes the socket-service aggregate borrowed by producers.
inline fc::RecordDescription fc_description(std::type_identity<DirectPumpObservation>) {
    return fc::describe<DirectPumpObservation>(
        "DirectPumpObservation.v2", &DirectPumpObservation::socket_id, &DirectPumpObservation::pump_id, &DirectPumpObservation::started_qpc,
        &DirectPumpObservation::finished_qpc, &DirectPumpObservation::service_gap, &DirectPumpObservation::attempts,
        &DirectPumpObservation::datagrams, &DirectPumpObservation::oversized, &DirectPumpObservation::error,
        &DirectPumpObservation::budget_reached, &DirectPumpObservation::first_empty_qpc, &DirectPumpObservation::last_empty_qpc,
        &DirectPumpObservation::maximum_receive_duration, &DirectPumpObservation::accepted, &DirectPumpObservation::rejected,
        &DirectPumpObservation::terminal_reason);
}

// Describes the metadata copied at arrival and after the same datagram is handled.
inline fc::RecordDescription fc_description(std::type_identity<DirectDatagramObservation>) {
    return fc::describe<DirectDatagramObservation>(
        "DirectDatagramObservation.v1", &DirectDatagramObservation::socket_id, &DirectDatagramObservation::pump_id,
        &DirectDatagramObservation::attempt, &DirectDatagramObservation::entered_qpc, &DirectDatagramObservation::returned_qpc,
        &DirectDatagramObservation::peer_id, &DirectDatagramObservation::slot, &DirectDatagramObservation::connection_id,
        &DirectDatagramObservation::sequence, &DirectDatagramObservation::source_ipv4_network_order,
        &DirectDatagramObservation::source_port, &DirectDatagramObservation::bound_port, &DirectDatagramObservation::bytes,
        &DirectDatagramObservation::disposition, &DirectDatagramObservation::kind, &DirectDatagramObservation::header_available,
        &DirectDatagramObservation::detail);
}

// Describes the cached socket configuration and independent option errors.
inline fc::RecordDescription fc_description(std::type_identity<DirectSocketObservation>) {
    return fc::describe<DirectSocketObservation>(
        "DirectSocketObservation.v1", &DirectSocketObservation::socket_id, &DirectSocketObservation::requested_send_bytes,
        &DirectSocketObservation::requested_receive_bytes, &DirectSocketObservation::actual_send_bytes,
        &DirectSocketObservation::actual_receive_bytes, &DirectSocketObservation::send_set_error,
        &DirectSocketObservation::receive_set_error, &DirectSocketObservation::send_read_error,
        &DirectSocketObservation::receive_read_error, &DirectSocketObservation::bound_port);
}

// Describes the actual scheduling decision and the values used by its gate.
inline fc::RecordDescription fc_description(std::type_identity<DestinationGateObservation>) {
    return fc::describe<DestinationGateObservation>(
        "DestinationGateObservation.v1", &DestinationGateObservation::destination, &DestinationGateObservation::peer_id,
        &DestinationGateObservation::reason, &DestinationGateObservation::outstanding, &DestinationGateObservation::capacity,
        &DestinationGateObservation::fence_map, &DestinationGateObservation::fence_turn, &DestinationGateObservation::network_time,
        &DestinationGateObservation::fence_sent_time, &DestinationGateObservation::average_rtt, &DestinationGateObservation::deviation_rtt,
        &DestinationGateObservation::native_window_open, &DestinationGateObservation::timed_out);
}

// Describes the borrowed final-send observation.
inline fc::RecordDescription fc_description(std::type_identity<DirectSendObservation>) {
    return fc::describe<DirectSendObservation>(
        "DirectSendObservation.v2", &DirectSendObservation::slot, &DirectSendObservation::peer_id, &DirectSendObservation::connection_id,
        &DirectSendObservation::sequence, &DirectSendObservation::payload, &DirectSendObservation::payload_bytes,
        &DirectSendObservation::entered_qpc, &DirectSendObservation::returned_qpc, &DirectSendObservation::requested_bytes,
        &DirectSendObservation::returned_bytes, &DirectSendObservation::error, &DirectSendObservation::phase,
        &DirectSendObservation::rejection, &DirectSendObservation::socket_id, &DirectSendObservation::encoding_qpc,
        &DirectSendObservation::destination_ipv4_network_order, &DirectSendObservation::destination_port,
        &DirectSendObservation::bound_port);
}

// Describes the borrowed recovery opportunity.
inline fc::RecordDescription fc_description(std::type_identity<RecoveryDrainObservation>) {
    return fc::describe<RecoveryDrainObservation>("RecoveryDrainObservation.v1", &RecoveryDrainObservation::groups,
                                                  &RecoveryDrainObservation::oldest_turn, &RecoveryDrainObservation::newest_turn,
                                                  &RecoveryDrainObservation::entered_qpc, &RecoveryDrainObservation::returned_qpc);
}

// Describes the producer's send-storage decision and before/after counts.
inline fc::RecordDescription fc_description(std::type_identity<SendStorageObservation>) {
    return fc::describe<SendStorageObservation>(
        "SendStorageObservation.v1", &SendStorageObservation::destination, &SendStorageObservation::turn,
        &SendStorageObservation::native_slot, &SendStorageObservation::phase, &SendStorageObservation::peer_id,
        &SendStorageObservation::network_time, &SendStorageObservation::before_count, &SendStorageObservation::after_count,
        &SendStorageObservation::extended_recorded, &SendStorageObservation::native_write_skipped);
}

// Describes the CREATE fence lifecycle decision and native timing operands.
inline fc::RecordDescription fc_description(std::type_identity<CreateFenceObservation>) {
    return fc::describe<CreateFenceObservation>(
        "CreateFenceObservation.v1", &CreateFenceObservation::destination, &CreateFenceObservation::phase,
        &CreateFenceObservation::map_before, &CreateFenceObservation::map_after, &CreateFenceObservation::turn_before,
        &CreateFenceObservation::turn_after, &CreateFenceObservation::sent_time, &CreateFenceObservation::network_time,
        &CreateFenceObservation::average_rtt, &CreateFenceObservation::deviation_rtt);
}

// Describes all required synchronous methods for exact runtime compatibility checks.
inline fc::ApiContract<NetworkDiagnosticsApi> api_contract() {
    return fc::api<NetworkDiagnosticsApi>(
        NetworkDiagnosticsApi::id, &NetworkDiagnosticsApi::capture_mode, &NetworkDiagnosticsApi::transport_send,
        &NetworkDiagnosticsApi::direct_intake, &NetworkDiagnosticsApi::direct_association, &NetworkDiagnosticsApi::direct_receive,
        &NetworkDiagnosticsApi::direct_pump, &NetworkDiagnosticsApi::direct_datagram, &NetworkDiagnosticsApi::direct_socket,
        &NetworkDiagnosticsApi::destination_gate, &NetworkDiagnosticsApi::direct_send, &NetworkDiagnosticsApi::recovery_drain,
        &NetworkDiagnosticsApi::send_storage, &NetworkDiagnosticsApi::create_transition, &NetworkDiagnosticsApi::codec_position,
        &NetworkDiagnosticsApi::waitlate_grace);
}

} // namespace fc_plugins::network_diagnostics
