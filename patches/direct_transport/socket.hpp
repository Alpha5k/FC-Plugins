#pragma once

#include "security.hpp"
#include "../network_diagnostics/api.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string_view>

namespace fc_plugins::direct_transport {

// Stores IPv4 in Winsock network order and the UDP port in host order.
struct Endpoint {
    std::uint32_t ipv4_network_order{};
    std::uint16_t port_host_order{};

    constexpr bool valid() const noexcept {
        return ipv4_network_order != 0 && port_host_order != 0;
    }

    friend bool operator==(const Endpoint &, const Endpoint &) = default;
};

// Allows an initial source only when binding is enabled; an existing endpoint requires an exact match.
// The caller must authenticate a new source before storing it.
inline constexpr bool endpoint_admits_source(const Endpoint &expected, const Endpoint &source, bool allow_initial_bind) noexcept {
    return expected.valid() ? source == expected : allow_initial_bind;
}

// Rejects the listed nonpublic IPv4 ranges for Offer addresses; passing does not prove reachability.
bool is_public_ipv4(std::uint32_t network_order) noexcept;
// Formats an endpoint into caller-owned storage, truncating to fit; invalid endpoints or empty storage return an empty view.
std::string_view format_endpoint(const Endpoint &endpoint, std::span<char> output) noexcept;

struct SocketResult {
    std::int32_t value{-1};
    std::int32_t error{};
    std::uint64_t entered_qpc{};
    std::uint64_t returned_qpc{};

    bool succeeded() const noexcept {
        return value >= 0;
    }
};

inline constexpr std::uint32_t kReceiveAdmissionLimit = 80;
inline constexpr std::int32_t kSocketWouldBlock = 10035;
inline constexpr std::int32_t kSocketMessageTooLarge = 10040;

enum class DatagramDrainEnd : std::uint8_t {
    Empty,
    Budget,
    Error,
};

struct DatagramDrainResult {
    // Oversized datagrams discarded by Winsock within the receive attempt budget.
    std::uint32_t discarded{};
    std::int32_t error{};
    bool limit_reached{};
    DatagramDrainEnd reason{DatagramDrainEnd::Empty};
};

// Tries at most 80 receives and leaves queued excess datagrams for the next service.
// Oversized receives consume an attempt; would-block stops normally and other errors are returned.
template <typename Receiver, typename Handler> DatagramDrainResult drain_datagrams(Receiver &&receive, Handler &&handle) noexcept {
    DatagramDrainResult result;
    for (std::uint32_t received_datagrams = 0; received_datagrams < kReceiveAdmissionLimit; ++received_datagrams) {
        Endpoint source{};
        const auto received = std::invoke(receive, source);
        if (!received.succeeded()) {
            if (received.error == kSocketWouldBlock) {
                return result;
            }
            if (received.error != kSocketMessageTooLarge) {
                result.error = received.error;
                result.reason = DatagramDrainEnd::Error;
                return result;
            }
            ++result.discarded;
        }

        if (received.succeeded()) {
            std::invoke(handle, received.value, source);
        }
    }
    result.limit_reached = true;
    result.reason = DatagramDrainEnd::Budget;
    return result;
}

// Owns one nonblocking IPv4 UDP socket and the matching Winsock process reference.
class UdpSocket final {
  public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;
    // Closes the handle and balances this socket's Winsock reference.
    ~UdpSocket();

    // Binds port zero to an ephemeral port; failure stops the socket and reports error.
    // Buffer tuning is optional; buffer_error records its first failure even when startup succeeds.
    bool start(std::uint16_t port, std::int32_t &error, std::int32_t &buffer_error) noexcept;
    // Releases the socket and its matching Winsock reference.
    void stop() noexcept;
    bool available() const noexcept;
    std::uint16_t bound_port() const noexcept;

    const network_diagnostics::DirectSocketObservation &configuration() const noexcept {
        return configuration_;
    }

    // Reads and caches effective buffer sizes on the socket owner thread; startup set errors remain unchanged.
    const network_diagnostics::DirectSocketObservation &capture_configuration() noexcept;

    // Sends one datagram and preserves the corresponding Winsock error in the result.
    SocketResult send(std::span<const std::uint8_t> bytes, const Endpoint &endpoint, bool measure = false) noexcept;
    // Reads one datagram and copies its source into caller-owned storage; optional QPC stamps bracket recvfrom.
    SocketResult receive(std::span<std::uint8_t> bytes, Endpoint &endpoint, bool measure = false) noexcept;

  private:
    using Handle = std::uintptr_t;
    static constexpr Handle kInvalidHandle = (std::numeric_limits<Handle>::max)();

    Handle handle_{kInvalidHandle};
    std::uint16_t bound_port_{};
    bool win_sock_started_{};
    bool configuration_read_{};
    network_diagnostics::DirectSocketObservation configuration_{};
};

// Writes header, native payload and tag; returns the complete datagram length.
// Returns zero for invalid payload length or insufficient output space.
std::size_t write_data_datagram(std::span<std::uint8_t> output, Direction direction, const SessionKey &key, std::uint32_t connection_id,
                                std::uint32_t sequence, std::span<const std::uint8_t> payload) noexcept;
// Writes a maximum-size, zero-padded Probe or ProbeAck and its tag; excess output space is untouched.
bool write_probe_datagram(std::span<std::uint8_t> output, DirectKind kind, Direction direction, const SessionKey &key,
                          std::uint32_t connection_id, std::uint32_t sequence) noexcept;
// Requires a full datagram send and returns the native payload length, excluding the Direct header.
// Every result is handled, including failures, so callers must not retry through Galaxy.
NativeTransmitResult send_direct_data(UdpSocket &socket, std::span<std::uint8_t> output, Direction direction, const SessionKey &key,
                                      std::uint32_t connection_id, std::uint32_t sequence, const Endpoint &endpoint,
                                      std::span<const std::uint8_t> payload,
                                      network_diagnostics::DirectSendObservation *observation = nullptr) noexcept;

} // namespace fc_plugins::direct_transport
