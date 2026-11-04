#include "handler.hpp"

#include "../ambient_errors.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <optional>

namespace fc_plugins::direct_transport::client {
namespace direct = direct_transport;

namespace {

constexpr std::uint8_t kHostSlot = 64;

using direct_transport::AmbientErrors;
} // namespace

std::int32_t Handler::final_send(std::int32_t destination, const void *bytes, std::int32_t length) noexcept {
    const auto state = game_.current();
    const auto client = state && state->mode == fc::GameMode::Client;
    // A retained host group fails after session exit until the balanced observer ends it.
    if (!client && (destination != kHostSlot || !transmit_group_.active())) {
        return original_send_.call(destination, bytes, length).value_or(-1);
    }

    const auto incoming = AmbientErrors::capture();
    direct::NativeTransmitResult transmit{};
    if (!client) {
        transmit = {.result = -1, .error = WSAENOTCONN, .handled = true};
    } else if (bytes != nullptr && length > 0) {
        transmit = send(destination, static_cast<const std::uint8_t *>(bytes), static_cast<std::uint32_t>(length));
    }

    auto result = transmit.result;
    // Record which carrier was attempted; a failed original send still belongs to Native.
    const auto carrier = !transmit.handled      ? network_diagnostics::PacketCarrier::Native
                         : transmit.result >= 0 ? network_diagnostics::PacketCarrier::Direct
                                                : network_diagnostics::PacketCarrier::NotSent;
    if (!transmit.handled) {
        incoming.restore();
        result = original_send_.call(destination, bytes, length).value_or(-1);
    } else {
        incoming.restore();
        if (result < 0) {
            WSASetLastError(transmit.error);
        }
    }

    // Diagnostics must preserve the error produced by either sender.
    const auto outgoing = AmbientErrors::capture();
    if (const auto *output = diagnostics(); output != nullptr) {
        output->transport_send(destination, length > 0 ? static_cast<std::uint32_t>(length) : 0, carrier, result);
    }
    outgoing.restore();
    return result;
}

void Handler::submit_native(void *packet, direct::NativeAddress address) noexcept {
    // Intake owns every rebuilt packet.
    static_cast<void>(packet_intake_.call(packet, address.data()));
    const auto outgoing = AmbientErrors::capture();
    if (const auto *output = diagnostics(); output != nullptr) {
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
    // Session IDs survive map rotation; a changed session or participation mode retires the host.
    const auto state = game_.current();
    if (!state || state->mode != fc::GameMode::Client || state->session != session_) {
        end_association(direct::AssociationEndReason::Disconnected);
        session_ = state && state->mode == fc::GameMode::Client ? state->session : 0;
    }
    if (session_ == 0) {
        return;
    }

    // Match the raw lobby separately from FC's current connection and resolved owner user ID.
    const auto host = multiplayer_.host();
    std::uint64_t identity{};
    if (association_.live &&
        (!direct::read_native_identity(host_endpoint_, identity) || identity != association_.lobby_id || host != association_.peer.id)) {
        end_association(direct::AssociationEndReason::Disconnected);
    }

    // FC identifies the owner connection; native readiness does not require the lobby ID to equal the user ID.
    const auto native_ready = direct::read_native_identity(host_endpoint_, identity);
    const auto publication = mailbox_.take_if(kHostSlot, [&](const direct::ControlMailbox::Publication &candidate) noexcept {
        return candidate.agreement != fc::Agreement::Compatible || (native_ready && host == candidate.peer.id);
    });
    if (!publication) {
        return;
    }

    if (publication->agreement == fc::Agreement::Compatible && multiplayer_.host() == publication->peer.id &&
        direct::read_native_identity(host_endpoint_, identity)) {
        peer_available(publication->peer);
    } else if (publication->agreement == fc::Agreement::Unavailable) {
        peer_unavailable(publication->peer);
    }

    if (publication->agreement == fc::Agreement::Failed && current_peer(publication->peer) &&
        association_.state != direct::RouteState::DirectLocked) {
        set_route(direct::RouteState::GalaxyLocked, direct::TransmitRoute::Galaxy, false, direct::RouteReason::CommitTimeout);
    }

    // Consume the copied controls in Offer, Ready, Commit order for the matching FC connection.
    for (std::size_t kind = 0; kind < publication->present.size(); ++kind) {
        if (publication->present[kind] && current_peer(publication->peer)) {
            handle_control(publication->controls[kind], GetTickCount64());
        }
    }
}

void Handler::before_receive() noexcept {
    // The first DoRecvs caller owns native state for the handler's lifetime, even outside Client mode.
    if (network_thread_.claim()) {
        synchronize_control();
        if (session_ != 0) {
            pump_direct(GetTickCount64());
        }
    }
}

void Handler::after_receive() noexcept {
    if (network_thread_.current()) {
        if (session_ != 0) {
            service_association(GetTickCount64());
        }
    }
}

bool Handler::begin_group(std::int32_t destination) noexcept {
    if (destination != kHostSlot || !network_thread_.current() || !association_.live) {
        return false;
    }

    // Fragment sends can run DoRecvs before the next sibling. Pin the current connection and carrier;
    // native delayed sends can outlive this group call.
    if (!transmit_group_.active() && association_.transmit_route == direct::TransmitRoute::DirectPending) {
        set_route(association_.state, direct::TransmitRoute::Direct, association_.receive_permission);
    }

    const auto carrier = association_.transmit_route == direct::TransmitRoute::Direct ? direct::Carrier::Direct : direct::Carrier::Galaxy;
    return transmit_group_.begin(kHostSlot, association_.generation, carrier);
}

void Handler::end_group(std::int32_t destination) noexcept {
    if (destination == kHostSlot && network_thread_.current()) {
        transmit_group_.end();
    }
}

direct::NativeTransmitResult Handler::send(std::int32_t destination, const std::uint8_t *bytes, std::uint32_t size) noexcept {
    if (destination != kHostSlot || bytes == nullptr || size == 0) {
        return {};
    }
    const auto rejected = [this, bytes, size](std::uint32_t reason) noexcept {
        const auto *output = diagnostics();
        if (output != nullptr) {
            network_diagnostics::DirectSendObservation observation{.slot = kHostSlot,
                                                                   .payload = bytes,
                                                                   .payload_bytes = size,
                                                                   .requested_bytes = size,
                                                                   .returned_bytes = -1,
                                                                   .error = WSAENOTCONN,
                                                                   .phase = 3,
                                                                   .rejection = reason};
            if (reason != 1) {
                observation.peer_id = association_.peer.id.value;
                observation.connection_id = association_.connection_id;
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

    // A group keeps its original association generation across nested receive callbacks.
    // If that association is replaced or retired, fail its remaining fragments here.
    const auto grouped = transmit_group_.active();
    const auto stale_group = grouped && !transmit_group_.belongs_to(kHostSlot, association_.generation);
    std::uint64_t identity{};
    if (association_.live && (!direct::read_native_identity(host_endpoint_, identity) || identity != association_.lobby_id)) {
        return rejected(2);
    }
    if (!association_.live || stale_group) {
        return grouped || association_.transmit_route != direct::TransmitRoute::Galaxy ? rejected(stale_group ? 4U : 3U)
                                                                                       : direct::NativeTransmitResult{};
    }

    // An ungrouped send can apply the pending switch to Direct immediately.
    auto carrier = grouped ? transmit_group_.carrier() : direct::Carrier::Galaxy;
    if (!grouped && association_.transmit_route == direct::TransmitRoute::DirectPending) {
        set_route(association_.state, direct::TransmitRoute::Direct, association_.receive_permission);
    }
    if (!grouped && association_.transmit_route == direct::TransmitRoute::Direct) {
        carrier = direct::Carrier::Direct;
    }
    if (carrier != direct::Carrier::Direct) {
        return {};
    }

    if (association_.state != direct::RouteState::DirectLocked || !association_.server_endpoint.valid() || !socket_.available()) {
        return rejected(0x100U | (static_cast<std::uint32_t>(association_.state != direct::RouteState::DirectLocked) << 0) |
                        (static_cast<std::uint32_t>(!association_.server_endpoint.valid()) << 1) |
                        (static_cast<std::uint32_t>(!socket_.available()) << 2));
    }

    // A Direct failure remains handled. The native caller frees this outgoing packet rather
    // than retrying the same fragment through Galaxy.
    const auto *output = diagnostics();
    const auto sequence = association_.send_sequence++;
    std::optional<network_diagnostics::DirectSendObservation> observation;
    if (output != nullptr && diagnostics_full_) {
        observation.emplace(network_diagnostics::DirectSendObservation{
            .slot = kHostSlot,
            .peer_id = association_.peer.id.value,
            .connection_id = association_.connection_id,
            .sequence = sequence,
            .payload = bytes,
            .payload_bytes = size,
        });
    }
    const auto result = direct::send_direct_data(socket_, send_buffer_, direct::Direction::ClientToServer, association_.session_key,
                                                 association_.connection_id, sequence, association_.server_endpoint, {bytes, size},
                                                 observation ? &*observation : nullptr);
    if (output != nullptr && (diagnostics_full_ || result.result < 0)) {
        if (!observation) {
            observation.emplace(network_diagnostics::DirectSendObservation{
                .slot = kHostSlot,
                .peer_id = association_.peer.id.value,
                .connection_id = association_.connection_id,
                .sequence = sequence,
                .requested_bytes = size,
                .returned_bytes = result.result,
                .error = result.error,
            });
        }
        const auto errors = AmbientErrors::capture();
        output->direct_send(&*observation);
        errors.restore();
    }
    if (result.result < 0) {
        ++association_.stats.send_failures;
    } else {
        ++association_.stats.tx_datagrams;
    }
    return result;
}

} // namespace fc_plugins::direct_transport::client
