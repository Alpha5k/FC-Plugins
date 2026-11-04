#pragma once

#include "../../network_diagnostics/api.hpp"
#include "../association.hpp"
#include "../game.hpp"
#include "../socket.hpp"
#include "layout.hpp"
#include "groups.hpp"
#include "galaxy_peer.hpp"
#include "../network.hpp"

#include <fc/native.hpp>
#include "../control.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

namespace fc_plugins::direct_transport::server {

namespace direct = direct_transport;
inline constexpr std::size_t kPhysicalAssociationCount = 64;

struct Settings {
    direct::Policy policy{direct::Policy::Disabled};
};

// Negotiates authenticated UDP for dedicated Galaxy clients and applies the configured admission policy.
class Handler final {
  public:
    using Settings = server::Settings;

    // Initializes the configured policy and staged removals.
    Handler(const fc::CreateContext &context, const Settings &settings) noexcept;
    // Retires all player associations and releases the owned UDP socket.
    ~Handler();

    // Declares game packet/send/removal operations and observes GalaxyPeer's existing address queries.
    void plan(fc::Plan &plan);
    // Resolves native fields, registers Galaxy controls and binds the Direct socket to the configured game port.
    fc::Result<> prepare(fc::PrepareContext &context);
    // Reports cumulative native counters on the serialized control thread.
    void update(const fc::UpdateContext &) noexcept;
    // Reports the latest transport state without reading native-owned associations.
    void write_status(fc::StatusContext &output) const noexcept;

  private:
    using DiagnosticsApi = network_diagnostics::NetworkDiagnosticsApi;

    enum class RemovalReason : std::uint8_t {
        DirectRequired,
        NegotiationFailed,
    };

    enum class FailureAction : std::uint8_t {
        GalaxyFallback,
        NegotiationRemoval,
        UnsupportedRemoval,
    };

    enum class DataFailureKind : std::uint8_t {
        Send,
        PacketFactoryUnavailable,
        PacketPrefixInvalid,
        PacketAllocationFailed,
        Count,
    };

    // Tracks one physical slot and FC connection. Probe fixes its UDP source; Ready then permits a Commit submission.
    // DirectLocked admits Data after local Commit acceptance; output switches outside an active synchronous group.
    struct Association {
        direct_transport::Peer peer{};
        // Internal connection generation distinguishes replacements while a native group is still sending.
        std::uint64_t generation{};
        // GetTickCount64 milliseconds, independent of the game's floating-point boot timer.
        std::uint64_t phase_start_ms{};
        std::uint64_t proof_deadline_ms{};
        // Random nonzero wire ID, unique among live associations; FC PeerId is a separate connection identity.
        std::uint32_t connection_id{};
        std::uint32_t send_sequence{};
        direct::SessionKey session_key{};
        // First authenticated Probe source; Data must keep this exact IPv4 and port after commitment.
        direct::Endpoint client_endpoint{};
        direct::ReplayWindow receive_replay;
        direct::AssociationStats stats;
        direct::RouteState state{direct::RouteState::Unclassified};
        direct::TransmitRoute transmit_route{direct::TransmitRoute::Galaxy};
        std::uint8_t offer_attempts{};
        std::uint8_t commit_attempts{};
        bool live{};
        bool compatible{};
        // FC copied the Offer into its outgoing queue; this does not establish client receipt.
        bool offer_submitted{};
    };

    // Keeps admission refusal and the staged native boot on the failed connection and its primary owner.
    struct Removal {
        direct::Peer peer{};
        std::uint64_t generation{};
        std::uint64_t primary_identity{};
        std::int32_t primary{-1};
        // UpdateBootPlayer can change this target to primary, then -1 after native disconnection.
        std::int32_t player{-1};
    };

    // Copies control-thread peer and message publications at the native receive boundary.
    void synchronize_control() noexcept;
    // Withdraws unavailable peers; otherwise copies the FC peer, native slot, Galaxy user ID and agreement to the mailbox.
    void peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept;
    // Parses setup messages and copies their fields before the SDK releases the message.
    void control_received(fc::PeerId peer, fc::Bytes bytes) noexcept;

    // Synchronizes control and pumps direct datagrams on the native receive thread.
    void before_receive() noexcept;
    // Advances negotiation and removals after ordinary native receive service.
    void after_receive() noexcept;
    // Pins a live physical association and carrier for this synchronous group; returns its primary slot or -1.
    std::int32_t begin_group(std::int32_t destination) noexcept;
    // Balances a successful begin without changing an outer invocation's carrier.
    void end_group(std::int32_t destination, std::int32_t primary) noexcept;
    // Uses an active packet group's carrier or the current route; Direct errors never retry through Galaxy.
    direct_transport::NativeTransmitResult send(std::int32_t destination, const std::uint8_t *bytes, std::uint32_t size) noexcept;

