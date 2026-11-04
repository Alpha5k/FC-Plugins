#include "handler.hpp"

#include "../receive_diagnostics.hpp"

#include <algorithm>

#include <Windows.h>

namespace fc_plugins::direct_transport::client {
namespace direct = direct_transport;

namespace {

constexpr std::uint8_t kHostSlot = 64;

// Maps transport state to the public diagnostic route, combining the two negotiation phases.
network_diagnostics::DirectRoute observed_route(direct::RouteState state) noexcept {
    switch (state) {
    case direct::RouteState::Unclassified:
        return network_diagnostics::DirectRoute::Unclassified;
    case direct::RouteState::Negotiating:
    case direct::RouteState::AwaitCommit:
        return network_diagnostics::DirectRoute::Negotiating;
    case direct::RouteState::GalaxyLocked:
        return network_diagnostics::DirectRoute::Native;
    case direct::RouteState::DirectLocked:
        return network_diagnostics::DirectRoute::Direct;
    }
    return network_diagnostics::DirectRoute::Unclassified;
}

} // namespace

void Handler::peer_available(direct_transport::Peer peer) noexcept {
    if (peer.slot != kHostSlot) {
        return;
    }
    if (current_peer(peer)) {
        return;
    }

    end_association(direct::AssociationEndReason::Replaced);
    start_association(peer, GetTickCount64());
}

void Handler::peer_unavailable(direct_transport::Peer peer) noexcept {
    if (current_peer(peer) && network_thread_.current()) {
        end_association(direct::AssociationEndReason::Disconnected);
    }
}

bool Handler::current_peer(direct_transport::Peer peer) const noexcept {
    return association_.live && association_.peer.slot == peer.slot && association_.peer.id == peer.id &&
           association_.peer.galaxy_id == peer.galaxy_id;
}

void Handler::start_association(direct_transport::Peer peer, std::uint64_t now) noexcept {
    std::uint64_t lobby_id{};
    if (!direct::read_native_identity(host_endpoint_, lobby_id)) {
        return;
    }

    // Replacement clears keys, sequences, replay history and counters for the new FC connection.
    Association next{};
    next.live = true;
    next.peer = peer;
    next.lobby_id = lobby_id;
    next.generation = ++next_generation_;
    next.phase_start_ms = now;
    next.stats.started_ms = now;
    association_ = next;
    observe_association(DirectAssociationPhase::Started, now, direct::AssociationEndReason::Disconnected);

    if (!socket_.available()) {
        set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, false, direct::RouteReason::SocketUnavailable);
    } else {
        publish_status();
    }
}

void Handler::end_association(direct::AssociationEndReason reason) noexcept {
    if (!association_.live) {
        return;
    }

    observe_association(DirectAssociationPhase::Ended, GetTickCount64(), reason);
    association_ = {};
    publish_status();
}

void Handler::observe_association(DirectAssociationPhase phase, std::uint64_t now, direct::AssociationEndReason end_reason,
                                  std::uint32_t attempts) noexcept {
    const auto *output = diagnostics();
    if (output == nullptr) {
        return;
    }

    // Durations use the monotonic Windows millisecond clock, independently of game time.
    const auto elapsed = now >= association_.stats.started_ms ? now - association_.stats.started_ms : 0;
    const auto direct_ms = association_.stats.direct_started_ms != 0 && now >= association_.stats.direct_started_ms
                               ? now - association_.stats.direct_started_ms
                               : 0;
    const network_diagnostics::DirectAssociationObservation observation{
        .slot = association_.peer.slot,
        .peer_id = association_.peer.id.value,
        .connection_id = association_.connection_id,
        .phase = phase,
        .route = observed_route(association_.state),
        .route_reason = static_cast<std::uint8_t>(association_.stats.route_reason),
        .end_reason = phase == DirectAssociationPhase::Ended ? static_cast<std::uint8_t>(end_reason) : std::uint8_t{},
        .attempts = attempts,
        .tx_datagrams = association_.stats.tx_datagrams,
        .rx_datagrams = association_.stats.rx_datagrams,
        .send_failures = association_.stats.send_failures,
        .endpoint_rejects = association_.stats.endpoint_rejects,
        .authentication_rejects = association_.stats.authentication_rejects,
        .replay_rejects = association_.stats.replay_rejects,
        .invalid_rejects = association_.stats.invalid_rejects,
        .elapsed_ms = elapsed,
        .direct_ms = direct_ms,
    };
    output->direct_association(&observation);
}

