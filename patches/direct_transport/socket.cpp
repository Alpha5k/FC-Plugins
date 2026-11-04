#include "socket.hpp"
#include "receive_diagnostics.hpp"

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace fc_plugins::direct_transport {
namespace {

constexpr std::int32_t kRequestedSendBuffer = 64 * 1024;
constexpr std::int32_t kRequestedReceiveBuffer = 32 * 1024;
constexpr std::int32_t kInvalidSocketOperation = 10022;

// Converts the owned handle representation to the Winsock handle type.
SOCKET to_socket(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

// Packs address octets into the host-order integer used to compare IPv4 prefixes.
consteval std::uint32_t pack_ipv4(std::array<std::uint8_t, 4> octets) {
    return (static_cast<std::uint32_t>(octets[0]) << 24) | (static_cast<std::uint32_t>(octets[1]) << 16) |
           (static_cast<std::uint32_t>(octets[2]) << 8) | octets[3];
}

// Builds the high-bit mask for an IPv4 prefix length from zero through 32.
consteval std::uint32_t prefix_mask(std::uint8_t length) {
    return length == 0 ? 0 : ~std::uint32_t{} << (32 - length);
}

class Ipv4Cidr final {
  public:
    // Stores the network bits and mask for one excluded address range.
    consteval Ipv4Cidr(std::array<std::uint8_t, 4> octets, std::uint8_t prefix_length)
        : mask_(prefix_mask(prefix_length)), network_(pack_ipv4(octets) & mask_) {}

    constexpr bool contains(std::uint32_t address) const noexcept {
        return (address & mask_) == network_;
    }

  private:
    std::uint32_t mask_;
    std::uint32_t network_;
};

// Reject private, loopback, link-local, documentation, benchmark, multicast, and reserved ranges.
constexpr std::array kNonPublicIpv4Ranges{
    Ipv4Cidr{{0, 0, 0, 0}, 8},      Ipv4Cidr{{10, 0, 0, 0}, 8},     Ipv4Cidr{{100, 64, 0, 0}, 10}, Ipv4Cidr{{127, 0, 0, 0}, 8},
    Ipv4Cidr{{169, 254, 0, 0}, 16}, Ipv4Cidr{{172, 16, 0, 0}, 12},  Ipv4Cidr{{192, 0, 0, 0}, 24},  Ipv4Cidr{{192, 0, 2, 0}, 24},
    Ipv4Cidr{{192, 88, 99, 0}, 24}, Ipv4Cidr{{192, 168, 0, 0}, 16}, Ipv4Cidr{{198, 18, 0, 0}, 15}, Ipv4Cidr{{198, 51, 100, 0}, 24},
    Ipv4Cidr{{203, 0, 113, 0}, 24}, Ipv4Cidr{{224, 0, 0, 0}, 3},
};

} // namespace

bool is_public_ipv4(std::uint32_t network_order) noexcept {
    const auto address = ntohl(network_order);
    return std::ranges::none_of(kNonPublicIpv4Ranges, [address](const auto &range) {
        return range.contains(address);
    });
}

std::string_view format_endpoint(const Endpoint &endpoint, std::span<char> output) noexcept {
    if (!endpoint.valid() || output.empty()) {
        return {};
    }
    const auto address = ntohl(endpoint.ipv4_network_order);
    const auto written = std::snprintf(output.data(), output.size(), "%u.%u.%u.%u:%u", address >> 24, (address >> 16) & 0xFF,
                                       (address >> 8) & 0xFF, address & 0xFF, endpoint.port_host_order);
    if (written <= 0) {
        output.front() = '\0';
        return {};
    }

    return {output.data(), std::min(static_cast<std::size_t>(written), output.size() - 1)};
}

UdpSocket::~UdpSocket() {
    stop();
}

bool UdpSocket::start(std::uint16_t port, std::int32_t &error, std::int32_t &buffer_error) noexcept {
    // Each owned socket balances one Winsock process reference, including every partial failure path.
    stop();
    configuration_read_ = false;
    configuration_ = {.requested_send_bytes = kRequestedSendBuffer, .requested_receive_bytes = kRequestedReceiveBuffer};
    error = 0;
    buffer_error = 0;
    WSADATA data{};
    error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        return false;
    }
    win_sock_started_ = true;
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
        error = WSAVERNOTSUPPORTED;
        stop();
        return false;
    }

    const auto socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        error = WSAGetLastError();
        stop();
        return false;
    }
    handle_ = static_cast<Handle>(socket_handle);

    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        error = WSAGetLastError();
        stop();
        return false;
    }

    // Retain the first buffer-tuning error, but allow the socket to use Windows' buffer sizes.
    if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&kRequestedSendBuffer),
                   sizeof(kRequestedSendBuffer)) == SOCKET_ERROR) {
        buffer_error = WSAGetLastError();
        configuration_.send_set_error = buffer_error;
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&kRequestedReceiveBuffer),
                   sizeof(kRequestedReceiveBuffer)) == SOCKET_ERROR) {
        configuration_.receive_set_error = WSAGetLastError();
        if (buffer_error == 0) {
            buffer_error = configuration_.receive_set_error;
        }
    }

    // Bind all local IPv4 interfaces; a zero port lets Windows choose the client's port.
    sockaddr_in requested{};
    requested.sin_family = AF_INET;
    requested.sin_addr.s_addr = htonl(INADDR_ANY);
    requested.sin_port = htons(port);
    if (bind(socket_handle, reinterpret_cast<const sockaddr *>(&requested), sizeof(requested)) == SOCKET_ERROR) {
        error = WSAGetLastError();
        stop();
        return false;
    }

    // Snapshot the actual local port after bind, including an OS-selected ephemeral port.
    sockaddr_in bound{};
    int bytes = sizeof(bound);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr *>(&bound), &bytes) == SOCKET_ERROR ||
        bytes < static_cast<int>(sizeof(bound)) || bound.sin_family != AF_INET) {
        error = WSAGetLastError();
        if (error == 0) {
            error = WSAEFAULT;
        }
        stop();
        return false;
    }

    bound_port_ = ntohs(bound.sin_port);
    if (bound_port_ == 0) {
        error = kInvalidSocketOperation;
        stop();
        return false;
    }
    configuration_.socket_id = receive_stamp();
    configuration_.bound_port = bound_port_;
    return true;
}

