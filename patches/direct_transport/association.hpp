#pragma once

#include "protocol.hpp"

#include <atomic>
#include <cstdint>

namespace fc_plugins::direct_transport {

// Records why a connection chose Galaxy or Direct, or failed to complete setup.
enum class RouteReason : std::uint8_t {
    None = 0,
    OfferTimeout = 1,
    ProofTimeout = 2,
    ReadyTimeout = 3,
    CommitTimeout = 4,
    SocketUnavailable = 5,
    EndpointUnavailable = 6,
    Commit = 7,
    ImplicitCommit = 8,
    RandomGenerationFailed = 9,
};

// Records why an association ended, independently of its chosen transport.
enum class AssociationEndReason : std::uint8_t {
    Disconnected = 1,
    Replaced = 2,
    PolicyRemoval = 3,
    Shutdown = 4,
};

// Identifies the check that rejected a datagram for a known association.
enum class RejectionKind : std::uint8_t {
    Endpoint,
    Authentication,
    Replay,
    Invalid,
};

// Allows setup to advance; once a connection chooses Galaxy or Direct, its route stays fixed.
constexpr bool valid_route_transition(RouteState from, RouteState to) noexcept {
    if (from == to) {
        return true;
    }
    switch (from) {
    case RouteState::Unclassified:
        return to == RouteState::Negotiating || to == RouteState::GalaxyLocked;
    case RouteState::Negotiating:
        return to == RouteState::AwaitCommit || to == RouteState::GalaxyLocked;
    case RouteState::AwaitCommit:
        return to == RouteState::DirectLocked || to == RouteState::GalaxyLocked;
    case RouteState::GalaxyLocked:
    case RouteState::DirectLocked:
        return false;
    }
    return false;
}

// A matching Commit remains valid after authenticated Direct data has already confirmed the same connection.
constexpr bool valid_commit(RouteState state, std::uint32_t connection_id, std::uint32_t committed_connection_id) noexcept {
    return connection_id != 0 && committed_connection_id == connection_id &&
           (state == RouteState::AwaitCommit || state == RouteState::DirectLocked);
}

// Holds counters and timestamps copied to diagnostics when an association starts, chooses a route or ends.
struct AssociationStats {
    // GetTickCount64 milliseconds; direct_started_ms remains zero until Direct is selected.
    std::uint64_t started_ms{};
    std::uint64_t direct_started_ms{};
    std::uint32_t tx_datagrams{};
    // Counts packets submitted to native intake, which can still reject them.
    std::uint32_t rx_datagrams{};
    std::uint32_t send_failures{};
    std::uint32_t endpoint_rejects{};
    std::uint32_t authentication_rejects{};
    std::uint32_t replay_rejects{};
    std::uint32_t invalid_rejects{};
    RouteReason route_reason{RouteReason::None};

    // Attributes one rejected datagram without logging on the network callback path.
    void reject(RejectionKind kind) noexcept {
        switch (kind) {
        case RejectionKind::Endpoint:
            ++endpoint_rejects;
            break;
        case RejectionKind::Authentication:
            ++authentication_rejects;
            break;
        case RejectionKind::Replay:
            ++replay_rejects;
            break;
        case RejectionKind::Invalid:
            ++invalid_rejects;
            break;
        }
    }
};

// Keeps the outer group's connection and transport across nested sends on the native owner thread.
class TransmitGroup final {
  public:
    // Nested begins keep the outermost connection and transport; depth overflow returns false.
    bool begin(std::uint8_t slot, std::uint64_t generation, Carrier carrier) noexcept;
    // Balances one successful begin; returns false when no group is active.
    bool end() noexcept;
    bool active() const noexcept;
    // Returns whether the active group belongs to this connection generation and endpoint slot.
    bool belongs_to(std::uint8_t slot, std::uint64_t generation) const noexcept;
    Carrier carrier() const noexcept;

  private:
    // These values remain unchanged until the last matching end.
    Carrier carrier_{Carrier::Galaxy};
    std::uint64_t generation_{};
    std::uint32_t depth_{};
    std::uint8_t slot_{};
};

// Records the first receive thread and rejects callers on other threads for the handler's lifetime.
class NetworkThread final {
  public:
    // The first receive callback establishes ownership; later callbacks may only match that thread.
    [[nodiscard]] bool claim() noexcept;
    // Accepts the current owner, or any caller before ownership is established; counts mismatched callers.
    [[nodiscard]] bool current() noexcept;
    std::uint64_t rejected() const noexcept;

  private:
    // Counts rejected calls when the established native owner differs from the caller.
    bool matches(std::uint32_t current, std::uint32_t owner) noexcept;

    std::atomic_uint32_t owner_{};
    std::atomic_uint64_t rejected_{};
};

// Reports cumulative counts at power-of-two thresholds to limit repeated logs.
class DiagnosticLogGate final {
  public:
    // Admits a count at the next threshold and advances that threshold beyond the reported count.
    bool admit(std::uint64_t count) noexcept;

  private:
    std::uint64_t next_{1};
};

} // namespace fc_plugins::direct_transport
