#include "handler.hpp"

#include <WinSock2.h>

#include <cstdio>
#include <format>
#include <string_view>

namespace fc_plugins::direct_transport::client {
namespace {

namespace direct = direct_transport;

// Packs the address and port into one atomic status value.
std::uint64_t pack_endpoint(const direct::Endpoint &endpoint) noexcept {
    return endpoint.ipv4_network_order | (static_cast<std::uint64_t>(endpoint.port_host_order) << 32);
}

// Restores the address and port from a copied atomic status value.
direct::Endpoint unpack_endpoint(std::uint64_t packed) noexcept {
    return {static_cast<std::uint32_t>(packed), static_cast<std::uint16_t>(packed >> 32)};
}

// Names the current carrier or negotiation phase for status output.
std::string_view route_name(direct::RouteState route) noexcept {
    switch (route) {
    case direct::RouteState::Unclassified:
        // Also used when there is no compatible association, including outside a Client session.
        return "Waiting";
    case direct::RouteState::Negotiating:
    case direct::RouteState::AwaitCommit:
        return "Negotiating";
    case direct::RouteState::GalaxyLocked:
        return "GalaxyPeer";
    case direct::RouteState::DirectLocked:
        return "Direct";
    }
    return "Unknown";
}

// Describes the client's timeout or local failure when it chooses Galaxy.
std::string_view route_reason(direct::RouteReason reason) noexcept {
    switch (reason) {
    case direct::RouteReason::OfferTimeout:
        return "No server Offer was processed before the deadline";
    case direct::RouteReason::ProofTimeout:
        return "No valid UDP ProbeAck was received before the deadline";
    case direct::RouteReason::ReadyTimeout:
        return "FC did not accept the Ready message before the deadline";
    case direct::RouteReason::CommitTimeout:
        return "Direct setup failed or no Commit or authenticated Data was processed before the deadline";
    case direct::RouteReason::SocketUnavailable:
        return "The UDP socket is unavailable";
    case direct::RouteReason::RandomGenerationFailed:
        return "Could not generate a connection ID or session key";
    default:
        return "Direct negotiation ended";
    }
}

} // namespace

Handler::Handler(const fc::CreateContext &context) noexcept : logger_(context.log()) {}

Handler::~Handler() {
    end_association(direct::AssociationEndReason::Shutdown);
    socket_.stop();
}

void Handler::plan(fc::Plan &plan) {
    const auto game = direct::select_game_layout(plan.profile());
    const auto network = direct_transport::select_network_layout(plan.profile());
    if (!game || !network) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    packet_factory_ = {
        .allocate = plan.function(game->allocate_packet),
        .initialize = plan.function(game->initialize_packet),
    };
    host_connected_memory_ = plan.access<const std::byte>(game->endpoints[64].connected.rva);
    host_identity_memory_ = plan.access<const std::uint64_t>(game->endpoints[64].identity.rva, direct::NativeAddress{}.size());
    prefix_memory_ = plan.access<const std::uint32_t>(game->packet_prefix.rva);
    packet_intake_ = plan.function(network->packet_intake);

    original_send_ = plan.hook(network->final_send, [this](std::int32_t destination, const void *bytes, std::int32_t length) noexcept {
        return final_send(destination, bytes, length);
    });

    // The invocation records its successful begin so nested early returns cannot end an outer group.
    plan.observe<bool>(network->group_send, fc::before([this](std::int32_t destination, std::int32_t, void *, bool &begun) noexcept {
                           begun = begin_group(destination);
                       }),
                       fc::after([this](std::int32_t destination, std::int32_t, void *, const bool &begun) noexcept {
                           if (begun) {
                               end_group(destination);
                           }
                       }));

    plan.observe(network->receive_service, fc::before([this]() noexcept {
                     before_receive();
                 }),
                 fc::after([this]() noexcept {
                     after_receive();
                 }));
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    const auto prefix = prefix_memory_.view();
    if (!prefix) {
        return std::unexpected(prefix.error());
    }

    // FC keeps the native read views alive for the installation; the factory borrows their pointers.
    packet_factory_.prefix = prefix->data();

    const auto connected = host_connected_memory_.view();
    const auto identity = host_identity_memory_.view();
    if (!connected) {
        return std::unexpected(connected.error());
    }
    if (!identity) {
        return std::unexpected(identity.error());
    }
    host_endpoint_ = {connected->data(), identity->data()};

    // FC owns compatibility and control framing; callbacks copy their borrowed data into the mailbox.
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    const auto feature = multiplayer_.feature({std::string{direct::kFeatureId}, fc::FeatureRole::Client});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    if (auto result = feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            peer_changed(peer, agreement);
        });
        !result) {
        return result;
    }
    if (auto result = feature_.on_message([this](fc::PeerId peer, fc::Bytes bytes) noexcept {
            control_received(peer, bytes);
        });
        !result) {
        return result;
    }

    if (auto result = context.on_available("Diagnostics.NetworkDiagnostics", network_diagnostics::api_contract(),
                                           [this](const DiagnosticsApi &api) noexcept {
                                               connect_diagnostics(api);
                                           });
        !result) {
        return result;
    }

    // Bind a separate UDP socket to an OS-selected port. Failure leaves the native route available.
    std::int32_t error{};
    std::int32_t buffer_error{};
    const auto available_socket = socket_.start(0, error, buffer_error);
    published_port_.store(available_socket ? socket_.bound_port() : 0, std::memory_order_release);
    if (!available_socket) {
        logger_.warning("Direct UDP is unavailable; the game's normal transport will be used (Winsock error {})", error);
    } else if (buffer_error != 0) {
        logger_.warning("Direct UDP buffer tuning failed; system buffer sizes will be used (Winsock error {})", buffer_error);
    }
    return {};
}

