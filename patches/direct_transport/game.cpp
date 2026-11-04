#include "game.hpp"

#include "../soldier_state_sync/patch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace fc_plugins::direct_transport {
namespace {

// Matches the allocator's CL pool choice and register setup in both retail builds.
fc::Evidence allocate_evidence() {
    return fc::masked_bytes({0x53, 0x84, 0xC9, 0xB8, 0, 0, 0, 0, 0xBB, 0, 0, 0, 0, 0x0F, 0x44, 0xD8},
                            {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF});
}

// Checks BeginWrite's ECX packet input and the data field at offset 0x0C.
fc::Evidence initialize_evidence() {
    return fc::exact_bytes({0x8B, 0xC1, 0xC7, 0x41, 0x0C});
}

struct ProfileSites {
    std::string_view profile;
    std::uint32_t allocate_packet;
    std::uint32_t initialize_packet;
    std::uint32_t packet_prefix;
    std::uint32_t first_identity;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .allocate_packet = {0x001D8DF0},
        .initialize_packet = {0x001D8F60},
        .packet_prefix = {0x01BAB8D0},
        .first_identity = {0x01ACC0E0},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .allocate_packet = {0x001D9EA0},
        .initialize_packet = {0x001DA010},
        .packet_prefix = {0x01BACD80},
        .first_identity = {0x01ACD590},
    },
};

// Constructs packet calls and endpoint reads from the selected retail RVAs.
GameLayout make_layout(const ProfileSites &sites) {
    GameLayout layout{
        .allocate_packet =
            {
                .rva = sites.allocate_packet,
                .name = "NetPacket::Alloc",
                .evidence = allocate_evidence(),
            },
        .initialize_packet =
            {
                .rva = sites.initialize_packet,
                .name = "NetPacket::BeginWrite",
                .evidence = initialize_evidence(),
            },
        // The game initializes this value at runtime; the file image contains zero.
        .packet_prefix = {.rva = sites.packet_prefix, .name = "bytes before the packet communication header"},
    };

    // Each endpoint's connected byte precedes its address by 0x20 bytes within a 0x60-byte record.
    for (std::size_t slot = 0; slot < layout.endpoints.size(); ++slot) {
        const auto slot_offset = static_cast<std::int32_t>(slot * kNativeEndpointStride);
        const auto identity = sites.first_identity + slot_offset;
        layout.endpoints[slot] = {
            .connected =
                {
                    .rva = identity - kNativeEndpointConnectedOffset,
                    .name = "network endpoint connected flag",
                },
            .identity = {.rva = identity, .name = "network endpoint identity"},
        };
    }
    return layout;
}

} // namespace

std::optional<GameLayout> select_game_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

bool read_native_identity(const NativeEndpoint &endpoint, std::uint64_t &identity) noexcept {
    identity = 0;
    if (endpoint.connected == nullptr || endpoint.identity == nullptr || *endpoint.connected == std::byte{}) {
        return false;
    }

    identity = *endpoint.identity;
    return identity != 0;
}

bool read_native_address(const NativeEndpoint &endpoint, NativeAddress &address) noexcept {
    address = {};
    std::uint64_t identity{};
    if (!read_native_identity(endpoint, identity)) {
        return false;
    }

    std::memcpy(address.data(), endpoint.identity, sizeof(address));
    return true;
}

NativePacketResult build_native_packet(const NativePacketFactory &factory, std::span<const std::uint8_t> bytes, void *&packet) noexcept {
    packet = nullptr;
    if (bytes.size() < kNativeHeaderBytes || bytes.size() > kMaximumNativeBytes) {
        return NativePacketResult::RejectedPayload;
    }

    // Native game packets, including negotiated Soldier formats, reserve three bytes before their header and carry
    // at most 1009 received bytes in the native large pool's 1012-byte data area.
    const bool game_packet = bytes[0] < 0x1E || bytes[0] == soldier_state_sync::kStateUpdatePacket ||
                             bytes[0] == soldier_state_sync::kPrecisionUpdatePacket ||
                             bytes[0] == soldier_state_sync::kWeaponStateUpdatePacket;
    const auto prefix = game_packet && factory.prefix != nullptr ? *factory.prefix : 0U;
    if ((game_packet && prefix != 3) || bytes.size() > kMaximumNativeBytes - prefix) {
        return NativePacketResult::InvalidPrefix;
    }

    // The small pool has 0x74 data bytes; the large pool has 1012. Both follow a 12-byte object
    // header. Native allocation can flush queued packets and repopulate pools when they are empty.
    const auto allocated = factory.allocate.call(bytes.size() <= 0x74 - prefix);
    if (!allocated) {
        return NativePacketResult::FactoryUnavailable;
    }
    packet = *allocated;
    if (packet == nullptr) {
        return NativePacketResult::AllocationFailed;
    }

    // BeginWrite initializes the pool-dependent header. Recreate the native receive layout:
    // mCommIndex at +8 selects the communication header, and uint16 mMaxIndex at +6 bounds it.
    static_cast<void>(factory.initialize.call(packet));
    auto *packet_bytes = static_cast<std::uint8_t *>(packet);
    std::memcpy(packet_bytes + 0x0C + prefix, bytes.data(), bytes.size());
    packet_bytes[8] = static_cast<std::uint8_t>(prefix);
    const auto total = static_cast<std::uint16_t>(bytes.size() + prefix);
    fc::write_field(packet, 6, total);
    return NativePacketResult::Built;
}

} // namespace fc_plugins::direct_transport
