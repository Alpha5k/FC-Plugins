#include "handler.hpp"

#include "../ambient_errors.hpp"

#include <WinSock2.h>

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace fc_plugins::direct_transport::server {
namespace {

// Names the configured admission policy for status output.
std::string_view policy_name(direct::Policy policy) noexcept {
    switch (policy) {
    case direct::Policy::Disabled:
        return "Disabled";
    case direct::Policy::PreferDirect:
        return "Prefer direct";
    case direct::Policy::RequireDirectPatched:
        return "Require direct for compatible clients";
    case direct::Policy::RequireDirectAll:
        return "Require direct for all clients";
    }
    return "Unknown";
}

// Describes the observed server negotiation failure for the policy outcome log.
std::string_view failure_name(direct::RouteReason reason) noexcept {
    switch (reason) {
    case direct::RouteReason::OfferTimeout:
        return "the Direct Transport offer could not be queued";
    case direct::RouteReason::ProofTimeout:
        return "UDP endpoint proof timed out";
    case direct::RouteReason::ReadyTimeout:
        return "the client's Ready message was not received";
    case direct::RouteReason::CommitTimeout:
        return "Direct Transport activation did not complete";
    case direct::RouteReason::EndpointUnavailable:
        return "no usable public IPv4 address was observed";
    case direct::RouteReason::RandomGenerationFailed:
        return "a connection ID or session key could not be generated";
    default:
        return "direct negotiation failed";
    }
}

} // namespace

Handler::Handler(const fc::CreateContext &context, const Settings &settings) noexcept : settings_(settings), logger_(context.log()) {}

Handler::~Handler() {
    end_all(direct::AssociationEndReason::Shutdown);
    socket_.stop();
}

void Handler::plan(fc::Plan &plan) {
    if (settings_.policy == direct::Policy::Disabled) {
        return;
    }

    auto game = plan.for_image("game");
    const auto game_layout = direct::select_game_layout(game.profile());
    const auto server = select_server_layout(game.profile());
    const auto network = direct_transport::select_network_layout(game.profile());
    if (!game_layout || !server || !network) {
        game.fail(std::format("No native layout for image profile '{}'", game.profile()));
        return;
    }

    // Keep all 67 native destinations readable; only physical client slots 0–63 own associations.
    for (std::size_t slot = 0; slot < endpoints_.size(); ++slot) {
        connected_memory_[slot] = game.access<const std::byte>(game_layout->endpoints[slot].connected.rva);
        identity_memory_[slot] =
            game.access<const std::uint64_t>(game_layout->endpoints[slot].identity.rva, direct::NativeAddress{}.size());
    }
    packet_factory_ = {
        .allocate = game.function(game_layout->allocate_packet),
        .initialize = game.function(game_layout->initialize_packet),
    };
    prefix_memory_ = game.access<const std::uint32_t>(game_layout->packet_prefix.rva);
    packet_intake_ = game.function(network->packet_intake);
    port_memory_ = game.access<const std::uint32_t>(server->game_port.rva);
    is_playing_ = game.function(server->is_playing);
    update_boot_player_ = game.function(server->update_boot_player);
    set_not_playing_ = game.function(server->set_not_playing);
    get_guest_owner_ = game.function(server->get_guest_owner);
    // The Galaxy bridge observes this same boundary; retiring here closes reuse before the next mailbox sample.
    game.observe(server->disconnect, fc::after([this](std::int32_t slot) noexcept {
                     const auto errors = direct::AmbientErrors::capture();
                     native_disconnect(slot);
                     errors.restore();
                 }));

    original_send_ = game.hook(network->final_send, [this](std::int32_t destination, const void *bytes, std::int32_t length) noexcept {
        return final_send(destination, bytes, length);
    });
    // Each invocation remembers whether it began a group, even if native work replaces the slot before returning.
    game.observe<std::int32_t>(network->group_send,
                               fc::before([this](std::int32_t destination, std::int32_t, void *, std::int32_t &primary) noexcept {
                                   primary = begin_group(destination);
                               }),
                               fc::after([this](std::int32_t destination, std::int32_t, void *, const std::int32_t &primary) noexcept {
                                   if (primary >= 0) {
                                       end_group(destination, primary);
                                   }
                               }));
    game.observe(network->receive_service, fc::before([this]() noexcept {
                     before_receive();
                 }),
                 fc::after([this]() noexcept {
                     after_receive();
                 }));

    auto galaxy_peer = plan.for_image("galaxy_peer");
    const auto galaxy_layout = select_galaxy_peer_layout(galaxy_peer.profile());
    if (!galaxy_layout) {
        galaxy_peer.fail(std::format("No native layout for image profile '{}'", galaxy_peer.profile()));
        return;
    }
    vtable_memory_ = galaxy_peer.access<const std::byte>(galaxy_layout->raw_peer_vtable.rva, 4);
    owner_vtable_memory_ = galaxy_peer.access<const std::byte>(galaxy_layout->owner_peer_vtable.rva, 4);
    owner_callback_memory_ = galaxy_peer.access<const std::byte>(galaxy_layout->owner_callback.rva);
    unassigned_memory_ = galaxy_peer.access<const GalaxyAddress>(galaxy_layout->unassigned_address.rva);
    owner_callback_offset_ = galaxy_layout->owner_callback_offset;
    owner_context_offset_ = galaxy_layout->owner_context_offset;
    // Observe queries the DLL already makes. No query, NAT mapping or Direct port discovery is requested here.
    galaxy_peer.observe(galaxy_layout->get_external_id,
                        fc::after([this](void *raw_peer, GalaxyAddress *, GalaxyAddress observer, GalaxyAddress *result) noexcept {
                            observe_galaxy_peer(raw_peer, observer, result);
                        }));
    galaxy_peer.observe(galaxy_layout->destroy_raw_peer, fc::before([this](void *raw_peer) noexcept {
                            destroy_galaxy_peer(raw_peer);
                        }));
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    if (settings_.policy == direct::Policy::Disabled) {
        return {};
    }

    const auto prefix = prefix_memory_.view();
    if (!prefix) {
        return std::unexpected(prefix.error());
    }

    packet_factory_.prefix = prefix->data();
    const auto port = port_memory_.view();
    if (!port) {
        return std::unexpected(port.error());
    }

    const auto *game_port_value = port->data();
    for (std::size_t slot = 0; slot < endpoints_.size(); ++slot) {
        const auto connected = connected_memory_[slot].view();
        const auto identity = identity_memory_[slot].view();
        if (!connected) {
            return std::unexpected(connected.error());
        }
        if (!identity) {
            return std::unexpected(identity.error());
        }
        endpoints_[slot] = {connected->data(), identity->data()};
    }

    if (*game_port_value == 0 || *game_port_value > UINT16_MAX) {
        return std::unexpected(fc::Error{"Direct Transport requires a valid /gameport"});
    }

    game_port_ = static_cast<std::uint16_t>(*game_port_value);

    const auto vtable = vtable_memory_.view();
    if (!vtable) {
        return std::unexpected(vtable.error());
    }

    // The object's first pointer is the table address; it identifies the implementation shared by raw RakPeer instances.
    raw_peer_vtable_ = vtable->data();
    const auto owner_vtable = owner_vtable_memory_.view();
    const auto owner_callback = owner_callback_memory_.view();
    const auto unassigned = unassigned_memory_.view();
    if (!owner_vtable) {
        return std::unexpected(owner_vtable.error());
    }
    if (!owner_callback) {
        return std::unexpected(owner_callback.error());
    }
    if (!unassigned) {
        return std::unexpected(unassigned.error());
    }
    owner_peer_vtable_ = owner_vtable->data();
    owner_callback_ = owner_callback->data();
    unassigned_address_ = unassigned->data();

    game_ = context.game();
    multiplayer_ = context.multiplayer();
    const auto feature = multiplayer_.feature({std::string{direct::kFeatureId}, fc::FeatureRole::Host});
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

    std::int32_t error{};
    std::int32_t buffer_error{};
    // This socket is independent of the game's and GalaxyPeer's sockets. Bind failure aborts installation
    // under every enabled policy; PreferDirect fallback applies only after negotiation starts.
    if (!socket_.start(game_port_, error, buffer_error)) {
        char reason[96]{};
        std::snprintf(reason, sizeof(reason), "Could not start Direct UDP on /gameport (Winsock error %d)", error);
        return std::unexpected(fc::Error{reason});
    }

    if (buffer_error != 0) {
        logger_.warning("Direct UDP buffer tuning failed; system buffer sizes will be used (Winsock error {})", buffer_error);
    }
    return {};
}

void Handler::connect_diagnostics(DiagnosticsApi diagnostics) noexcept {
    diagnostics_full_ = diagnostics.capture_mode() == 2;
    diagnostics_ = diagnostics;
    diagnostics_ready_.store(true, std::memory_order_release);
}

const Handler::DiagnosticsApi *Handler::diagnostics() const noexcept {
    return diagnostics_ready_.load(std::memory_order_acquire) ? &diagnostics_ : nullptr;
}

void Handler::update(const fc::UpdateContext &) noexcept {
    if (settings_.policy == direct::Policy::Disabled) {
        return;
    }

    const auto rejected = network_thread_.rejected();
    if (thread_log_.admit(rejected)) {
        logger_.warning("Direct Transport callbacks ignored from unexpected threads: {}", rejected);
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
}

void Handler::log_failure(std::uint8_t slot, direct::RouteReason route_reason, FailureAction action) noexcept {
    const auto galaxy_id = associations_[slot].peer.galaxy_id;
    if (action == FailureAction::UnsupportedRemoval) {
        logger_.warning("Scheduled removal of player slot {} (Galaxy ID {}) because required Direct Transport is unsupported", slot,
                        galaxy_id);
        return;
    }

    const auto reason = failure_name(route_reason);
    if (action == FailureAction::GalaxyFallback) {
        if (route_reason == direct::RouteReason::RandomGenerationFailed) {
            logger_.error("Kept player slot {} (Galaxy ID {}) on GalaxyPeer because {}", slot, galaxy_id, reason);
        } else {
            logger_.debug("Kept player slot {} (Galaxy ID {}) on GalaxyPeer: {}", slot, galaxy_id, reason);
        }
    } else if (route_reason == direct::RouteReason::RandomGenerationFailed) {
        logger_.error("Scheduled removal of player slot {} (Galaxy ID {}) because {}", slot, galaxy_id, reason);
    } else {
        logger_.warning("Scheduled removal of player slot {} (Galaxy ID {}) after Direct Transport failed: {}", slot, galaxy_id, reason);
    }
}

void Handler::log_data_failure(std::uint8_t slot, DataFailureKind kind, std::int32_t system_error) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    const auto failures = ++data_failure_counts_[index];
    if (!data_failure_logs_[index].admit(failures)) {
        return;
    }

    const auto galaxy_id = associations_[slot].peer.galaxy_id;
    switch (kind) {
    case DataFailureKind::Send:
        logger_.warning("Direct UDP send failures: {}; latest player slot {} (Galaxy ID {}, Winsock error {})", failures, slot, galaxy_id,
                        system_error);
        break;
    case DataFailureKind::PacketFactoryUnavailable:
        logger_.error("Direct packet rebuild failures: {}; latest player slot {} (Galaxy ID {}): the "
                      "game's packet factory is unavailable",
                      failures, slot, galaxy_id);
        break;
    case DataFailureKind::PacketPrefixInvalid:
        logger_.error("Direct packet rebuild failures: {}; latest player slot {} (Galaxy ID {}): the "
                      "game's packet prefix is invalid",
                      failures, slot, galaxy_id);
        break;
    case DataFailureKind::PacketAllocationFailed:
        logger_.warning("Direct packet allocation failures: {}; latest player slot {} (Galaxy ID {})", failures, slot, galaxy_id);
        break;
    case DataFailureKind::Count:
        break;
    }
}

void Handler::write_status(fc::StatusContext &output) const noexcept {
    output.line("Policy: {}", policy_name(settings_.policy));
    if (settings_.policy == direct::Policy::Disabled) {
        output.line("State: {}", "Disabled");
        return;
    }

    output.line("UDP port: {}", game_port_);
    const auto public_ip = public_ipv4_.load(std::memory_order_acquire);
    if (public_ip != 0) {
        char text[48]{};
        output.line("Advertised UDP endpoint: {}", direct::format_endpoint({public_ip, game_port_}, text));
    } else if (endpoint_unavailable_.load(std::memory_order_acquire)) {
        output.line("Advertised UDP endpoint: {}", std::string_view{"Unavailable"});
    } else {
        output.line("Advertised UDP endpoint: {}", std::string_view{"Waiting for a GalaxyPeer address query"});
    }

    output.line("Association routes: Direct {}, GalaxyPeer {}, negotiating {}", direct_count_.load(std::memory_order_acquire),
                galaxy_count_.load(std::memory_order_acquire), negotiating_count_.load(std::memory_order_acquire));
}

bool Handler::read_identity(std::uint8_t slot, std::uint64_t &identity) const noexcept {
    return slot < endpoints_.size() && direct::read_native_identity(endpoints_[slot], identity);
}

std::int32_t Handler::resolve_primary(std::int32_t destination) const noexcept {
    if (destination < 0 || destination >= static_cast<std::int32_t>(direct::kNativeEndpointCount)) {
        return -1;
    }

    std::uint64_t identity{};
    return read_identity(static_cast<std::uint8_t>(destination), identity) ? find_identity(identity) : -1;
}

std::int32_t Handler::find_identity(std::uint64_t identity) const noexcept {
    if (identity == 0) {
        return -1;
    }

    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        std::uint64_t current{};
        if (read_identity(slot, current) && current == identity) {
            return slot;
        }
    }
    return -1;
}

