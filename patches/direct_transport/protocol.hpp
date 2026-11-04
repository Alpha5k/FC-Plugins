#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fc_plugins::direct_transport {

// Setup retries use Windows milliseconds. The UDP size limit fits the native large packet pool.
inline constexpr std::string_view kFeatureId = "FC.DirectTransport";
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint64_t kHandshakeRetryIntervalMs = 1'000;
inline constexpr std::uint64_t kHandshakeDeadlineMs = 5'000;
inline constexpr std::uint8_t kHandshakeAttemptLimit = 3;
inline constexpr std::size_t kNativeHeaderBytes = 5;
inline constexpr std::size_t kMaximumNativeBytes = 1012;
inline constexpr std::size_t kOfferBytes = 27;
inline constexpr std::size_t kActivationBytes = 5;
inline constexpr std::size_t kDirectHeaderBytes = 24;
inline constexpr std::size_t kProbePayloadBytes = kMaximumNativeBytes;
inline constexpr std::size_t kProbeDatagramBytes = kDirectHeaderBytes + kProbePayloadBytes;
inline constexpr std::size_t kMaximumDirectDatagramBytes = kDirectHeaderBytes + kMaximumNativeBytes;

// Controls admission and the outcome of negotiation failure on a dedicated server.
enum class Policy : std::uint8_t {
    Disabled = 1,
    PreferDirect = 2,
    RequireDirectPatched = 3,
    RequireDirectAll = 4,
};

// Unsupported peers stay native except under RequireDirectAll; compatible negotiation failures follow this decision.
enum class PolicyAction : std::uint8_t {
    Native,
    Negotiate,
    Remove,
};

// Decides whether a peer stays native, negotiates Direct or must leave under the server's policy.
constexpr PolicyAction policy_action(Policy policy, bool compatible, bool failed = false) noexcept {
    if (policy == Policy::Disabled) {
        return PolicyAction::Native;
    }
    if (!compatible) {
        return policy == Policy::RequireDirectAll ? PolicyAction::Remove : PolicyAction::Native;
    }
    if (!failed) {
        return PolicyAction::Negotiate;
    }
    return policy == Policy::PreferDirect ? PolicyAction::Native : PolicyAction::Remove;
}

// A route becomes terminal only after choosing GalaxyPeer or committing authenticated UDP.
enum class RouteState : std::uint8_t {
    Unclassified = 1,
    Negotiating = 2,
    AwaitCommit = 3,
    GalaxyLocked = 4,
    DirectLocked = 5,
};

enum class Carrier : std::uint8_t {
    Galaxy = 1,
    Direct = 2,
};

enum class Direction : std::uint8_t {
    ClientToServer = 1,
    ServerToClient = 2,
};

// DirectPending waits for the next outer packet group or an ungrouped send to apply Direct.
enum class TransmitRoute : std::uint8_t {
    Galaxy = 1,
    DirectPending = 2,
    Direct = 3,
};

enum class ControlKind : std::uint8_t {
    Offer = 1,
    Ready = 2,
    Commit = 3,
};

enum class DirectKind : std::uint8_t {
    Data = 1,
    Probe = 2,
    ProbeAck = 3,
};

// Reports whether Direct Transport replaced the game's send and which result the hook must return.
struct NativeTransmitResult {
    // Native payload byte count on success, or -1 with a Winsock error on failure.
    std::int32_t result{};
    std::int32_t error{};
    // True prevents the hook from trying the original native sender, including after failure.
    bool handled{};
};

enum class ParseStatus : std::uint8_t {
    Ok,
    TooShort,
    BadMagic,
    BadLength,
    UnsupportedVersion,
    UnknownKind,
    InvalidPayload,
};

// FC frames setup messages separately. Packed integer arrays below use big-endian wire bytes.
#pragma pack(push, 1)

