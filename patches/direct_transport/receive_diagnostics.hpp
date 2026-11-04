#pragma once

#include "ambient_errors.hpp"
#include "socket.hpp"

#include <algorithm>
#include <cstdint>

namespace fc_plugins::direct_transport {

// Samples this process's QPC clock without changing either ambient OS error.
inline std::uint64_t receive_stamp() noexcept {
    const auto errors = AmbientErrors::capture();
    LARGE_INTEGER value{};
    const auto valid = QueryPerformanceCounter(&value);
    errors.restore();
    return valid ? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

// Collects syscall work and empty returns without emitting an event for each empty poll.
inline void observe_receive_attempt(network_diagnostics::DirectPumpObservation &pump, const SocketResult &received) noexcept {
    ++pump.attempts;
    pump.datagrams += received.succeeded() ? 1U : 0U;
    if (received.returned_qpc >= received.entered_qpc && received.entered_qpc != 0) {
        pump.maximum_receive_duration = (std::max)(pump.maximum_receive_duration, received.returned_qpc - received.entered_qpc);
    }
    if (!received.succeeded() && received.error == kSocketWouldBlock) {
        if (pump.first_empty_qpc == 0) {
            pump.first_empty_qpc = received.returned_qpc;
        }
        pump.last_empty_qpc = received.returned_qpc;
    }
}

// Copies socket metadata and available identity fields; copied header fields are unauthenticated wire values.
inline network_diagnostics::DirectDatagramObservation datagram_observation(const network_diagnostics::DirectPumpObservation &pump,
                                                                           const SocketResult &received, const Endpoint &source,
                                                                           std::uint16_t bound_port,
                                                                           std::span<const std::uint8_t> bytes) noexcept {
    network_diagnostics::DirectDatagramObservation observation{
        .socket_id = pump.socket_id,
        .pump_id = pump.pump_id,
        .attempt = pump.attempts,
        .entered_qpc = received.entered_qpc,
        .returned_qpc = received.returned_qpc,
        .source_ipv4_network_order = source.ipv4_network_order,
        .source_port = source.port_host_order,
        .bound_port = bound_port,
        .bytes = static_cast<std::uint32_t>(bytes.size()),
    };
    if (bytes.size() >= kDirectHeaderBytes) {
        observation.connection_id = load_big_endian32(bytes.data() + offsetof(DirectHeaderV1, connection_id));
        observation.sequence = load_big_endian32(bytes.data() + offsetof(DirectHeaderV1, datagram_sequence));
        observation.kind = bytes[offsetof(DirectHeaderV1, kind)];
        observation.header_available = 1;
    }
    return observation;
}

// Reports a metadata snapshot synchronously while preserving the originating socket's ambient errors.
inline void report_datagram(const network_diagnostics::NetworkDiagnosticsApi &output,
                            const network_diagnostics::DirectDatagramObservation &observation) noexcept {
    const auto errors = AmbientErrors::capture();
    output.direct_datagram(&observation);
    errors.restore();
}

// Sets a terminal result only when diagnostics requested a snapshot for this datagram.
inline void set_disposition(network_diagnostics::DirectDatagramObservation *observation, network_diagnostics::DirectDisposition disposition,
                            std::uint8_t detail = 0) noexcept {
    if (observation != nullptr) {
        observation->disposition = disposition;
        observation->detail = detail;
    }
}

} // namespace fc_plugins::direct_transport
