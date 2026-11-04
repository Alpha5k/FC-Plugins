#pragma once

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::direct_transport::server {

// GalaxyPeer's SystemAddress is passed by value and copied into caller-owned output storage.
#pragma pack(push, 1)

struct GalaxyAddress {
    std::uint16_t family;
    std::uint16_t port_network_order;
    std::uint32_t address_network_order;
    // Native address bytes remain part of the ABI even though the observer reads only the IPv4 prefix.
    std::byte reserved[12];
};

#pragma pack(pop)

static_assert(sizeof(GalaxyAddress) == 20);

// ECX carries the raw peer; EAX returns the caller's output pointer and RET 0x18 removes both stack arguments.
// An unassigned observer selects the cached default; a remote observer selects the address it saw for this peer.
using GetExternalId = fc::NativeCall<
    GalaxyAddress *(void *raw_peer, GalaxyAddress *output, GalaxyAddress observer) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

// The raw destructor takes only ECX; observing it retires the address before this instance can be reused.
using DestroyRawPeer = fc::NativeCall<void(void *raw_peer) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// Locates address queries, raw destruction and GalaxyRootPeer ownership fields for one supported image.
struct GalaxyPeerLayout {
    fc::FunctionLocation<GetExternalId> get_external_id;
    fc::FunctionLocation<DestroyRawPeer> destroy_raw_peer;
    fc::DataLocation<std::byte> raw_peer_vtable;
    fc::DataLocation<std::byte> owner_peer_vtable;
    fc::DataLocation<std::byte> owner_callback;
    fc::DataLocation<GalaxyAddress> unassigned_address;
    std::size_t owner_callback_offset;
    std::size_t owner_context_offset;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<GalaxyPeerLayout> select_galaxy_peer_layout(std::string_view profile);

} // namespace fc_plugins::direct_transport::server
