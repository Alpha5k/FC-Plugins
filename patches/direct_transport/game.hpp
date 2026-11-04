#pragma once

#include "protocol.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fc_plugins::direct_transport {

// NetPacket::Alloc takes the small-pool choice in CL and returns the pool packet in EAX.
using AllocateNativePacket = void *(__fastcall *)(bool small) noexcept;
// The selected retail entry corresponds to Phantom NetPacket::BeginWrite, with the packet in ECX.
using InitializeNativePacket = void(__fastcall *)(void *packet) noexcept;

inline constexpr std::size_t kNativeEndpointCount = 67;
inline constexpr std::size_t kNativeEndpointStride = 0x60;
inline constexpr std::size_t kNativeEndpointConnectedOffset = 0x20;

// Native intake reads a 24-byte address: the raw identity followed by 16 opaque bytes.
using NativeAddress = std::array<std::uint64_t, 3>;

struct NativeEndpointLocation {
    fc::DataLocation<std::byte> connected;
    fc::DataLocation<std::uint64_t> identity;
};

struct NativeEndpoint {
    // Borrowed views into a native 0x60-byte endpoint record; zero means disconnected.
    const std::byte *connected{};
    // Borrows all 24 address bytes; the first word is a hosted user ID or the joined client's lobby ID.
    const std::uint64_t *identity{};
};

// Locates endpoint identities and the game resources used to rebuild incoming packets.
struct GameLayout {
    std::array<NativeEndpointLocation, kNativeEndpointCount> endpoints;
    fc::FunctionLocation<AllocateNativePacket> allocate_packet;
    fc::FunctionLocation<InitializeNativePacket> initialize_packet;
    fc::DataLocation<std::uint32_t> packet_prefix;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<GameLayout> select_game_layout(std::string_view profile);

struct NativePacketFactory {
    fc::NativeFunction<AllocateNativePacket> allocate;
    fc::NativeFunction<InitializeNativePacket> initialize;
    // Borrowed runtime count of bytes before the communication header; low packet types need 3.
    const std::uint32_t *prefix{};
};

enum class NativePacketResult : std::uint8_t {
    Built,
    RejectedPayload,
    FactoryUnavailable,
    InvalidPrefix,
    AllocationFailed,
};

// Reads only connected endpoints and rejects absent native identities.
bool read_native_identity(const NativeEndpoint &endpoint, std::uint64_t &identity) noexcept;

// Copies a connected endpoint's complete address before native calls; failure clears the output.
bool read_native_address(const NativeEndpoint &endpoint, NativeAddress &address) noexcept;

// Rebuilds a serialized payload in a native pool packet. Built transfers ownership to the caller,
// which must submit it to native intake; allocation can flush the game's queues under pool pressure.
NativePacketResult build_native_packet(const NativePacketFactory &factory, std::span<const std::uint8_t> bytes, void *&packet) noexcept;

} // namespace fc_plugins::direct_transport