void Handler::pump_direct(std::uint64_t now) noexcept {
    if (!socket_.available()) {
        return;
    }

    const auto *output = diagnostics();
    const auto detailed = output != nullptr && diagnostics_full_;
    network_diagnostics::DirectPumpObservation observation{};
    network_diagnostics::DirectDatagramObservation datagram{};
    if (output != nullptr) {
        if (detailed && !socket_observed_) {
            socket_observed_ = true;
            const auto errors = direct::AmbientErrors::capture();
            output->direct_socket(&socket_.capture_configuration());
            errors.restore();
        }
        observation.socket_id = socket_.configuration().socket_id;
        observation.pump_id = ++direct_pump_id_;
        observation.started_qpc = direct::receive_stamp();
        observation.service_gap = last_direct_service_qpc_ == 0 || observation.started_qpc < last_direct_service_qpc_
                                      ? 0
                                      : observation.started_qpc - last_direct_service_qpc_;
        last_direct_service_qpc_ = observation.started_qpc;
    }
    // Capture arrival before validation; nested intake uses another invocation's metadata and pump identity.
    const auto drained = direct::drain_datagrams(
        [this, output, detailed, &observation, &datagram](direct::Endpoint &source) noexcept {
            const auto result = socket_.receive(receive_buffer_, source, detailed);
            if (output != nullptr) {
                direct::observe_receive_attempt(observation, result);
                if (result.succeeded()) {
                    if (detailed) {
                        datagram = direct::datagram_observation(observation, result, source, socket_.bound_port(),
                                                                std::span<const std::uint8_t>(receive_buffer_).first(result.value));
                        direct::report_datagram(*output, datagram);
                    } else {
                        datagram = {.socket_id = observation.socket_id,
                                    .pump_id = observation.pump_id,
                                    .attempt = observation.attempts,
                                    .source_ipv4_network_order = source.ipv4_network_order,
                                    .source_port = source.port_host_order,
                                    .bound_port = socket_.bound_port(),
                                    .bytes = static_cast<std::uint32_t>(result.value)};
                    }
                }
            }
            return result;
        },
        [this, output, detailed, &observation, &datagram, now](std::int32_t bytes, const direct::Endpoint &source) noexcept {
            handle_direct(std::span<const std::uint8_t>(receive_buffer_).first(bytes), source, now,
                          output != nullptr ? &datagram : nullptr);
            if (output != nullptr) {
                if (detailed || (datagram.disposition != network_diagnostics::DirectDisposition::Data &&
                                 datagram.disposition != network_diagnostics::DirectDisposition::Control)) {
                    direct::report_datagram(*output, datagram);
                }
                const auto accepted = datagram.disposition == network_diagnostics::DirectDisposition::Data ||
                                      datagram.disposition == network_diagnostics::DirectDisposition::Control;
                observation.accepted += accepted ? 1U : 0U;
                observation.rejected += accepted ? 0U : 1U;
            }
        });
    record_drain(drained);
    if (output != nullptr) {
        observation.finished_qpc = direct::receive_stamp();
        observation.oversized = drained.discarded;
        observation.error = drained.error;
        observation.budget_reached = drained.limit_reached;
        observation.terminal_reason = static_cast<std::uint8_t>(drained.reason);
        const auto errors = direct::AmbientErrors::capture();
        output->direct_pump(&observation);
        errors.restore();
    }
}