void Handler::connect_diagnostics(DiagnosticsApi diagnostics) noexcept {
    // Publish all function handles before a native callback can read the API.
    diagnostics_full_ = diagnostics.capture_mode() == 2;
    diagnostics_ = diagnostics;
    diagnostics_ready_.store(true, std::memory_order_release);
}

const Handler::DiagnosticsApi *Handler::diagnostics() const noexcept {
    return diagnostics_ready_.load(std::memory_order_acquire) ? &diagnostics_ : nullptr;
}

void Handler::update(const fc::UpdateContext &) noexcept {
    // Native callbacks increment counters; report cumulative totals at powers of two here.
    const auto rejected = network_thread_.rejected();
    if (thread_log_.admit(rejected)) {
        logger_.warning("Direct Transport callbacks rejected from threads other than the first receive thread: {}", rejected);
    }

    const auto receive_errors = receive_errors_.load(std::memory_order_acquire);
    if (receive_error_log_.admit(receive_errors)) {
        logger_.warning("Direct UDP receive failures: {}; latest Winsock error {}", receive_errors,
                        latest_receive_error_.load(std::memory_order_acquire));
    }

    const auto drain_limits = drain_limits_.load(std::memory_order_acquire);
    if (drain_limit_log_.admit(drain_limits)) {
        logger_.warning("Direct UDP receive work limit reached; count: {}", drain_limits);
    }

    const auto prefix_failures = packet_prefix_failures_.load(std::memory_order_acquire);
    if (packet_prefix_log_.admit(prefix_failures)) {
        logger_.error("Direct packet rebuild failures: {}; latest server Galaxy user ID {}: the game packet prefix is "
                      "invalid",
                      prefix_failures, latest_prefix_failure_peer_.load(std::memory_order_relaxed));
    }

    const auto allocation_failures = packet_allocation_failures_.load(std::memory_order_acquire);
    if (packet_allocation_log_.admit(allocation_failures)) {
        logger_.warning("Direct packet allocation failures: {}; latest server Galaxy user ID {}", allocation_failures,
                        latest_allocation_failure_peer_.load(std::memory_order_relaxed));
    }
}

void Handler::log_route(direct::RouteState state, direct::RouteReason reason) noexcept {
    if (state == direct::RouteState::DirectLocked) {
        logger_.debug("Selected authenticated UDP for the server (Galaxy user ID {}, lobby ID {})", association_.peer.galaxy_id,
                      association_.lobby_id);
    } else {
        logger_.debug("Kept the server on GalaxyPeer (Galaxy user ID {}, lobby ID {}): {}", association_.peer.galaxy_id,
                      association_.lobby_id, route_reason(reason));
    }
}

void Handler::record_drain(direct::DatagramDrainResult result) noexcept {
    if (result.error != 0) {
        latest_receive_error_.store(result.error, std::memory_order_release);
        receive_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    if (result.limit_reached) {
        drain_limits_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Handler::publish_status() noexcept {
    // Each value is atomic; readers can see route and endpoint values from different updates.
    published_route_.store(association_.live ? association_.state : direct::RouteState::Unclassified, std::memory_order_release);
    published_endpoint_.store(association_.live ? pack_endpoint(association_.server_endpoint) : 0, std::memory_order_release);
}

void Handler::write_status(fc::StatusContext &output) const noexcept {
    const auto port = published_port_.load(std::memory_order_acquire);
    if (port != 0) {
        output.line("UDP port: {}", port);
    } else {
        output.line("UDP socket: {}", std::string_view{"Unavailable"});
    }

    output.line("Route: {}", route_name(published_route_.load(std::memory_order_acquire)));
    const auto endpoint = unpack_endpoint(published_endpoint_.load(std::memory_order_acquire));
    if (endpoint.valid()) {
        char text[48]{};
        output.line("Server endpoint: {}", direct::format_endpoint(endpoint, text));
    }
}

} // namespace fc_plugins::direct_transport::client
