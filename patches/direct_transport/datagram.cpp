#include "socket.hpp"
#include "receive_diagnostics.hpp"

#include <WinSock2.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace fc_plugins::direct_transport {

std::size_t write_data_datagram(std::span<std::uint8_t> output, Direction direction, const SessionKey &key, std::uint32_t connection_id,
                                std::uint32_t sequence, std::span<const std::uint8_t> payload) noexcept {
    const auto datagram_bytes = kDirectHeaderBytes + payload.size();
    if (payload.size() < kNativeHeaderBytes || payload.size() > kMaximumNativeBytes || output.size() < datagram_bytes) {
        return 0;
    }

    const auto datagram = output.first(datagram_bytes);
    write_direct_header(datagram, DirectKind::Data, connection_id, sequence, 0);
    std::memcpy(datagram.data() + kDirectHeaderBytes, payload.data(), payload.size());

    // The tag covers the serialized header with a zero tag field and the exact native payload bytes.
    const auto tag = compute_authentication_tag(direction, key, datagram.first(kDirectHeaderBytes), datagram.subspan(kDirectHeaderBytes));
    store_big_endian64(datagram.data() + offsetof(DirectHeaderV1, authentication_tag), tag);
    return datagram_bytes;
}

bool write_probe_datagram(std::span<std::uint8_t> output, DirectKind kind, Direction direction, const SessionKey &key,
                          std::uint32_t connection_id, std::uint32_t sequence) noexcept {
    if ((kind != DirectKind::Probe && kind != DirectKind::ProbeAck) || output.size() < kProbeDatagramBytes) {
        return false;
    }

    const auto datagram = output.first(kProbeDatagramBytes);
    // Zero padding makes the proof as large as any supported Data datagram.
    std::ranges::fill(datagram, std::uint8_t{});
    write_direct_header(datagram, kind, connection_id, sequence, 0);
    const auto tag = compute_authentication_tag(direction, key, datagram.first(kDirectHeaderBytes), datagram.subspan(kDirectHeaderBytes));
    store_big_endian64(datagram.data() + offsetof(DirectHeaderV1, authentication_tag), tag);
    return true;
}

NativeTransmitResult send_direct_data(UdpSocket &socket, std::span<std::uint8_t> output, Direction direction, const SessionKey &key,
                                      std::uint32_t connection_id, std::uint32_t sequence, const Endpoint &endpoint,
                                      std::span<const std::uint8_t> payload,
                                      network_diagnostics::DirectSendObservation *observation) noexcept {
    if (observation != nullptr) {
        observation->encoding_qpc = receive_stamp();
        observation->socket_id = socket.configuration().socket_id;
        observation->destination_ipv4_network_order = endpoint.ipv4_network_order;
        observation->destination_port = endpoint.port_host_order;
        observation->bound_port = socket.bound_port();
    }

    // Keep failures handled so the hook returns the error without retrying this payload through Galaxy.
    NativeTransmitResult result{.handled = true};
    const auto bytes = write_data_datagram(output, direction, key, connection_id, sequence, payload);
    if (bytes == 0) {
        result.result = -1;
        result.error = WSAEMSGSIZE;
        if (observation != nullptr) {
            observation->requested_bytes = static_cast<std::uint32_t>(kDirectHeaderBytes + payload.size());
            observation->returned_bytes = -1;
            observation->error = WSAEMSGSIZE;
            observation->phase = 2;
        }
        return result;
    }

    // A datagram must be sent in full; expose only the native payload length to the game's caller.
    const auto sent = socket.send(output.first(bytes), endpoint, observation != nullptr);
    if (observation != nullptr) {
        observation->phase = sent.entered_qpc != 0 ? 1U : 4U;
        observation->entered_qpc = sent.entered_qpc;
        observation->returned_qpc = sent.returned_qpc;
        observation->requested_bytes = static_cast<std::uint32_t>(bytes);
        observation->returned_bytes = sent.value;
        observation->error = sent.error;
    }
    if (!sent.succeeded()) {
        result.result = -1;
        result.error = sent.error;
    } else if (sent.value != static_cast<std::int32_t>(bytes)) {
        result.result = -1;
        result.error = WSAEMSGSIZE;
    } else {
        result.result = static_cast<std::int32_t>(payload.size());
    }
    return result;
}

} // namespace fc_plugins::direct_transport