struct OfferMessage {
    std::uint8_t kind;
    std::array<std::uint8_t, 4> connection_id;
    // IPv4 octets are copied directly to and from Winsock's network-order address.
    std::array<std::uint8_t, 4> server_ipv4;
    std::array<std::uint8_t, 2> server_port;
    std::array<std::uint8_t, 16> session_key;
};

struct ActivationMessage {
    std::uint8_t kind;
    std::array<std::uint8_t, 4> connection_id;
};

struct DirectHeaderV1 {
    // BF2D identifies the UDP protocol; version 1 uses a 24-byte header.
    std::array<std::uint8_t, 4> magic;
    std::uint8_t version;
    std::uint8_t kind;
    std::array<std::uint8_t, 2> header_bytes;
    std::array<std::uint8_t, 4> connection_id;
    // Data and proof packets share a sequence stream in each direction.
    std::array<std::uint8_t, 4> datagram_sequence;
    // SipHash-2-4 covers direction, this header with a zero tag, and the full payload.
    std::array<std::uint8_t, 8> authentication_tag;
};

#pragma pack(pop)

static_assert(sizeof(OfferMessage) == kOfferBytes);
static_assert(sizeof(ActivationMessage) == kActivationBytes);
static_assert(sizeof(DirectHeaderV1) == kDirectHeaderBytes);

// Parsed records expose host-order values while retaining IPv4 in the Winsock network-order form.
struct ParsedControl {
    ControlKind kind{};
    std::uint32_t connection_id{};
    std::uint32_t server_ipv4_network_order{};
    // Host-order UDP port from Offer; Ready and Commit leave these Offer fields zero.
    std::uint16_t server_port{};
    std::array<std::uint8_t, 16> session_key{};
};

struct ParsedDirect {
    DirectKind kind{};
    std::uint32_t connection_id{};
    std::uint32_t datagram_sequence{};
    std::uint64_t authentication_tag{};
    // Borrows the parser's input; valid only while that input remains alive and unchanged.
    std::span<const std::uint8_t> payload{};
};

// Loads unaligned big-endian wire integers into host-order values.
std::uint16_t load_big_endian16(const void *bytes) noexcept;
std::uint32_t load_big_endian32(const void *bytes) noexcept;
std::uint64_t load_big_endian64(const void *bytes) noexcept;
// Stores host-order integers as unaligned big-endian wire bytes.
void store_big_endian16(void *bytes, std::uint16_t value) noexcept;
void store_big_endian32(void *bytes, std::uint32_t value) noexcept;
void store_big_endian64(void *bytes, std::uint64_t value) noexcept;

// Returns the exact payload size for a setup kind, or zero for an unknown kind.
std::size_t control_bytes(ControlKind kind) noexcept;
// Copies an exact-size setup payload into output; the caller validates its state and field values.
ParseStatus parse_control(std::span<const std::uint8_t> bytes, ParsedControl &output) noexcept;
// Writes an Offer into storage of exactly kOfferBytes; does not validate endpoint reachability.
bool write_offer(std::span<std::uint8_t> output, std::uint32_t connection_id, std::uint32_t server_ipv4_network_order,
                 std::uint16_t server_port, std::span<const std::uint8_t, 16> session_key) noexcept;
// Writes Ready or Commit into storage of exactly kActivationBytes.
bool write_activation(std::span<std::uint8_t> output, ControlKind kind, std::uint32_t connection_id) noexcept;

// Checks header, payload length and proof padding; source, tag and replay checks belong to the handler.
ParseStatus parse_direct(std::span<const std::uint8_t> bytes, ParsedDirect &output) noexcept;
// Writes the common UDP header; the caller supplies payload bytes and computes their authentication tag.
bool write_direct_header(std::span<std::uint8_t> output, DirectKind kind, std::uint32_t connection_id, std::uint32_t datagram_sequence,
                         std::uint64_t authentication_tag) noexcept;

} // namespace fc_plugins::direct_transport
