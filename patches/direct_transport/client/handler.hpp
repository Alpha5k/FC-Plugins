#pragma once

#include "../../network_diagnostics/api.hpp"
#include "../association.hpp"
#include "../game.hpp"
#include "../socket.hpp"
#include "../network.hpp"

#include <fc/native.hpp>
#include "../control.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace fc_plugins::direct_transport::client {

// Negotiates authenticated UDP with a compatible dedicated server while Galaxy carries setup messages.
class Handler final {
  public:
    explicit Handler(const fc::CreateContext &context) noexcept;
    // Retires the server association and releases the owned UDP socket.
    ~Handler();

    // Declares native packet calls, endpoint reads, send hooks and receive observers.
    void plan(fc::Plan &plan);
    // Gets native read views, registers FC callbacks and opens the client UDP socket.
    fc::Result<> prepare(fc::PrepareContext &context);
    // Reports cumulative native errors on the serialized control thread.
    void update(const fc::UpdateContext &) noexcept;
    // Reports the latest transport state without reading native-owned associations.
    void write_status(fc::StatusContext &output) const noexcept;

  private:
    using DiagnosticsApi = network_diagnostics::NetworkDiagnosticsApi;
    using DirectAssociationPhase = network_diagnostics::DirectAssociationPhase;

    // Holds the current FC server connection, its UDP proof and its selected route.
    struct Association {
        // Slot 64 is the host endpoint; galaxy_id is its resolved Galaxy user ID.
        direct_transport::Peer peer{};
        // The native host route stores the lobby ID, separately from the resolved owner user ID.
        std::uint64_t lobby_id{};
        // Changes on replacement so an active packet group can detect an old connection.
        std::uint64_t generation{};
        // GetTickCount64 milliseconds for the current control phase and the UDP proof.
        std::uint64_t phase_start_ms{};
        std::uint64_t proof_start_ms{};
        // Server-chosen wire ID, distinct from the FC PeerId and native endpoint slot.
        std::uint32_t connection_id{};
        // Probe and Data share this outgoing sequence, including unsuccessful socket sends.
        std::uint32_t send_sequence{};
        direct_transport::SessionKey session_key{};
        // Offered IPv4 address and port; every received datagram must match both.
        direct_transport::Endpoint server_endpoint{};
        direct_transport::ReplayWindow receive_replay;
        direct_transport::AssociationStats stats;
        direct_transport::RouteState state{direct_transport::RouteState::Unclassified};
        direct_transport::TransmitRoute transmit_route{direct_transport::TransmitRoute::Galaxy};
        std::uint8_t probe_attempts{};
        std::uint8_t ready_attempts{};
        bool live{};
        // Enables Data after an authenticated ProbeAck, while output may still use Galaxy.
        bool receive_permission{};
    };

    // Copies control-thread peer and message publications at the native receive boundary.
    void synchronize_control() noexcept;
    // Copies FC peer changes from the serialized Multiplayer callback.
    void peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept;
    // Parses and copies setup controls before the SDK releases the borrowed message bytes.
    void control_received(fc::PeerId peer, fc::Bytes bytes) noexcept;

    // Synchronizes control and pumps direct datagrams on the native receive thread.
    void before_receive() noexcept;
    // Advances negotiation after ordinary native receive service.
    void after_receive() noexcept;
    // Stores the group's connection and transport; remaining fragments fail after Client mode ends.
    // Returns whether this invocation requires a matching end.
    bool begin_group(std::int32_t destination) noexcept;
    // Balances a successful begin without changing an outer invocation's carrier.
    void end_group(std::int32_t destination) noexcept;
    // Uses the active group's transport or the current route; Direct failures remain handled.
    direct_transport::NativeTransmitResult send(std::int32_t destination, const std::uint8_t *bytes, std::uint32_t size) noexcept;

    // Returns the native or direct send result and preserves ambient OS errors around diagnostics.
    std::int32_t final_send(std::int32_t destination, const void *bytes, std::int32_t length) noexcept;
    // Transfers the rebuilt pool packet with its copied native source address; intake may queue or free it.
    void submit_native(void *packet, direct_transport::NativeAddress address) noexcept;

    // Publishes optional diagnostics before enabling readers on the native thread.
    void connect_diagnostics(DiagnosticsApi diagnostics) noexcept;
    const DiagnosticsApi *diagnostics() const noexcept;

    // Starts a new association for a compatible host, replacing any previous FC connection.
    void peer_available(direct_transport::Peer peer) noexcept;
    // Ends only the matching complete peer identity on the native owner thread.
    void peer_unavailable(direct_transport::Peer peer) noexcept;
    // Requires a live association with the same full FC, native-slot and Galaxy identities.
    bool current_peer(direct_transport::Peer peer) const noexcept;