std::int32_t Handler::find_connection(std::uint32_t connection_id) const noexcept {
    if (connection_id == 0) {
        return -1;
    }

    for (std::uint8_t slot = 0; slot < associations_.size(); ++slot) {
        if (associations_[slot].live && associations_[slot].connection_id == connection_id) {
            return slot;
        }
    }
    return -1;
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

void Handler::observe_public_endpoint(std::uint32_t network_order_ipv4) noexcept {
    if (!direct::is_public_ipv4(network_order_ipv4)) {
        observe_unusable_endpoint();
        return;
    }

    // Offers combine this IPv4 with our configured port. Passing this filter does not prove that socket is reachable.
    public_ipv4_.store(network_order_ipv4, std::memory_order_release);
    endpoint_unavailable_.store(false, std::memory_order_release);
}

void Handler::observe_unusable_endpoint() noexcept {
    // Only an owning default query reaches this withdrawal; remote-specific misses leave prior acquisition intact.
    public_ipv4_.store(0, std::memory_order_release);
    if (!endpoint_unavailable_.exchange(true, std::memory_order_acq_rel)) {
        const auto incoming = direct_transport::AmbientErrors::capture();
        logger_.warning("GalaxyPeer address query returned no usable public IPv4 address");
        incoming.restore();
    }
}

} // namespace fc_plugins::direct_transport::server
