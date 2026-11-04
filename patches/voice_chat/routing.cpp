#include "handler.hpp"
#include "microphone_test.hpp"

namespace fc_plugins::voice_chat {

void Handler::peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept {
    const auto mode = mode_.load(std::memory_order_acquire);
    if (mode != fc::GameMode::Host && mode != fc::GameMode::Client && mode != fc::GameMode::Dedicated) {
        return;
    }

    // A playing host has distinct host and local-client FC endpoints, so it handles both sides of negotiation.
    const auto host = mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated;
    const auto host_endpoint = peer == multiplayer_.host() || peer == host_peer_.id;
    if ((!host && !host_endpoint) || (host_endpoint && mode == fc::GameMode::Dedicated)) {
        return;
    }

    PeerBinding binding{.id = peer, .slot = kHostSlot};
    if (!host_endpoint) {
        if (agreement == fc::Agreement::Compatible) {
            const auto info = multiplayer_.info(peer);
            if (!info || !info->slot || *info->slot >= kMaximumPlayers) {
                return;
            }
            binding.slot = static_cast<std::uint8_t>(*info->slot);
        } else {
            // Disconnect callbacks can arrive after live PeerInfo disappears; find the retained full identity.
            const auto retained = server_peers_.find(peer);
            if (!retained) {
                return;
            }
            binding = *retained;
        }
    }

    apply_session_event(
        {.kind = agreement == fc::Agreement::Compatible ? CallbackKind::Available : CallbackKind::Unavailable, .peer = binding},
        host && !host_endpoint);
}

void Handler::message_received(fc::PeerId peer, fc::Bytes payload) noexcept {
    const auto mode = mode_.load(std::memory_order_acquire);
    const auto host = (mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated) && peer != multiplayer_.host();
    if ((!host && peer != host_peer_.id) || feature_.agreement(peer) != fc::Agreement::Compatible) {
        return;
    }

    PeerBinding binding{.id = peer, .slot = kHostSlot};
    if (host) {
        const auto info = multiplayer_.info(peer);
        if (!info || !info->slot || *info->slot >= kMaximumPlayers) {
            return;
        }
        binding.slot = static_cast<std::uint8_t>(*info->slot);
    }

    // FC supplies the connection identity. Copy its borrowed payload and validate the patch's message shape and direction.
    const auto parsed = parse_state(payload);
    const auto expected = host ? StateKind::ClientSnapshot : StateKind::HostSnapshot;
    if (!parsed || (host && parsed->header.identity != 0) ||
        (!host && parsed->header.kind == StateKind::Delta && parsed->header.identity == 0 && parsed->header.flags != 0) ||
        (parsed->header.kind != expected && parsed->header.kind != StateKind::Delta &&
         !(parsed->header.kind == StateKind::Resync && !host))) {
        report_rejection();
        observe_state({.kind = CallbackKind::Rejected, .peer = binding}, StateResult::Rejected);
        return;
    }

    apply_session_event({.kind = CallbackKind::Message, .peer = binding, .state = *parsed}, host);
}

void Handler::game_changed(std::optional<fc::GameState> state) noexcept {
    const auto mode = state ? state->mode : fc::GameMode::None;
    const auto session = state ? state->session : 0;
    if (session == session_ && mode == mode_.load(std::memory_order_acquire)) {
        return;
    }

    // Close remote routes while rebuilding control state, preserving current devices and codec address claims.
    mode_.store(fc::GameMode::None, std::memory_order_release);
    server_peers_.reset();
    remote_players_.clear();
    host_peer_ = {};
    audio_host_.store(0, std::memory_order_release);
    host_flags_.store(0, std::memory_order_release);
    local_player_.store(kInvalidPlayer, std::memory_order_release);
    changed_players_.fill(false);
    host_revision_ = 0;
    server_revision_ = 1;
    local_snapshot_pending_ = false;
    local_state_dirty_.store(true, std::memory_order_release);
    session_ = session;
    mode_.store(mode, std::memory_order_release);

    // Game can notify after feature agreement is established; reuse the current compatible endpoints.
    const auto endpoint = multiplayer_.host();
    if (mode == fc::GameMode::Client || mode == fc::GameMode::Host) {
        if (feature_.agreement(endpoint) == fc::Agreement::Compatible) {
            peer_changed(endpoint, fc::Agreement::Compatible);
        }
    }
    if (mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated) {
        for (const auto peer : multiplayer_.clients()) {
            if (feature_.agreement(peer) == fc::Agreement::Compatible) {
                peer_changed(peer, fc::Agreement::Compatible);
            }
        }
    }
}

void Handler::apply_session_event(const CallbackEvent &event, bool host) noexcept {
    if (event.kind == CallbackKind::Available) {
        // A client connection needs its first snapshot; a client facing the host must replace all prior host state.
        if (host) {
            const auto started = server_peers_.begin(event.peer);
            if (!started) {
                observe_state(event, StateResult::Rejected);
                return;
            }
            if (started->previous != kInvalidPlayer) {
                mark_player_changed(started->previous);
            }
            mark_player_changed(event.peer.slot);
        } else {
            if (host_peer_ == event.peer) {
                return;
            }
            host_peer_ = event.peer;
            audio_host_.store(event.peer.id.value, std::memory_order_release);
            host_flags_.store(0, std::memory_order_release);
            host_revision_ = 0;
            remote_players_.clear();
            local_snapshot_pending_ = true;
            local_state_dirty_.store(true, std::memory_order_release);
        }
        observe_state(event, StateResult::Available);
        return;
    }

    if (event.kind == CallbackKind::Unavailable) {
        if (host) {
            if (const auto player = server_peers_.clear(event.peer)) {
                mark_player_changed(*player);
            }
        } else if (host_peer_.id == event.peer.id) {
            host_peer_ = {};
            audio_host_.store(0, std::memory_order_release);
            host_flags_.store(0, std::memory_order_release);
            host_revision_ = 0;
            remote_players_.clear();
            local_snapshot_pending_ = false;
        }
        observe_state(event, StateResult::Unavailable);
        return;
    }

    const auto &state = event.state.header;
    bool accepted{};
    if (host) {
        const auto updated =
            server_peers_.update(event.peer, state.player, state.flags, state.revision, state.kind == StateKind::ClientSnapshot);
        accepted = updated.has_value();
        if (updated) {
            if (state.kind == StateKind::ClientSnapshot) {
                server_peers_.require_snapshot(event.peer.slot);
            }
            if (updated->changed) {
                mark_player_changed(state.player);
            }
        }
    } else if (host_peer_.id == event.peer.id && state.revision >= host_revision_) {
        // Equal host revisions are valid: several changed player rows can share one revision on FC's ordered control path.
        if (state.kind == StateKind::Resync) {
            host_flags_.store(0, std::memory_order_release);
            remote_players_.clear();
            local_snapshot_pending_ = true;
            local_state_dirty_.store(true, std::memory_order_release);
            accepted = true;
        } else if (state.kind == StateKind::HostSnapshot) {
            // Publish every replacement row, clearing omissions; native readers observe individual stores.
            host_flags_.store(state.flags, std::memory_order_release);
            // A replacement with unchanged owners must keep their native stream progress.
            std::array<PlayerState, kMaximumPlayers> replacement{};
            for (std::size_t index = 0; index < event.state.count; ++index) {
                const auto &row = event.state.players[index];
                replacement[row.player] = row;
            }
            for (std::size_t player = 0; player < replacement.size(); ++player) {
                remote_players_.set(static_cast<std::uint8_t>(player), replacement[player].flags, replacement[player].identity);
            }
            accepted = true;
        } else {
            remote_players_.set(state.player, state.flags, state.identity);
            accepted = true;
        }
        if (accepted) {
            host_revision_ = state.revision;
        }
    }

    if (!accepted) {
        report_rejection();
    }
    observe_state(event, accepted ? StateResult::Accepted : StateResult::Rejected);
}

void Handler::update_client_state() noexcept {
    if (host_peer_.id.value == 0 || !local_state_dirty_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const auto player = local_player_.load(std::memory_order_acquire);
    const auto info = multiplayer_.info(multiplayer_.local());
    // Match the native player against the local-client slot before sending its claim to the host endpoint.
    if (!info || !info->slot || *info->slot != player || player >= kMaximumPlayers ||
        !send_state(host_peer_, local_snapshot_pending_ ? StateKind::ClientSnapshot : StateKind::Delta, player,
                    local_flags_.load(std::memory_order_acquire), local_revision_)) {
        local_state_dirty_.store(true, std::memory_order_release);
        return;
    }

    // Accepted means FC copied the message, not that the host applied it. Later native changes need a fresh revision.
    if (++local_revision_ == 0) {
        ++local_revision_;
    }
    local_snapshot_pending_ = false;
}

void Handler::update_server_state() noexcept {
    // Attempt full replacements first; a peer attempted here receives no row deltas during this update.
    std::array<bool, kMaximumPlayers> replacement_started{};
    server_peers_.for_each([&](PeerBinding peer, bool snapshot_pending) noexcept {
        if (!snapshot_pending) {
            return;
        }
        replacement_started[peer.slot] = true;
        if (send_host_snapshot(peer)) {
            server_peers_.snapshot_sent(peer.slot);
        }
    });

    // A refused delta schedules a full replacement for that peer, preserving its recovery work when dirty rows clear.
    for (std::size_t player = 0; player < changed_players_.size(); ++player) {
        if (!changed_players_[player]) {
            continue;
        }
        const auto flags = server_peers_.flags(static_cast<std::uint8_t>(player));
        server_peers_.for_each([&](PeerBinding peer, bool snapshot_pending) noexcept {
            if (replacement_started[peer.slot] || snapshot_pending) {
                return;
            }
            if (!send_state(peer, StateKind::Delta, static_cast<std::uint8_t>(player), flags, server_revision_)) {
                server_peers_.require_snapshot(peer.slot);
            }
        });
    }

    changed_players_.fill(false);
}

void Handler::mark_player_changed(std::uint8_t player) noexcept {
    if (player >= changed_players_.size()) {
        return;
    }

    changed_players_[player] = true;
    if (++server_revision_ == 0) {
        ++server_revision_;
    }
}

bool Handler::send_state(PeerBinding peer, StateKind kind, std::uint8_t player, std::uint8_t flags, std::uint32_t revision) noexcept {
    // Feature::send copies accepted bytes before returning; its asynchronous queue owns the copy.
    const auto mode = mode_.load(std::memory_order_acquire);
    const auto host = (mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated) && peer.slot != kHostSlot;
    const StateMessage message{
        .kind = kind, .flags = flags, .player = player, .revision = revision, .identity = host ? server_peers_.identity(player) : 0};
    const auto payload = bytes(message);
    const auto send_result = feature_.send(peer.id, payload);
    const auto accepted = send_result == fc::SendResult::Accepted;
    if (diagnostics_enabled()) {
        const voice_diagnostics::StateObservation observed{.peer_id = peer.id.value,
                                                           .revision = revision,
                                                           .peer_slot = peer.slot,
                                                           .player = player,
                                                           .flags = flags,
                                                           .message_kind = static_cast<std::uint8_t>(kind),
                                                           .result = accepted ? StateResult::SendAccepted : StateResult::SendRefused,
                                                           .send_result = static_cast<std::uint8_t>(send_result)};
        const auto error = GetLastError();
        diagnostics_.state(&observed);
        SetLastError(error);
    }
    return accepted;
}

bool Handler::send_host_snapshot(PeerBinding peer) noexcept {
    // Header flags describe relay service even when the host player's saved microphone preference is off.
    HostSnapshot snapshot{
        .header =
            {
                .kind = StateKind::HostSnapshot,
                .flags = kRoutingFlags,
                .player = 0,
                .revision = server_revision_,
            },
    };

    // Only populated rows are transmitted. FC owns a copy after acceptance, so this stack snapshot can expire.
    snapshot.header.player = server_peers_.copy_players(snapshot.players);
    const auto payload = bytes(snapshot);
    const auto send_result = feature_.send(peer.id, payload);
    const auto accepted = send_result == fc::SendResult::Accepted;
    if (diagnostics_enabled()) {
        const auto error = GetLastError();
        voice_diagnostics::StateObservation observed{.peer_id = peer.id.value,
                                                     .revision = server_revision_,
                                                     .peer_slot = peer.slot,
                                                     .player = kInvalidPlayer,
                                                     .flags = kRoutingFlags,
                                                     .message_kind = static_cast<std::uint8_t>(StateKind::HostSnapshot),
                                                     .result = accepted ? StateResult::SendAccepted : StateResult::SendRefused,
                                                     .send_result = static_cast<std::uint8_t>(send_result)};
        diagnostics_.state(&observed);
        if (accepted) {
            for (std::uint8_t index = 0; index < snapshot.header.player; ++index) {
                observed.player = snapshot.players[index].player;
                observed.flags = snapshot.players[index].flags;
                observed.result = StateResult::SentSnapshotRow;
                diagnostics_.state(&observed);
            }
        }
        SetLastError(error);
    }
    return accepted;
}

bool Handler::route_allowed(std::uint8_t source, std::uint8_t destination, std::uint8_t &exclusions) const noexcept {
    const auto mode = mode_.load(std::memory_order_acquire);
    if (mode != fc::GameMode::Host && mode != fc::GameMode::Dedicated) {
        exclusions = voice_diagnostics::Mode;
        return false;
    }
    const auto source_flags = server_peers_.flags(source);
    const auto destination_flags = server_peers_.flags(destination);
    if (source_flags == 0 || destination_flags == 0) {
        exclusions |= voice_diagnostics::NoAdvertisedFlags;
    }
    if ((source_flags & Talk) == 0) {
        exclusions |= voice_diagnostics::TalkDisabled;
    }
    if ((destination_flags & Listen) == 0) {
        exclusions |= voice_diagnostics::ListenDisabled;
    }
    if ((source_flags & SendConnected) == 0) {
        exclusions |= voice_diagnostics::CaptureUnavailable;
    }
    if ((destination_flags & ReceiveConnected) == 0) {
        exclusions |= voice_diagnostics::PlaybackUnavailable;
    }
    return can_send_voice(source_flags) && can_receive_voice(destination_flags);
}

bool Handler::send_possible(fc::NativeFunction<VoicePossible> original, void *transport, std::uint8_t source,
                            std::uint8_t destination) noexcept {
    if (!MicrophoneTestRoute::permits_send(source, destination)) {
        return false;
    }
    // Local echo is a stock node-to-itself route, independent of a multiplayer
    // session. Preserve that route while replacing decisions between peers.
    if (source == destination) {
        return original.call(transport, source, destination).value_or(false);
    }

    const auto local = local_player_.load(std::memory_order_acquire);
    // The sender owns its native recipient mask; each receiver applies its own listen policy during playback.
    if (source != local || !voice_active() || !send_enable_.call(transport, source, destination).value_or(false)) {
        return false;
    }

    const auto source_flags = local_flags_.load(std::memory_order_acquire);
    const auto destination_flags = remote_players_.get(destination);
    return can_send_voice(source_flags) && can_receive_voice(destination_flags);
}

bool Handler::receive_possible(fc::NativeFunction<VoicePossible> original, void *transport, std::uint8_t receiver,
                               std::uint8_t source) noexcept {
    auto observed = ui_observation(voice_diagnostics::UiQuery::ReceivePermission, source, receiver);
    observed.context = reinterpret_cast<std::uintptr_t>(transport);
    const auto finish = [&](bool result, voice_diagnostics::UiReason reason) {
        observed.result = result;
        observed.reason = reason;
        observe_ui(observed);
        return result;
    };

    // The native node-to-itself route keeps menu microphone testing independent of feature agreement.
    if (receiver == source) {
        const auto result = original.call(transport, receiver, source);
        observed.call_failed = !result;
        return finish(result.value_or(false), voice_diagnostics::UiReason::NativeLocal);
    }

    const auto local = local_player_.load(std::memory_order_acquire);
    observed.local_player = local;
    // The receiver owns its native listen mask; the source's recipient choice arrives in the packet header.
    if (receiver != local) {
        return finish(false, voice_diagnostics::UiReason::WrongReceiver);
    }
    if (!voice_active()) {
        return finish(false, voice_diagnostics::UiReason::Inactive);
    }
    const auto enabled = receive_enable_.call(transport, receiver, source);
    observed.receive_mask = enabled ? static_cast<std::uint8_t>(*enabled) : 2;
    if (!enabled) {
        return finish(false, voice_diagnostics::UiReason::MaskReadFailed);
    }
    if (!*enabled) {
        return finish(false, voice_diagnostics::UiReason::MaskDisabled);
    }

    const auto receiver_flags = local_flags_.load(std::memory_order_acquire);
    const auto source_flags = remote_players_.get(source);
    observed.local_flags = receiver_flags;
    observed.player_flags = source_flags;
    if (!can_receive_voice(receiver_flags)) {
        return finish(false, voice_diagnostics::UiReason::ReceiveUnavailable);
    }
    return finish(can_send_voice(source_flags),
                  can_send_voice(source_flags) ? voice_diagnostics::UiReason::Complete : voice_diagnostics::UiReason::SendUnavailable);
}

bool Handler::player_state(fc::NativeFunction<VoicePlayerQuery> original, void *transport, std::uint8_t player,
                           std::uint8_t required) const noexcept {
    // Native devices can remap before FC's cached local player changes; preserve every native local node.
    const auto native_local = is_local_.call(transport, player);
    const auto local = native_local.value_or(false);
    const auto flags =
        mode_.load(std::memory_order_acquire) == fc::GameMode::Dedicated ? server_peers_.flags(player) : remote_players_.get(player);
    const auto native_result = local ? original.call(transport, player) : fc::Result<bool>{(flags & required) == required};
    const auto result = native_result.value_or(false);
    if (required == (Speaking | SendConnected)) {
        auto observed = ui_observation(voice_diagnostics::UiQuery::Sending, player);
        observed.context = reinterpret_cast<std::uintptr_t>(transport);
        observed.native_local = native_local ? static_cast<std::uint8_t>(*native_local) : 2;
        observed.call_failed = !native_local || !native_result;
        if (!local) {
            observed.player_flags = flags;
        }
        observed.result = result;
        observed.reason = local ? voice_diagnostics::UiReason::NativeLocal : voice_diagnostics::UiReason::Complete;
        observe_ui(observed);
    }
    return result;
}

void Handler::set_sending(std::uint8_t player, bool speaking) noexcept {
    if (player >= kMaximumPlayers) {
        return;
    }

    const auto player_changed = local_player_.exchange(player, std::memory_order_acq_rel) != player;
    // Each producer changes only its mask, preserving concurrent routing publications.
    const auto previous = speaking ? local_flags_.fetch_or(Speaking, std::memory_order_acq_rel)
                                   : local_flags_.fetch_and(static_cast<std::uint8_t>(~Speaking), std::memory_order_acq_rel);
    if (player_changed || ((previous & Speaking) != 0) != speaking) {
        local_state_dirty_.store(true, std::memory_order_release);
    }
    auto observed = ui_observation(voice_diagnostics::UiQuery::LocalSending, player);
    observed.result = speaking;
    observe_ui(observed);
}

void Handler::set_local_routing(bool enabled) noexcept {
    constexpr auto policy = static_cast<std::uint8_t>(Talk | Listen);
    const auto previous = enabled ? local_flags_.fetch_or(policy, std::memory_order_acq_rel)
                                  : local_flags_.fetch_and(static_cast<std::uint8_t>(~policy), std::memory_order_acq_rel);
    if ((previous & policy) != (enabled ? policy : 0)) {
        local_state_dirty_.store(true, std::memory_order_release);
    }
}

void Handler::refresh_local_routing() noexcept {
    bool capture{};
    bool playback{};
    if (game_voice_is_open_.call().value_or(false)) {
        auto *node = find_voice_node_.call().value_or(nullptr);
        const auto player = node != nullptr ? fc::read_field<std::uint8_t>(node, kVoicePlayerOffset) : kInvalidPlayer;
        auto *transport = player < kMaximumPlayers ? get_voice_transport_.call().value_or(nullptr) : nullptr;
        if (transport != nullptr && is_local_.call(transport, player).value_or(false)) {
            // Original getters retain native local device truth, including an open node whose capture becomes unavailable.
            capture = send_connected_.call(transport, player).value_or(false);
            playback = receive_connected_.call(transport, player).value_or(false);
            if (local_player_.exchange(player, std::memory_order_acq_rel) != player) {
                local_state_dirty_.store(true, std::memory_order_release);
            }
        }
    }

    constexpr auto readiness = static_cast<std::uint8_t>(SendConnected | ReceiveConnected);
    const auto connected = local_routing_flags(false, capture, playback);
    auto previous = local_flags_.load(std::memory_order_acquire);
    // This producer owns only readiness; preserve policy changes and speaking updates from concurrent callbacks.
    while (!local_flags_.compare_exchange_weak(previous, static_cast<std::uint8_t>((previous & ~readiness) | connected),
                                               std::memory_order_acq_rel)) {
    }
    if ((previous & readiness) != connected) {
        local_state_dirty_.store(true, std::memory_order_release);
    }
}

bool Handler::voice_active() const noexcept {
    const auto mode = mode_.load(std::memory_order_acquire);
    return (mode == fc::GameMode::Client || mode == fc::GameMode::Host) &&
           (host_flags_.load(std::memory_order_acquire) & kRoutingFlags) == kRoutingFlags;
}

} // namespace fc_plugins::voice_chat