    // Returns the native or direct send result and preserves ambient OS errors around diagnostics.
    std::int32_t final_send(std::int32_t destination, const void *bytes, std::int32_t length) noexcept;
    // Transfers a pool packet with its copied native source address; RecvPacket may queue or free it.
    void submit_native(void *packet, direct::NativeAddress address) noexcept;

    // Publishes optional diagnostics before enabling readers on the native thread.
    void connect_diagnostics(DiagnosticsApi diagnostics) noexcept;
    const DiagnosticsApi *diagnostics() const noexcept;

    // Starts a compatible FC connection when its native slot/user agrees and this connection is not being removed.
    void peer_available(direct_transport::Peer peer) noexcept;
    // Ends only the matching complete peer identity on the native owner thread.
    void peer_unavailable(direct_transport::Peer peer) noexcept;
    // Requires a compatible live association with the same full FC and Galaxy identities in this slot.
    bool current_peer(direct_transport::Peer peer) const noexcept;
    // Finds unsupported native clients when RequireDirectAll needs to enforce admission.
    void scan_players(std::uint64_t now) noexcept;

    // Reads a connected native destination's nonzero Galaxy identity, including routes outside physical slots 0–63.
    bool read_identity(std::uint8_t slot, std::uint64_t &identity) const noexcept;
    // Resolves any native destination to the first connected physical slot with the same nonzero user ID.
    std::int32_t resolve_primary(std::int32_t destination) const noexcept;
    // Finds the first connected physical native slot with this full Galaxy user ID, even without an association.
    std::int32_t find_identity(std::uint64_t identity) const noexcept;
    // Finds a live association by its random Direct connection ID.
    std::int32_t find_connection(std::uint32_t connection_id) const noexcept;

    // Creates a fresh internal generation while retaining any outer native transmit group.
    void start_association(direct_transport::Peer peer, bool compatible, std::uint64_t now) noexcept;
    // Retires the association while retaining any still-active native transmit group.
    void end_association(std::uint8_t slot, direct::AssociationEndReason reason) noexcept;
    // Retires every live association when the session ends or the handler shuts down.
    void end_all(direct::AssociationEndReason reason) noexcept;
    // Copies the association counters into an optional lifecycle observation.
    void observe_association(std::uint8_t slot, network_diagnostics::DirectAssociationPhase phase, std::uint64_t now,
                             direct::AssociationEndReason end_reason, std::uint32_t attempts = 0) noexcept;

    // Drains bounded socket work and admits payloads through the authenticated parser.
    void pump_direct() noexcept;
    // Applies a setup message only to the already-validated complete native association.
    void handle_control(std::uint8_t slot, const direct::ParsedControl &control, std::uint64_t now) noexcept;
    // Validates endpoint, authentication, replay and route before transferring native payloads.
    void handle_direct(std::span<const std::uint8_t> bytes, const direct::Endpoint &source,
                       network_diagnostics::DirectDatagramObservation *datagram) noexcept;
    // Advances server admission, setup deadlines, retries and staged native removals.
    void service_associations(std::uint64_t now) noexcept;
    // Creates a random connection ID and key; Offer submission waits for an observed public IPv4 address.
    void begin_offer(std::uint8_t slot, std::uint64_t now) noexcept;
    // Reports local control submission; Accepted does not imply a remote acknowledgement.
    bool send_control(std::uint8_t slot, std::span<const std::uint8_t> bytes) noexcept;
    // Sends a full-size authenticated ProbeAck to the admitted client endpoint.
    void send_probe_ack(std::uint8_t slot, const direct::Endpoint &destination) noexcept;
    // Applies the configured negotiation failure policy before carrier commitment.
    void direct_failure(std::uint8_t slot, direct::RouteReason reason) noexcept;
    // Logs whether this negotiation failure keeps Galaxy or schedules native removal.
    void log_failure(std::uint8_t slot, direct::RouteReason reason, FailureAction action) noexcept;
    // Counts Direct send/reconstruction failures and logs at cumulative power-of-two thresholds.
    void log_data_failure(std::uint8_t slot, DataFailureKind kind, std::int32_t system_error = 0) noexcept;
    // Applies legal route transitions; terminal carriers remain fixed for this association.
    void set_route(std::uint8_t slot, direct::RouteState state, direct::TransmitRoute route,
                   direct::RouteReason reason = direct::RouteReason::None) noexcept;
    // Publishes cumulative socket and drain failures for later control-thread reporting.
    void record_drain(direct::DatagramDrainResult result) noexcept;
    // Copies native-owned route counts into atomic status fields.
    void publish_route_counts() noexcept;