void Handler::handle_control(const direct_transport::ParsedControl &control, std::uint64_t now) noexcept {
    // A matching Commit is harmless after Direct selection or an authenticated Commit timeout.
    if (control.kind == direct::ControlKind::Commit) {
        if (!direct::valid_commit(association_.state, association_.connection_id, control.connection_id) &&
            !(association_.state == direct::RouteState::GalaxyLocked && association_.receive_permission &&
              association_.connection_id != 0 && control.connection_id == association_.connection_id)) {
            association_.stats.reject(direct::RejectionKind::Invalid);
            return;
        }
        if (association_.state == direct::RouteState::AwaitCommit) {
            set_route(direct::RouteState::DirectLocked, direct::TransmitRoute::DirectPending, association_.receive_permission,
                      direct::RouteReason::Commit);
        }
        return;
    }

    if (control.kind != direct::ControlKind::Offer || association_.state != direct::RouteState::Unclassified ||
        control.connection_id == 0 || control.server_port == 0 || !direct::is_public_ipv4(control.server_ipv4_network_order)) {
        association_.stats.reject(direct::RejectionKind::Invalid);
        return;
    }

    // The server provides the key and endpoint. Output stays on Galaxy while the UDP proof runs.
    association_.connection_id = control.connection_id;
    association_.session_key = control.session_key;
    association_.server_endpoint = {control.server_ipv4_network_order, control.server_port};
    association_.proof_start_ms = now;
    association_.probe_attempts = 0;
    set_route(direct::RouteState::Negotiating, association_.transmit_route, false);
}

