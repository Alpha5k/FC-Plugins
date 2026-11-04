#include "handler.hpp"

#include "../receive_diagnostics.hpp"

#include <algorithm>

#include <Windows.h>

namespace fc_plugins::direct_transport::server {
namespace {

constexpr std::uint64_t kOfferSafetyTimeoutMs = 10'000;

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
    if (peer.slot >= associations_.size()) {
        return;
    }

    auto &removal = removals_[peer.slot];
    if (current_peer(peer)) {
        return;
    }
    if (current_removal(removal) && removal.peer.galaxy_id == peer.galaxy_id &&
        (removal.peer.id.value == 0 || removal.peer.id == peer.id)) {
        return;
    }

    removal = {};
    end_association(peer.slot, direct::AssociationEndReason::Replaced);
    start_association(peer, true, GetTickCount64());
}

void Handler::peer_unavailable(direct_transport::Peer peer) noexcept {
    if (peer.slot < removals_.size() && removals_[peer.slot].peer.id == peer.id && network_thread_.current()) {
        removals_[peer.slot] = {};
    }
    if (current_peer(peer) && network_thread_.current()) {
        end_association(peer.slot, direct::AssociationEndReason::Disconnected);
    }
}

bool Handler::current_peer(direct_transport::Peer peer) const noexcept {
    if (peer.slot >= associations_.size()) {
        return false;
    }

    const auto &association = associations_[peer.slot];
    return association.live && association.compatible && association.peer.galaxy_id == peer.galaxy_id && association.peer.id == peer.id;
}

void Handler::scan_players(std::uint64_t now) noexcept {
    if (direct::policy_action(settings_.policy, false) != direct::PolicyAction::Remove) {
        return;
    }

    // RequireDirectAll finds unsupported Galaxy users through connected physical slots 0–63.
    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        std::uint64_t identity{};
        if (!read_identity(slot, identity)) {
            removals_[slot] = {};
            end_association(slot, direct::AssociationEndReason::Disconnected);
            continue;
        }
        if (current_removal(removals_[slot])) {
            continue;
        }
        removals_[slot] = {};
        if (associations_[slot].live && associations_[slot].peer.galaxy_id != identity) {
            end_association(slot, direct::AssociationEndReason::Replaced);
        }
        if (!associations_[slot].live) {
            start_association({.galaxy_id = identity, .slot = slot}, false, now);
        }
    }
}

void Handler::start_association(direct_transport::Peer peer, bool compatible, std::uint64_t now) noexcept {
    // Association replacement leaves transmit_groups_ intact so remaining synchronous fragments can detect it.
    auto next = Association{};
    next.live = true;
    next.compatible = compatible;
    next.peer = peer;
    next.generation = ++next_generation_;
    next.phase_start_ms = now;
    next.stats.started_ms = now;
    associations_[peer.slot] = next;
    observe_association(peer.slot, network_diagnostics::DirectAssociationPhase::Started, now, direct::AssociationEndReason::Disconnected);
    if (compatible) {
        begin_offer(peer.slot, now);
    }

    publish_route_counts();
}

void Handler::end_association(std::uint8_t slot, direct::AssociationEndReason reason) noexcept {
    if (slot >= associations_.size() || !associations_[slot].live) {
        return;
    }

    auto &association = associations_[slot];
    observe_association(slot, network_diagnostics::DirectAssociationPhase::Ended, GetTickCount64(), reason);
    association = {};
    publish_route_counts();
}

void Handler::end_all(direct::AssociationEndReason reason) noexcept {
    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        end_association(slot, reason);
    }
}

void Handler::observe_association(std::uint8_t slot, network_diagnostics::DirectAssociationPhase phase, std::uint64_t now,
                                  direct::AssociationEndReason end_reason, std::uint32_t attempts) noexcept {
    const auto *output = diagnostics();
    if (output == nullptr) {
        return;
    }

    const auto &association = associations_[slot];
    const auto elapsed = now >= association.stats.started_ms ? now - association.stats.started_ms : 0;
    const auto direct_ms = association.stats.direct_started_ms != 0 && now >= association.stats.direct_started_ms
                               ? now - association.stats.direct_started_ms
                               : 0;
    const network_diagnostics::DirectAssociationObservation observation{
        .slot = slot,
        .peer_id = association.peer.id.value,
        .connection_id = association.connection_id,
        .phase = phase,
        .route = observed_route(association.state),
        .route_reason = static_cast<std::uint8_t>(association.stats.route_reason),
        .end_reason = phase == network_diagnostics::DirectAssociationPhase::Ended ? static_cast<std::uint8_t>(end_reason) : std::uint8_t{},
        .attempts = attempts,
        .tx_datagrams = association.stats.tx_datagrams,
        .rx_datagrams = association.stats.rx_datagrams,
        .send_failures = association.stats.send_failures,
        .endpoint_rejects = association.stats.endpoint_rejects,
        .authentication_rejects = association.stats.authentication_rejects,
        .replay_rejects = association.stats.replay_rejects,
        .invalid_rejects = association.stats.invalid_rejects,
        .elapsed_ms = elapsed,
        .direct_ms = direct_ms,
    };
    output->direct_association(&observation);
}