    // Advances the game's staged removals on successive native receive services.
    void service_removals() noexcept;
    // Checks the failed FC/native connection and the guest's unchanged native primary owner.
    bool current_removal(const Removal &removal) const noexcept;
    // Retires associations and staged boots when native Disconnect ends either recorded endpoint.
    void native_disconnect(std::int32_t slot) noexcept;
    // Blocks the current connection and schedules the game's ordinary player removal.
    void remove_peer(std::uint8_t slot, RemovalReason reason) noexcept;
    // Requires a live committed route, a proved endpoint and an available UDP socket.
    bool direct_route_ready(std::uint8_t slot) const noexcept;
    // Authenticates one native fragment for the committed endpoint and reports failure thresholds through FC.
    direct::NativeTransmitResult send_direct_packet(std::uint8_t slot, std::span<const std::uint8_t> bytes) noexcept;
    // Accepts native queries only from the game GalaxyRootPeer's raw peer; remote failures do not withdraw its cache.
    void observe_galaxy_peer(void *raw_peer, GalaxyAddress observer, GalaxyAddress *result) noexcept;
    // Ends the cached address lifetime before its owning raw peer is destroyed.
    void destroy_galaxy_peer(void *raw_peer) noexcept;
    // Stores an IPv4 address accepted by the public-address filter for future Offers.
    void observe_public_endpoint(std::uint32_t network_order_ipv4) noexcept;
    // Clears the cached address and warns on the first unavailable default query from its owning peer.
    void observe_unusable_endpoint() noexcept;

    Settings settings_;
    fc::NativeFunction<direct_transport::FinalSend> original_send_;
    fc::Logger logger_;
    std::array<direct::NativeEndpoint, direct::kNativeEndpointCount> endpoints_{};
    direct::NativePacketFactory packet_factory_;
    fc::Memory<const std::uint32_t> prefix_memory_;
    fc::NativeFunction<direct_transport::PacketIntake> packet_intake_;
    fc::Memory<const std::uint32_t> port_memory_;
    fc::Memory<const std::byte> vtable_memory_;
    fc::Memory<const std::byte> owner_vtable_memory_;
    fc::Memory<const std::byte> owner_callback_memory_;
    fc::Memory<const GalaxyAddress> unassigned_memory_;
    // Identifies the raw RakPeer implementation, not a particular instance or current session.
    const std::byte *raw_peer_vtable_{};
    const std::byte *owner_peer_vtable_{};
    const std::byte *owner_callback_{};
    const GalaxyAddress *unassigned_address_{};
    std::size_t owner_callback_offset_{};
    std::size_t owner_context_offset_{};
    std::array<fc::Memory<const std::byte>, direct::kNativeEndpointCount> connected_memory_;
    std::array<fc::Memory<const std::uint64_t>, direct::kNativeEndpointCount> identity_memory_;
    fc::NativeFunction<IsPlaying> is_playing_;
    fc::NativeFunction<UpdateBootPlayer> update_boot_player_;
    fc::NativeFunction<SetNotPlaying> set_not_playing_;
    fc::NativeFunction<GetGuestOwner> get_guest_owner_;
    fc::Game game_;
    fc::SessionId session_{};
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    direct_transport::ControlMailbox mailbox_;
    std::uint64_t next_generation_{};
    bool diagnostics_full_{};
    DiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};

    std::array<Association, kPhysicalAssociationCount> associations_{};
    // Native invocation groups survive retirement or replacement of their physical association.
    std::array<direct::TransmitGroup, kPhysicalAssociationCount> transmit_groups_;
    GroupDestinations group_destinations_;
    std::array<Removal, kPhysicalAssociationCount> removals_{};
    direct::NetworkThread network_thread_;
    direct::UdpSocket socket_;
    // Diagnostic QPC entry state belongs to this socket's native owner, including nested pumps.
    std::uint64_t last_direct_service_qpc_{};
    std::uint32_t direct_pump_id_{};
    bool socket_observed_{};
    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> receive_buffer_{};
    std::array<std::uint8_t, direct::kMaximumDirectDatagramBytes> send_buffer_{};
    // Snapshot of /gameport used by our socket, independent of the port returned by GalaxyPeer.
    std::uint16_t game_port_{};
    // Map rotation can retain the native raw peer; its destruction or replacement retires this cached address.
    std::atomic<void *> public_address_peer_{};
    std::atomic_uint32_t public_ipv4_{};
    std::atomic_bool endpoint_unavailable_{};
    // Counts live associations only; unsupported native clients may have no association under permissive policies.
    std::atomic_uint16_t direct_count_{};
    std::atomic_uint16_t galaxy_count_{};
    std::atomic_uint16_t negotiating_count_{};
    // Counts and gates have the same single native owner as the association state.
    std::array<std::uint64_t, static_cast<std::size_t>(DataFailureKind::Count)> data_failure_counts_{};
    std::array<direct::DiagnosticLogGate, static_cast<std::size_t>(DataFailureKind::Count)> data_failure_logs_{};
    std::atomic_uint64_t receive_errors_{};
    std::atomic_uint64_t drain_limits_{};
    std::atomic_int32_t latest_receive_error_{};
    direct::DiagnosticLogGate thread_log_;
    direct::DiagnosticLogGate receive_error_log_;
    direct::DiagnosticLogGate drain_limit_log_;
};

} // namespace fc_plugins::direct_transport::server