void Handler::handle_direct(std::span<const std::uint8_t> bytes, const direct::Endpoint &source, std::uint64_t now,
                            network_diagnostics::DirectDatagramObservation *datagram) noexcept {
    using network_diagnostics::DirectDisposition;
    direct::ParsedDirect packet{};
    const auto parsed = direct::parse_direct(bytes, packet);
    if (parsed != direct::ParseStatus::Ok) {
        direct::set_disposition(datagram, DirectDisposition::Malformed, static_cast<std::uint8_t>(parsed));
        return;
    }
    if (datagram != nullptr) {
        datagram->connection_id = packet.connection_id;
        datagram->sequence = packet.datagram_sequence;
        datagram->kind = static_cast<std::uint8_t>(packet.kind);
        datagram->header_available = 1;
    }
    if (!association_.live || packet.connection_id != association_.connection_id) {
        direct::set_disposition(datagram, DirectDisposition::Association);
        return;
    }
    if (datagram != nullptr) {
        datagram->peer_id = association_.peer.id.value;
        datagram->slot = association_.peer.slot;
    }

    if (source != association_.server_endpoint) {
        direct::set_disposition(datagram, DirectDisposition::Endpoint);
        association_.stats.reject(direct::RejectionKind::Endpoint);
        return;
    }

    // A ProbeAck is legal during setup. Data needs completed UDP proof, which survives Commit
    // timeout while outgoing traffic stays on Galaxy. The offered address and port remain fixed.
    const auto valid_state =
        packet.kind == direct::DirectKind::ProbeAck
            ? association_.state == direct::RouteState::Negotiating || association_.state == direct::RouteState::AwaitCommit
            : packet.kind == direct::DirectKind::Data && association_.receive_permission &&
                  (association_.state == direct::RouteState::AwaitCommit || association_.state == direct::RouteState::DirectLocked ||
                   association_.state == direct::RouteState::GalaxyLocked);
    if (!valid_state) {
        direct::set_disposition(datagram, DirectDisposition::State);
        association_.stats.reject(direct::RejectionKind::Invalid);
        return;
    }

    // Authenticate before changing replay history, so a forged sequence cannot discard real traffic.
    if (!direct::verify_authentication_tag(direct::Direction::ServerToClient, association_.session_key,
                                           bytes.first(direct::kDirectHeaderBytes), packet.payload)) {
        direct::set_disposition(datagram, DirectDisposition::Authentication);
        association_.stats.reject(direct::RejectionKind::Authentication);
        return;
    }

    const auto replay = association_.receive_replay.admit(packet.datagram_sequence);
    if (replay == direct::ReplayResult::Duplicate || replay == direct::ReplayResult::Stale || replay == direct::ReplayResult::InvalidJump) {
        direct::set_disposition(datagram, DirectDisposition::Replay, static_cast<std::uint8_t>(replay));
        association_.stats.reject(direct::RejectionKind::Replay);
        return;
    }

    // Each fresh Ack received while Negotiating restarts the Ready timer and its attempt count.
    direct::set_disposition(datagram, DirectDisposition::Control);
    if (packet.kind == direct::DirectKind::ProbeAck && association_.state == direct::RouteState::Negotiating) {
        association_.phase_start_ms = now;
        association_.receive_permission = true;
        association_.ready_attempts = 0;
        return;
    }
    if (packet.kind != direct::DirectKind::Data) {
        return;
    }
    direct::set_disposition(datagram, DirectDisposition::Data);

    // Authenticated server Data can stand in for Commit while the client is still waiting for it.
    if (association_.state == direct::RouteState::AwaitCommit) {
        set_route(direct::RouteState::DirectLocked, direct::TransmitRoute::DirectPending, true, direct::RouteReason::ImplicitCommit);
    }

    const auto *output = diagnostics();
    // Copy the existing communication header and association identity before rebuilding and transferring the native packet.
    std::array<std::uint8_t, 5> header{};
    network_diagnostics::DirectReceiveObservation observation{};
    if (output != nullptr) {
        if (diagnostics_full_) {
            std::copy_n(packet.payload.data(), header.size(), header.data());
        }
        observation = {
            .slot = association_.peer.slot,
            .peer_id = association_.peer.id.value,
            .connection_id = association_.connection_id,
            .sequence = packet.datagram_sequence,
            .received_qpc = datagram != nullptr ? datagram->returned_qpc : 0,
            .payload = diagnostics_full_ ? header.data() : nullptr,
            .payload_bytes = diagnostics_full_ ? static_cast<std::uint32_t>(header.size()) : 0,
            .rebuild_result = 5, // The native source address has not yet been validated.
        };
    }
    direct::NativeAddress address{};
    if (!direct::read_native_address(host_endpoint_, address) || address[0] != association_.lobby_id) {
        direct::set_disposition(datagram, DirectDisposition::Rebuild, 5);
        if (output != nullptr) {
            const auto errors = direct::AmbientErrors::capture();
            output->direct_receive(&observation);
            errors.restore();
        }
        return;
    }

    // Preserve the source before allocation can flush native queues. A fresh sequence stays consumed on failure;
    // the counter records submission rather than gameplay acceptance.
    void *native_packet{};
    const auto built = direct::build_native_packet(packet_factory_, packet.payload, native_packet);
    if (built != direct::NativePacketResult::Built) {
        direct::set_disposition(datagram, DirectDisposition::Rebuild, static_cast<std::uint8_t>(built));
    }
    if (built == direct::NativePacketResult::Built) {
        if (output != nullptr) {
            observation.native_packet = reinterpret_cast<std::uint32_t>(native_packet);
            observation.injected_qpc = diagnostics_full_ ? direct::receive_stamp() : 0;
        }
        submit_native(native_packet, address);
        ++association_.stats.rx_datagrams;
    } else if (built == direct::NativePacketResult::RejectedPayload) {
        association_.stats.reject(direct::RejectionKind::Invalid);
    } else if (built == direct::NativePacketResult::InvalidPrefix) {
        latest_prefix_failure_peer_.store(association_.peer.galaxy_id, std::memory_order_relaxed);
        packet_prefix_failures_.fetch_add(1, std::memory_order_release);
    } else if (built == direct::NativePacketResult::AllocationFailed) {
        latest_allocation_failure_peer_.store(association_.peer.galaxy_id, std::memory_order_relaxed);
        packet_allocation_failures_.fetch_add(1, std::memory_order_release);
    }
    if (output != nullptr) {
        observation.rebuild_result = static_cast<std::uint32_t>(built);
        const auto errors = direct::AmbientErrors::capture();
        output->direct_receive(&observation);
        errors.restore();
    }
}

bool Handler::send_control(std::span<const std::uint8_t> bytes) noexcept {
    return feature_.send(association_.peer.id, std::as_bytes(bytes)) == fc::SendResult::Accepted;
}

