#include "handler.hpp"

#include "../ambient_errors.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <optional>

namespace fc_plugins::direct_transport::server {
namespace {

using direct_transport::AmbientErrors;
} // namespace

std::int32_t Handler::final_send(std::int32_t destination, const void *bytes, std::int32_t length) noexcept {
    const auto state = game_.current();
    const auto dedicated = state && state->mode == fc::GameMode::Dedicated;
    // A session exit fails the remaining fragments of an active packet group; ordinary sends use the native carrier.
    if (!dedicated && group_destinations_.primary(destination) < 0) {
        return original_send_.call(destination, bytes, length).value_or(-1);
    }

    const auto incoming = AmbientErrors::capture();
    direct::NativeTransmitResult transmit{};
    if (!dedicated) {
        transmit = {.result = -1, .error = WSAENOTCONN, .handled = true};
    } else if (bytes != nullptr && length > 0) {
        transmit = send(destination, static_cast<const std::uint8_t *>(bytes), static_cast<std::uint32_t>(length));
    }

    // Native delivery frees the outgoing packet after this return, including errors; a failed Direct send loses this fragment.
    auto result = transmit.result;
    // Original-send failures still belong to Native; handled successes can only come from Direct.
    const auto carrier = !transmit.handled      ? network_diagnostics::PacketCarrier::Native
                         : transmit.result >= 0 ? network_diagnostics::PacketCarrier::Direct
                                                : network_diagnostics::PacketCarrier::NotSent;
    if (!transmit.handled) {
        // Let the native sender establish its own error state; a handled Direct failure publishes its Winsock error.
        incoming.restore();
        result = original_send_.call(destination, bytes, length).value_or(-1);
    } else {
        incoming.restore();
        if (result < 0) {
            WSASetLastError(transmit.error);
        }
    }

    const auto outgoing = AmbientErrors::capture();
    if (const auto *output = diagnostics(); output != nullptr) {
        output->transport_send(destination, length > 0 ? static_cast<std::uint32_t>(length) : 0, carrier, result);
    }
    outgoing.restore();
    return result;
}

void Handler::submit_native(void *packet, direct::NativeAddress address) noexcept {
    // RecvPacket owns every rebuilt packet.
    static_cast<void>(packet_intake_.call(packet, address.data()));
    const auto outgoing = AmbientErrors::capture();
    if (const auto *output = diagnostics(); output != nullptr) {
        // The native observer counts total intake; this callback marks the Direct subset of those handoffs.
        output->direct_intake(address.data());
    }
    outgoing.restore();
}

void Handler::peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept {
    if (agreement == fc::Agreement::Unavailable) {
        mailbox_.withdraw(peer);
        return;
    }

    const auto info = multiplayer_.info(peer);
    if (!info || !info->slot || !info->galaxy_id || *info->slot >= 65) {
        return;
    }

    mailbox_.publish({peer, *info->galaxy_id, static_cast<std::uint8_t>(*info->slot)}, agreement);
}

void Handler::control_received(fc::PeerId peer, fc::Bytes bytes) noexcept {
    direct::ParsedControl control{};
    if (direct::parse_control({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, control) == direct::ParseStatus::Ok) {
        mailbox_.receive(peer, control);
    }
}

void Handler::synchronize_control() noexcept {
    const auto state = game_.current();
    // Connection state follows the FC session, so a map change within the same session can keep its routes.
    if (!state || state->mode != fc::GameMode::Dedicated || state->session != session_) {
        end_all(direct::AssociationEndReason::Disconnected);
        removals_.fill({});
        session_ = state && state->mode == fc::GameMode::Dedicated ? state->session : 0;
    }
    if (session_ == 0) {
        return;
    }

    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        std::uint64_t identity{};
        if (associations_[slot].live && (!read_identity(slot, identity) || identity != associations_[slot].peer.galaxy_id)) {
            end_association(slot, direct::AssociationEndReason::Disconnected);
        }

        const auto native_ready = read_identity(slot, identity);
        // FC callbacks can precede native Connect. Keep a compatible publication until this slot has the same user ID.
        const auto publication = mailbox_.take_if(slot, [&](const direct::ControlMailbox::Publication &candidate) noexcept {
            return candidate.agreement != fc::Agreement::Compatible || (native_ready && identity == candidate.peer.galaxy_id);
        });
        if (!publication) {
            continue;
        }

        // Validate the full FC connection when accepting a publication; native slot/user equality alone can repeat on reconnect.
        if (publication->agreement == fc::Agreement::Compatible && read_identity(slot, identity) &&
            identity == publication->peer.galaxy_id && multiplayer_.info(publication->peer.id)) {
            peer_available(publication->peer);
        } else if (publication->agreement == fc::Agreement::Unavailable) {
            peer_unavailable(publication->peer);
        }

        // A failed feature agreement uses CommitTimeout even when no Commit has been attempted.
        if (publication->agreement == fc::Agreement::Failed && current_peer(publication->peer) &&
            associations_[slot].state != direct::RouteState::DirectLocked) {
            direct_failure(slot, direct::RouteReason::CommitTimeout);
        }

        for (std::size_t kind = 0; kind < publication->present.size(); ++kind) {
            if (publication->present[kind] && current_peer(publication->peer)) {
                handle_control(slot, publication->controls[kind], GetTickCount64());
            }
        }
    }
}

void Handler::before_receive() noexcept {
    // The first DoRecvs caller owns associations for this handler's lifetime, including later sessions.
    if (network_thread_.claim()) {
        synchronize_control();
        if (session_ != 0) {
            pump_direct();
        }
    }
}

void Handler::after_receive() noexcept {
    if (!network_thread_.current()) {
        return;
    }

    if (session_ != 0) {
        service_associations(GetTickCount64());
    }
}

std::int32_t Handler::begin_group(std::int32_t destination) noexcept {
    if (!network_thread_.current()) {
        return -1;
    }

    const auto retained_primary = group_destinations_.primary(destination);
    const auto primary = group_destinations_.resolve(destination, [this](std::int32_t current_destination) noexcept {
        return resolve_primary(current_destination);
    });
    // A group begun without a live association gets no pin; later fragments can see a newly created association.
    if (primary < 0 || (retained_primary < 0 && !associations_[primary].live)) {
        return -1;
    }

    auto &association = associations_[primary];
    auto &group = transmit_groups_[primary];
    // Nested DoRecvs can accept Commit between fragments. An existing outer group keeps its selected carrier.
    if (!group.active() && association.transmit_route == direct::TransmitRoute::DirectPending) {
        set_route(static_cast<std::uint8_t>(primary), association.state, direct::TransmitRoute::Direct);
    }

    const auto carrier = association.transmit_route == direct::TransmitRoute::Direct ? direct::Carrier::Direct : direct::Carrier::Galaxy;
    if (!group.begin(static_cast<std::uint8_t>(primary), association.generation, carrier)) {
        return -1;
    }

    if (!group_destinations_.begin(destination, primary)) {
        group.end();
        return -1;
    }

    return primary;
}

void Handler::end_group(std::int32_t destination, std::int32_t primary) noexcept {
    if (network_thread_.current() && primary >= 0 && primary < static_cast<std::int32_t>(associations_.size())) {
        transmit_groups_[primary].end();
        group_destinations_.end(destination);
    }
}

direct::NativeTransmitResult Handler::send(std::int32_t destination, const std::uint8_t *bytes, std::uint32_t size) noexcept {
    if (bytes == nullptr || size == 0) {
        return {};
    }

    const auto rejected = [this, bytes, size, destination](std::uint32_t reason, std::int32_t slot = -1) noexcept {
        const auto *output = diagnostics();
        if (output != nullptr) {
            network_diagnostics::DirectSendObservation observation{.slot = destination,
                                                                   .payload = bytes,
                                                                   .payload_bytes = size,
                                                                   .requested_bytes = size,
                                                                   .returned_bytes = -1,
                                                                   .error = WSAENOTCONN,
                                                                   .phase = 3,
                                                                   .rejection = reason};
            if (slot >= 0) {
                observation.peer_id = associations_[slot].peer.id.value;
                observation.connection_id = associations_[slot].connection_id;
            }
            const auto errors = AmbientErrors::capture();
            output->direct_send(&observation);
            errors.restore();
        }
        return direct::NativeTransmitResult{.result = -1, .error = WSAENOTCONN, .handled = true};
    };

    if (!network_thread_.current()) {
        return rejected(1);
    }

    // Nested DoRecvs can clear or remap endpoints between fragments; consult the original group before native lookup.
    const auto retained_primary = group_destinations_.primary(destination);
    const auto primary = group_destinations_.resolve(destination, [this](std::int32_t current_destination) noexcept {
        return resolve_primary(current_destination);
    });
    if (retained_primary >= 0 && !group_destination_matches(transmit_groups_[primary], primary, resolve_primary(destination),
                                                            associations_[primary].live, associations_[primary].generation)) {
        return rejected(4, primary);
    }

    if (primary < 0 || !associations_[primary].live) {
        if (primary >= 0 && transmit_groups_[primary].active()) {
            return rejected(3, primary);
        }
        return {};
    }

    auto &association = associations_[primary];
    auto &group = transmit_groups_[primary];
    std::uint64_t identity{};
    if (!read_identity(static_cast<std::uint8_t>(primary), identity) || identity != association.peer.galaxy_id) {
        return rejected(2, primary);
    }

    const auto grouped = group.active();
    const auto stale_group = grouped && !group.belongs_to(static_cast<std::uint8_t>(primary), association.generation);
    if (stale_group) {
        return rejected(4, primary);
    }

    auto carrier = grouped ? group.carrier() : direct::Carrier::Galaxy;
    // Sends outside an active observer group apply Commit immediately, including delayed NetDrag fragments
    // whose original group call has already returned.
    if (!grouped && association.transmit_route == direct::TransmitRoute::DirectPending) {
        set_route(static_cast<std::uint8_t>(primary), association.state, direct::TransmitRoute::Direct);
    }
    if (!grouped && association.transmit_route == direct::TransmitRoute::Direct) {
        carrier = direct::Carrier::Direct;
    }
    if (carrier != direct::Carrier::Direct) {
        return {};
    }

    if (!direct_route_ready(static_cast<std::uint8_t>(primary))) {
        return rejected(0x100U | (static_cast<std::uint32_t>(association.state != direct::RouteState::DirectLocked) << 0) |
                            (static_cast<std::uint32_t>(!association.client_endpoint.valid()) << 1) |
                            (static_cast<std::uint32_t>(!socket_.available()) << 2),
                        primary);
    }

    return send_direct_packet(static_cast<std::uint8_t>(primary), {bytes, size});
}

direct::NativeTransmitResult Handler::send_direct_packet(std::uint8_t slot, std::span<const std::uint8_t> bytes) noexcept {
    auto &association = associations_[slot];
    const auto *output = diagnostics();
    const auto sequence = association.send_sequence++;
    std::optional<network_diagnostics::DirectSendObservation> observation;
    if (output != nullptr && diagnostics_full_) {
        observation.emplace(network_diagnostics::DirectSendObservation{
            .slot = slot,
            .peer_id = association.peer.id.value,
            .connection_id = association.connection_id,
            .sequence = sequence,
            .payload = bytes.data(),
            .payload_bytes = static_cast<std::uint32_t>(bytes.size()),
        });
    }
    const auto result = direct::send_direct_data(socket_, send_buffer_, direct::Direction::ServerToClient, association.session_key,
                                                 association.connection_id, sequence, association.client_endpoint, bytes,
                                                 observation ? &*observation : nullptr);
    if (output != nullptr && (diagnostics_full_ || result.result < 0)) {
        if (!observation) {
            observation.emplace(network_diagnostics::DirectSendObservation{
                .slot = slot,
                .peer_id = association.peer.id.value,
                .connection_id = association.connection_id,
                .sequence = sequence,
                .requested_bytes = static_cast<std::uint32_t>(bytes.size()),
                .returned_bytes = result.result,
                .error = result.error,
            });
        }
        const auto errors = AmbientErrors::capture();
        output->direct_send(&*observation);
        errors.restore();
    }
    if (result.result < 0) {
        ++association.stats.send_failures;
        log_data_failure(slot, DataFailureKind::Send, result.error);
    } else {
        ++association.stats.tx_datagrams;
    }
    return result;
}

bool Handler::direct_route_ready(std::uint8_t slot) const noexcept {
    if (slot >= associations_.size()) {
        return false;
    }

    const auto &association = associations_[slot];
    return association.live && association.state == direct::RouteState::DirectLocked && association.client_endpoint.valid() &&
           socket_.available();
}

} // namespace fc_plugins::direct_transport::server