    // Starts a fresh connection generation while preserving any active packet group.
    void start_association(direct_transport::Peer peer, std::uint64_t now) noexcept;
    // Ends the association while preserving any active packet group's connection and transport.
    void end_association(direct_transport::AssociationEndReason reason) noexcept;
    // Copies the association counters into an optional lifecycle observation.
    void observe_association(DirectAssociationPhase phase, std::uint64_t now, direct_transport::AssociationEndReason end_reason,
                             std::uint32_t attempts = 0) noexcept;

    // Tries at most 80 socket receives and leaves excess datagrams queued for the next service.
    void pump_direct(std::uint64_t now) noexcept;
    // Advances client setup deadlines and bounded retries without changing terminal routes.
    void service_association(std::uint64_t now) noexcept;
    // Accepts an Offer while unclassified or a matching Commit after UDP proof, preserving terminal carriers.
    void handle_control(const direct_transport::ParsedControl &control, std::uint64_t now) noexcept;
    // Checks source, route, tag and sequence before rebuilding Data in the native packet pool.
    void handle_direct(std::span<const std::uint8_t> bytes, const direct_transport::Endpoint &source, std::uint64_t now,
                       network_diagnostics::DirectDatagramObservation *datagram) noexcept;
    // Reports local control submission; Accepted does not imply a remote acknowledgement.
    bool send_control(std::span<const std::uint8_t> bytes) noexcept;
    // Sends the padded authenticated client proof and counts one bounded attempt.
    void send_probe() noexcept;
    // Logs once when this connection selects Galaxy or Direct.
    void log_route(direct_transport::RouteState state, direct_transport::RouteReason reason) noexcept;
    // Applies legal route transitions; terminal carriers remain fixed for this association.
    void set_route(direct_transport::RouteState state, direct_transport::TransmitRoute route, bool receive_permission,
                   direct_transport::RouteReason reason = direct_transport::RouteReason::None) noexcept;
    // Publishes cumulative socket and drain failures for later control-thread reporting.
    void record_drain(direct_transport::DatagramDrainResult result) noexcept;
    // Copies the route and endpoint into separate atomic status values.
    void publish_status() noexcept;

    fc::NativeFunction<direct_transport::FinalSend> original_send_;
    fc::Logger logger_;
    direct_transport::NativePacketFactory packet_factory_;
    fc::Memory<const std::uint32_t> prefix_memory_;
    fc::Memory<const std::byte> host_connected_memory_;
    fc::Memory<const std::uint64_t> host_identity_memory_;
    // Borrowed native slot-64 fields; FC's installed memory resources keep the views valid.
    direct_transport::NativeEndpoint host_endpoint_;
    fc::NativeFunction<direct_transport::PacketIntake> packet_intake_;
    fc::Game game_;
    fc::SessionId session_{};
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    // Copies FC callbacks from the control thread for consumption by the native owner.
    direct_transport::ControlMailbox mailbox_;
    std::uint64_t next_generation_{};
    bool diagnostics_full_{};
    DiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};

    // Mutable connection, group and buffers are used by the native owner thread.
    Association association_;
    direct_transport::TransmitGroup transmit_group_;
    direct_transport::NetworkThread network_thread_;
    // The socket survives map and session changes; destruction releases it.
    direct_transport::UdpSocket socket_;
    // Diagnostic QPC entry state belongs to this socket's native owner, including nested pumps.
    std::uint64_t last_direct_service_qpc_{};
    std::uint32_t direct_pump_id_{};
    bool socket_observed_{};
    std::array<std::uint8_t, direct_transport::kMaximumDirectDatagramBytes> receive_buffer_{};
    std::array<std::uint8_t, direct_transport::kMaximumDirectDatagramBytes> send_buffer_{};

    std::atomic<direct_transport::RouteState> published_route_{direct_transport::RouteState::Unclassified};
    // IPv4 in the low 32 bits and host-order port in bits 32–47.
    std::atomic_uint64_t published_endpoint_{};
    // Local port; zero reports that socket startup failed.
    std::atomic_uint16_t published_port_{};
    std::atomic_uint64_t receive_errors_{};
    std::atomic_uint64_t drain_limits_{};
    std::atomic_uint64_t packet_prefix_failures_{};
    std::atomic_uint64_t packet_allocation_failures_{};
    std::atomic_uint64_t latest_prefix_failure_peer_{};
    std::atomic_uint64_t latest_allocation_failure_peer_{};
    std::atomic_int32_t latest_receive_error_{};
    direct_transport::DiagnosticLogGate thread_log_;
    direct_transport::DiagnosticLogGate receive_error_log_;
    direct_transport::DiagnosticLogGate drain_limit_log_;
    direct_transport::DiagnosticLogGate packet_prefix_log_;
    direct_transport::DiagnosticLogGate packet_allocation_log_;
};

} // namespace fc_plugins::direct_transport::client