void Handler::send_probe() noexcept {
    std::array<std::uint8_t, direct::kProbeDatagramBytes> datagram;
    if (!direct::write_probe_datagram(datagram, direct::DirectKind::Probe, direct::Direction::ClientToServer, association_.session_key,
                                      association_.connection_id, association_.send_sequence++)) {
        return;
    }

    // Socket failure consumes the attempt and the sequence; proof retries send a new datagram.
    if (!socket_.send(datagram, association_.server_endpoint).succeeded()) {
        ++association_.stats.send_failures;
    }
    ++association_.probe_attempts;
}

void Handler::service_association(std::uint64_t now) noexcept {
    if (!association_.live) {
        return;
    }

    // Deadlines are checked only when DoRecvs is serviced. A local timeout chooses Galaxy even
    // though a delayed Ready or Commit may let the server independently choose Direct.
    if (association_.state == direct::RouteState::Unclassified) {
        if (now - association_.phase_start_ms >= direct::kHandshakeDeadlineMs) {
            set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, false, direct::RouteReason::OfferTimeout);
        }
        return;
    }
    if (association_.state == direct::RouteState::AwaitCommit) {
        if (now - association_.phase_start_ms >= direct::kHandshakeDeadlineMs) {
            // The server may already send Direct after Ready; retain its authenticated receive permission.
            set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, association_.receive_permission,
                      direct::RouteReason::CommitTimeout);
        }
        return;
    }
    if (association_.state != direct::RouteState::Negotiating) {
        return;
    }

    // Before proof, try at most three full-size Probes at elapsed thresholds of zero, one and two seconds.
    // Delayed receive service can make successive attempts occur less than one second apart.
    if (!association_.receive_permission) {
        if (now - association_.proof_start_ms >= direct::kHandshakeDeadlineMs) {
            set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, false, direct::RouteReason::ProofTimeout);
        } else if (association_.probe_attempts < direct::kHandshakeAttemptLimit &&
                   now - association_.proof_start_ms >= association_.probe_attempts * direct::kHandshakeRetryIntervalMs) {
            send_probe();
        }
        return;
    }

    // After proof, retry Ready at the same elapsed thresholds. Accepted means FC copied the message
    // to its outgoing queue; the client then waits separately for Commit or Data.
    if (now - association_.phase_start_ms >= direct::kHandshakeDeadlineMs) {
        set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, false, direct::RouteReason::ReadyTimeout);
        return;
    }
    if (association_.ready_attempts < direct::kHandshakeAttemptLimit &&
        now - association_.phase_start_ms >= association_.ready_attempts * direct::kHandshakeRetryIntervalMs) {
        std::array<std::uint8_t, direct::kActivationBytes> ready{};
        direct::write_activation(ready, direct::ControlKind::Ready, association_.connection_id);
        ++association_.ready_attempts;
        if (send_control(ready)) {
            association_.phase_start_ms = now;
            set_route(direct::RouteState::AwaitCommit, association_.transmit_route, true);
        }
    }
}

void Handler::set_route(direct::RouteState state, direct::TransmitRoute route, bool receive_permission,
                        direct::RouteReason reason) noexcept {
    if (!direct::valid_route_transition(association_.state, state)) {
        return;
    }

    const auto state_changed = association_.state != state;
    const auto terminal = state_changed && (state == direct::RouteState::GalaxyLocked || state == direct::RouteState::DirectLocked);
    if (terminal && reason == direct::RouteReason::None) {
        return;
    }

    // DirectPending leaves an active packet group's transport unchanged; send applies it later.
    association_.state = state;
    association_.transmit_route = route;
    association_.receive_permission = receive_permission;

    if (terminal) {
        association_.stats.route_reason = reason;
        if (state == direct::RouteState::DirectLocked) {
            association_.stats.direct_started_ms = GetTickCount64();
        }
        log_route(state, reason);
        observe_association(DirectAssociationPhase::Terminal, GetTickCount64(), direct::AssociationEndReason::Disconnected,
                            association_.probe_attempts + association_.ready_attempts);
    }
    if (state_changed) {
        publish_status();
    }
}

} // namespace fc_plugins::direct_transport::client
