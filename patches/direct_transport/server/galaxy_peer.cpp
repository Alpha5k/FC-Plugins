#include "galaxy_peer.hpp"

#include "handler.hpp"

#include <WinSock2.h>

#include <array>
#include <string_view>

namespace fc_plugins::direct_transport::server {
namespace {

// Stores the query, destruction and native owner links for each supported GalaxyPeer revision.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t get_external_id;
    std::uint32_t destroy_raw_peer;
    std::uint32_t raw_peer_vtable;
    std::uint32_t owner_peer_vtable;
    std::uint32_t owner_callback;
    std::uint32_t unassigned_address;
    std::size_t owner_callback_offset;
    std::size_t owner_context_offset;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "GalaxyPeer.59E6304A",
        .get_external_id = {0x0048A970},
        .destroy_raw_peer = {0x004871A0},
        .raw_peer_vtable = {0x0087AE64},
        .owner_peer_vtable = {0x00865F94},
        .owner_callback = {0x0034D670},
        .unassigned_address = {0x00A4AD88},
        .owner_callback_offset = 0x598,
        .owner_context_offset = 0x59C,
    },
    ProfileSites{
        .profile = "GalaxyPeer.5BBE22A6",
        .get_external_id = {0x00475C50},
        .destroy_raw_peer = {0x004723F0},
        .raw_peer_vtable = {0x008B107C},
        .owner_peer_vtable = {0x008A0DF8},
        .owner_callback = {0x00340450},
        .unassigned_address = {0x00A81360},
        .owner_callback_offset = 0x7A0,
        .owner_context_offset = 0x7A4,
    },
};

// Constructs query, destruction and native ownership locations for the selected revision.
GalaxyPeerLayout make_layout(const ProfileSites &sites) {
    return {
        .get_external_id = {.rva = sites.get_external_id, .name = "external peer address query"},
        .destroy_raw_peer = {.rva = sites.destroy_raw_peer, .name = "raw peer destruction"},
        .raw_peer_vtable = {.rva = sites.raw_peer_vtable, .name = "raw peer vtable"},
        .owner_peer_vtable = {.rva = sites.owner_peer_vtable, .name = "GalaxyRootPeer vtable"},
        .owner_callback = {.rva = sites.owner_callback, .name = "Galaxy peer update callback"},
        .unassigned_address = {.rva = sites.unassigned_address, .name = "unassigned peer address"},
        .owner_callback_offset = sites.owner_callback_offset,
        .owner_context_offset = sites.owner_context_offset,
    };
}

} // namespace

std::optional<GalaxyPeerLayout> select_galaxy_peer_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

void Handler::observe_galaxy_peer(void *raw_peer, GalaxyAddress observer, GalaxyAddress *result) noexcept {
    const auto *vtable = raw_peer == nullptr ? nullptr : fc::read_field<const std::byte *>(raw_peer);

    // Vtable equality selects the raw RakPeer implementation; multiple instances can share this table.
    if (vtable != raw_peer_vtable_) {
        return;
    }

    // GalaxyRakPeer registers its update callback and its own context before starting this raw peer.
    // The game GalaxyRootPeer owns raw at +0x64; utility peers and other Galaxy owners cannot publish here.
    if (fc::read_field<const std::byte *>(raw_peer, owner_callback_offset_) != owner_callback_) {
        return;
    }
    const auto *owner = fc::read_field<const void *>(raw_peer, owner_context_offset_);
    if (owner == nullptr || fc::read_field<const std::byte *>(owner) != owner_peer_vtable_ ||
        fc::read_field<const void *>(owner, 0x64) != raw_peer) {
        return;
    }

    if (public_address_peer_.exchange(raw_peer, std::memory_order_acq_rel) != raw_peer) {
        public_ipv4_.store(0, std::memory_order_release);
        endpoint_unavailable_.store(false, std::memory_order_release);
    }

    // Keep both cached and remote-positive acquisition. Native SystemAddress equality uses the IPv4
    // observer's port/address, not its opaque tail; only a default query can withdraw this peer's address.
    if (result != nullptr && result->family == AF_INET && direct::is_public_ipv4(result->address_network_order)) {
        observe_public_endpoint(result->address_network_order);
    } else if (observer.family == AF_INET && observer.port_network_order == unassigned_address_->port_network_order &&
               observer.address_network_order == unassigned_address_->address_network_order) {
        observe_unusable_endpoint();
    }
}

void Handler::destroy_galaxy_peer(void *raw_peer) noexcept {
    auto *owner = raw_peer;
    if (public_address_peer_.compare_exchange_strong(owner, nullptr, std::memory_order_acq_rel)) {
        public_ipv4_.store(0, std::memory_order_release);
        endpoint_unavailable_.store(false, std::memory_order_release);
    }
}

} // namespace fc_plugins::direct_transport::server
