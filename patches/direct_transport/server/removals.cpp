#include "handler.hpp"

namespace fc_plugins::direct_transport::server {

void Handler::service_removals() noexcept {
    // UpdateBootPlayer marks the primary owner booted, sends MSG_BOOTED and sets a two-second native timer.
    // A later call past that timer disconnects the player and writes -1 into the supplied target.
    for (auto &removal : removals_) {
        if (!current_removal(removal)) {
            removal = {};
            continue;
        }

        if (!is_playing_.call(removal.player, false).value_or(false)) {
            static_cast<void>(set_not_playing_.call(removal.player));
            continue;
        }

        // Disconnect can clear this record inside UpdateBootPlayer. Keep its mutable native argument on the stack.
        auto player = removal.player;
        const auto generation = removal.generation;
        static_cast<void>(update_boot_player_.call(&player));
        if (removal.generation == generation) {
            removal.player = player;
            if (player < 0) {
                removal = {};
            }
        }
    }
}

bool Handler::current_removal(const Removal &removal) const noexcept {
    if (removal.player < 0 || removal.primary < 0 || removal.primary >= static_cast<std::int32_t>(associations_.size())) {
        return false;
    }

    std::uint64_t identity{};
    if (!read_identity(removal.peer.slot, identity) || identity != removal.peer.galaxy_id ||
        !read_identity(static_cast<std::uint8_t>(removal.primary), identity) || identity != removal.primary_identity) {
        return false;
    }

    const auto owner = get_guest_owner_.call(removal.peer.slot).value_or(-1);
    if ((owner >= 0 ? owner : removal.peer.slot) != removal.primary) {
        return false;
    }

    if (removal.peer.id.value != 0) {
        const auto peer = multiplayer_.info(removal.peer.id);
        if (!peer || peer->slot != removal.peer.slot || peer->galaxy_id != removal.peer.galaxy_id) {
            return false;
        }
    }
    const auto &association = associations_[removal.peer.slot];
    return !association.live || association.generation == removal.generation;
}

void Handler::native_disconnect(std::int32_t slot) noexcept {
    if (!network_thread_.current() || slot < 0 || slot >= static_cast<std::int32_t>(associations_.size())) {
        return;
    }

    // Unsupported clients have no FC identity; this boundary still ends their exact native connection lifetime.
    for (auto &removal : removals_) {
        if (removal.peer.slot == slot || removal.primary == slot) {
            removal = {};
        }
    }
    end_association(static_cast<std::uint8_t>(slot), direct::AssociationEndReason::Disconnected);
}

void Handler::remove_peer(std::uint8_t slot, RemovalReason reason) noexcept {
    if (slot >= associations_.size() || !associations_[slot].live) {
        return;
    }

    if (reason == RemovalReason::DirectRequired) {
        log_failure(slot, direct::RouteReason::None, FailureAction::UnsupportedRemoval);
    }

    const auto &association = associations_[slot];
    auto &removal = removals_[slot];
    const auto owner = get_guest_owner_.call(slot).value_or(-1);
    const auto primary = owner >= 0 ? owner : slot;
    std::uint64_t primary_identity{};
    if (primary >= static_cast<std::int32_t>(associations_.size()) ||
        !read_identity(static_cast<std::uint8_t>(primary), primary_identity)) {
        return;
    }

    removal = {.peer = association.peer,
               .generation = association.generation,
               .primary_identity = primary_identity,
               .primary = primary,
               .player = slot};
    end_association(slot, direct::AssociationEndReason::PolicyRemoval);

    // Playing clients use the game's boot notification/timer; nonplaying clients disconnect through SetNotPlaying.
    if (!is_playing_.call(static_cast<std::int32_t>(slot), false).value_or(false)) {
        static_cast<void>(set_not_playing_.call(static_cast<std::int32_t>(slot)));
    }
}

} // namespace fc_plugins::direct_transport::server