void Handler::pump_direct() noexcept {
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
        [this, output, detailed, &observation, &datagram](std::int32_t bytes, const direct::Endpoint &source) noexcept {
            handle_direct(std::span<const std::uint8_t>(receive_buffer_).first(bytes), source, output != nullptr ? &datagram : nullptr);
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

void Handler::handle_control(std::uint8_t slot, const direct::ParsedControl &control, std::uint64_t now) noexcept {
    auto &association = associations_[slot];
    // Ready is valid only after an authenticated UDP proof has fixed the client endpoint.
    if (control.kind != direct::ControlKind::Ready || association.state != direct::RouteState::Negotiating ||
        !association.offer_submitted || !association.client_endpoint.valid() || control.connection_id != association.connection_id) {
        association.stats.reject(direct::RejectionKind::Invalid);
        return;
    }

    association.commit_attempts = 0;
    association.phase_start_ms = now;
    set_route(slot, direct::RouteState::AwaitCommit, association.transmit_route);
}

void Handler::handle_direct(std::span<const std::uint8_t> bytes, const direct::Endpoint &source,
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

    const auto found = find_connection(packet.connection_id);
    if (found < 0) {
        direct::set_disposition(datagram, DirectDisposition::Association);
        return;
    }

    const auto slot = static_cast<std::uint8_t>(found);
    auto &association = associations_[slot];
    if (datagram != nullptr) {
        datagram->peer_id = association.peer.id.value;
        datagram->slot = slot;
    }
    // Probes belong to setup; native Data is admitted only after local Commit acceptance.
    const auto valid_state = packet.kind == direct::DirectKind::Probe
                                 ? association.offer_submitted && (association.state == direct::RouteState::Negotiating ||
                                                                   association.state == direct::RouteState::AwaitCommit)
                                 : packet.kind == direct::DirectKind::Data && association.state == direct::RouteState::DirectLocked;
    if (!valid_state) {
        direct::set_disposition(datagram, DirectDisposition::State);
        association.stats.reject(direct::RejectionKind::Invalid);
        return;
    }

    if (!direct::endpoint_admits_source(association.client_endpoint, source, packet.kind == direct::DirectKind::Probe)) {
        direct::set_disposition(datagram, DirectDisposition::Endpoint);
        association.stats.reject(direct::RejectionKind::Endpoint);
        return;
    }

    // Authenticate before updating replay history; a forged sequence must not displace valid traffic.
    if (!direct::verify_authentication_tag(direct::Direction::ClientToServer, association.session_key,
                                           bytes.first(direct::kDirectHeaderBytes), packet.payload)) {
        direct::set_disposition(datagram, DirectDisposition::Authentication);
        association.stats.reject(direct::RejectionKind::Authentication);
        return;
    }

    const auto replay = association.receive_replay.admit(packet.datagram_sequence);
    if (replay == direct::ReplayResult::Duplicate || replay == direct::ReplayResult::Stale || replay == direct::ReplayResult::InvalidJump) {
        direct::set_disposition(datagram, DirectDisposition::Replay, static_cast<std::uint8_t>(replay));
        association.stats.reject(direct::RejectionKind::Replay);
        return;
    }

    if (packet.kind == direct::DirectKind::Probe) {
        direct::set_disposition(datagram, DirectDisposition::Control);
        // The first authenticated full-size Probe fixes this client's source IP/port; ProbeAck tests the return path.
        if (!association.client_endpoint.valid()) {
            association.client_endpoint = source;
        }
        send_probe_ack(slot, source);
        return;
    }

    direct::set_disposition(datagram, DirectDisposition::Data);
    const auto *output = diagnostics();
    // Copy the existing communication header and association identity before rebuilding and transferring the native packet.
    std::array<std::uint8_t, 5> header{};
    network_diagnostics::DirectReceiveObservation observation{};
    if (output != nullptr) {
        if (diagnostics_full_) {
            std::copy_n(packet.payload.data(), header.size(), header.data());
        }
        observation = {
            .slot = slot,
            .peer_id = association.peer.id.value,
            .connection_id = association.connection_id,
            .sequence = packet.datagram_sequence,
            .received_qpc = datagram != nullptr ? datagram->returned_qpc : 0,
            .payload = diagnostics_full_ ? header.data() : nullptr,
            .payload_bytes = diagnostics_full_ ? static_cast<std::uint32_t>(header.size()) : 0,
            .rebuild_result = 5, // The native source address has not yet been validated.
        };
    }
    direct::NativeAddress address{};
    if (!direct::read_native_address(endpoints_[slot], address) || address[0] != association.peer.galaxy_id) {
        direct::set_disposition(datagram, DirectDisposition::Rebuild, 5);
        if (output != nullptr) {
            const auto errors = direct::AmbientErrors::capture();
            output->direct_receive(&observation);
            errors.restore();
        }
        return;
    }

    // Preserve the source before allocation can flush native queues; the authenticated sequence stays consumed on failure.
    void *native_packet{};
    const auto built = direct::build_native_packet(packet_factory_, packet.payload, native_packet);
    if (built != direct::NativePacketResult::Built) {
        direct::set_disposition(datagram, DirectDisposition::Rebuild, static_cast<std::uint8_t>(built));
    }
    if (built == direct::NativePacketResult::Built) {
        // RecvPacket can silently reject a submitted packet. These counters/observations record injection, not gameplay acceptance.
        if (output != nullptr) {
            observation.native_packet = reinterpret_cast<std::uint32_t>(native_packet);
            observation.injected_qpc = diagnostics_full_ ? direct::receive_stamp() : 0;
        }
        submit_native(native_packet, address);
        ++association.stats.rx_datagrams;
    } else if (built == direct::NativePacketResult::RejectedPayload) {
        association.stats.reject(direct::RejectionKind::Invalid);
    } else if (built == direct::NativePacketResult::FactoryUnavailable) {
        log_data_failure(slot, DataFailureKind::PacketFactoryUnavailable);
    } else if (built == direct::NativePacketResult::InvalidPrefix) {
        log_data_failure(slot, DataFailureKind::PacketPrefixInvalid);
    } else if (built == direct::NativePacketResult::AllocationFailed) {
        log_data_failure(slot, DataFailureKind::PacketAllocationFailed);
    }
    if (output != nullptr) {
        observation.rebuild_result = static_cast<std::uint32_t>(built);
        const auto errors = direct::AmbientErrors::capture();
        output->direct_receive(&observation);
        errors.restore();
    }
}

void Handler::begin_offer(std::uint8_t slot, std::uint64_t now) noexcept {
    auto &association = associations_[slot];
    association.phase_start_ms = now;
    association.offer_attempts = 0;
    set_route(slot, direct::RouteState::Negotiating, association.transmit_route);

    std::array<std::uint32_t, kPhysicalAssociationCount> live_ids{};
    for (std::size_t index = 0; index < associations_.size(); ++index) {
        if (associations_[index].live) {
            live_ids[index] = associations_[index].connection_id;
        }
    }
    if (!direct::generate_connection_id(direct::system_random_source(), live_ids, association.connection_id) ||
        !direct::fill_random(direct::system_random_source(), association.session_key)) {
        direct_failure(slot, direct::RouteReason::RandomGenerationFailed);
    }
}

bool Handler::send_control(std::uint8_t slot, std::span<const std::uint8_t> bytes) noexcept {
    return feature_.send(associations_[slot].peer.id, std::as_bytes(bytes)) == fc::SendResult::Accepted;
}

void Handler::send_probe_ack(std::uint8_t slot, const direct::Endpoint &destination) noexcept {
    auto &association = associations_[slot];
    std::array<std::uint8_t, direct::kProbeDatagramBytes> datagram;
    if (!direct::write_probe_datagram(datagram, direct::DirectKind::ProbeAck, direct::Direction::ServerToClient, association.session_key,
                                      association.connection_id, association.send_sequence++)) {
        return;
    }

    const auto sent = socket_.send(datagram, destination);
    if (!sent.succeeded()) {
        ++association.stats.send_failures;
        log_data_failure(slot, DataFailureKind::Send, sent.error);
    }
}

void Handler::service_associations(std::uint64_t now) noexcept {
    // Deadlines and eligible retries are checked after DoRecvs, using Windows milliseconds rather than simulation turns.
    scan_players(now);
    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        auto &association = associations_[slot];
        if (!association.live) {
            continue;
        }

        if (association.state == direct::RouteState::Unclassified) {
            // RequireDirectAll removes peers that never advertise support; other policies never create this state.
            if (now - association.phase_start_ms >= direct::kHandshakeDeadlineMs) {
                remove_peer(slot, RemovalReason::DirectRequired);
            }
            continue;
        }

        if (association.state == direct::RouteState::Negotiating) {
            // Try at most three Offers before five seconds, at elapsed thresholds of zero, one and two seconds.
            // Local acceptance starts a ten-second proof/Ready deadline; address changes do not revise that Offer.
            if (!association.offer_submitted) {
                const auto public_ip = public_ipv4_.load(std::memory_order_acquire);
                if (now - association.phase_start_ms >= direct::kHandshakeDeadlineMs) {
                    direct_failure(slot, public_ip == 0 ? direct::RouteReason::EndpointUnavailable : direct::RouteReason::OfferTimeout);
                } else if (public_ip != 0 && association.offer_attempts < direct::kHandshakeAttemptLimit &&
                           now - association.phase_start_ms >= association.offer_attempts * direct::kHandshakeRetryIntervalMs) {
                    std::array<std::uint8_t, direct::kOfferBytes> offer{};

                    direct::write_offer(offer, association.connection_id, public_ip, game_port_, association.session_key);
                    ++association.offer_attempts;
                    association.offer_submitted = send_control(slot, offer);
                    if (association.offer_submitted) {
                        association.proof_deadline_ms = now + kOfferSafetyTimeoutMs;
                    }
                }
            } else if (now >= association.proof_deadline_ms) {
                direct_failure(slot,
                               association.client_endpoint.valid() ? direct::RouteReason::ReadyTimeout : direct::RouteReason::ProofTimeout);
            }
            continue;
        }

        if (association.state == direct::RouteState::AwaitCommit) {
            // Try at most three Commit submissions before five seconds, at elapsed thresholds of zero, one and two seconds.
            // Delayed receive service can make successive attempts occur less than one second apart.
            if (now - association.phase_start_ms >= direct::kHandshakeDeadlineMs) {
                direct_failure(slot, direct::RouteReason::CommitTimeout);
            } else if (association.commit_attempts < direct::kHandshakeAttemptLimit &&
                       now - association.phase_start_ms >= association.commit_attempts * direct::kHandshakeRetryIntervalMs) {
                std::array<std::uint8_t, direct::kActivationBytes> commit{};
                direct::write_activation(commit, direct::ControlKind::Commit, association.connection_id);
                ++association.commit_attempts;
                // Local queue acceptance enables Direct now; the client can still time out before receiving Commit.
                // Output waits until it is outside an active synchronous group.
                if (send_control(slot, commit)) {
                    set_route(slot, direct::RouteState::DirectLocked, direct::TransmitRoute::DirectPending, direct::RouteReason::Commit);
                }
            }
        }
    }

    service_removals();
}

void Handler::direct_failure(std::uint8_t slot, direct::RouteReason reason) noexcept {
    log_failure(slot, reason,
                direct::policy_action(settings_.policy, true, true) == direct::PolicyAction::Native ? FailureAction::GalaxyFallback
                                                                                                    : FailureAction::NegotiationRemoval);
    if (direct::policy_action(settings_.policy, true, true) == direct::PolicyAction::Native) {
        auto &mutable_association = associations_[slot];
        mutable_association.client_endpoint = {};
        set_route(slot, direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, reason);
    } else {
        remove_peer(slot, RemovalReason::NegotiationFailed);
    }
}

void Handler::set_route(std::uint8_t slot, direct::RouteState state, direct::TransmitRoute route, direct::RouteReason reason) noexcept {
    auto &association = associations_[slot];
    // A generation may settle on one carrier only; reconnecting creates a fresh association before renegotiation.
    if (!direct::valid_route_transition(association.state, state)) {
        return;
    }

    const auto state_changed = association.state != state;
    const auto terminal = state_changed && (state == direct::RouteState::GalaxyLocked || state == direct::RouteState::DirectLocked);
    if (terminal && reason == direct::RouteReason::None) {
        return;
    }

    association.state = state;
    association.transmit_route = route;
    if (terminal) {
        association.stats.route_reason = reason;
        if (state == direct::RouteState::DirectLocked) {
            association.stats.direct_started_ms = GetTickCount64();
            logger_.debug("Selected authenticated UDP for player slot {} (Galaxy ID {})", slot, association.peer.galaxy_id);
        }
        observe_association(slot, network_diagnostics::DirectAssociationPhase::Terminal, GetTickCount64(),
                            direct::AssociationEndReason::Disconnected, association.offer_attempts + association.commit_attempts);
    }
    if (state_changed) {
        publish_route_counts();
    }
}

void Handler::publish_route_counts() noexcept {
    std::uint16_t direct_count{};
    std::uint16_t galaxy_count{};
    std::uint16_t negotiating_count{};
    // Permissive policies leave unsupported native clients outside this array, so these are association counts.
    for (const auto &association : associations_) {
        if (!association.live) {
            continue;
        }

        if (association.state == direct::RouteState::DirectLocked) {
            ++direct_count;
        } else if (association.state == direct::RouteState::GalaxyLocked) {
            ++galaxy_count;
        } else {
            ++negotiating_count;
        }
    }

    direct_count_.store(direct_count, std::memory_order_release);
    galaxy_count_.store(galaxy_count, std::memory_order_release);
    negotiating_count_.store(negotiating_count, std::memory_order_release);
}

} // namespace fc_plugins::direct_transport::server