const network_diagnostics::DirectSocketObservation &UdpSocket::capture_configuration() noexcept {
    if (!available() || configuration_read_) {
        return configuration_;
    }
    configuration_read_ = true;
    const auto socket_handle = to_socket(handle_);
    // The native owner requests this once for Full diagnostics; later snapshots reuse the same lifetime.
    const auto errors = AmbientErrors::capture();
    int option_bytes = sizeof(configuration_.actual_send_bytes);
    if (getsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&configuration_.actual_send_bytes), &option_bytes) ==
        SOCKET_ERROR) {
        configuration_.send_read_error = WSAGetLastError();
    }
    option_bytes = sizeof(configuration_.actual_receive_bytes);
    if (getsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&configuration_.actual_receive_bytes), &option_bytes) ==
        SOCKET_ERROR) {
        configuration_.receive_read_error = WSAGetLastError();
    }
    errors.restore();

    return configuration_;
}

void UdpSocket::stop() noexcept {
    if (handle_ != kInvalidHandle) {
        closesocket(to_socket(handle_));
        handle_ = kInvalidHandle;
    }
    bound_port_ = 0;

    if (win_sock_started_) {
        WSACleanup();
        win_sock_started_ = false;
    }
}

bool UdpSocket::available() const noexcept {
    return win_sock_started_ && handle_ != kInvalidHandle && bound_port_ != 0;
}

std::uint16_t UdpSocket::bound_port() const noexcept {
    return bound_port_;
}

SocketResult UdpSocket::send(std::span<const std::uint8_t> bytes, const Endpoint &endpoint, bool measure) noexcept {
    if (!available() || bytes.empty() || !endpoint.valid()) {
        return {-1, kInvalidSocketOperation};
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {-1, WSAEMSGSIZE};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = endpoint.ipv4_network_order;
    address.sin_port = htons(endpoint.port_host_order);
    const auto entered = measure ? receive_stamp() : 0;
    const auto result = sendto(to_socket(handle_), reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                               reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    const auto error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    const auto returned = measure ? receive_stamp() : 0;
    return {result, error, entered, returned};
}

SocketResult UdpSocket::receive(std::span<std::uint8_t> bytes, Endpoint &endpoint, bool measure) noexcept {
    endpoint = {};
    if (!available() || bytes.empty()) {
        return {-1, kInvalidSocketOperation};
    }

    sockaddr_in address{};
    int address_bytes = sizeof(address);
    const auto entered = measure ? receive_stamp() : 0;
    const auto result = recvfrom(to_socket(handle_), reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
                                 reinterpret_cast<sockaddr *>(&address), &address_bytes);
    const auto error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    const auto returned = measure ? receive_stamp() : 0;
    if (result == SOCKET_ERROR) {
        return {-1, error, entered, returned};
    }
    if (address_bytes < static_cast<int>(sizeof(address)) || address.sin_family != AF_INET) {
        return {-1, WSAEFAULT, entered, returned};
    }

    endpoint = {.ipv4_network_order = address.sin_addr.s_addr, .port_host_order = ntohs(address.sin_port)};
    return {result, 0, entered, returned};
}

} // namespace fc_plugins::direct_transport
