#include "handler.hpp"

#include <Windows.h>

namespace fc_plugins::voice_chat {

bool Handler::diagnostics_enabled() const noexcept {
    if (!diagnostics_ready_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto error = GetLastError();
    const bool enabled = diagnostics_.capture() != 0;
    SetLastError(error);
    return enabled;
}

voice_diagnostics::UiObservation Handler::ui_observation(voice_diagnostics::UiQuery query, std::uint8_t player,
                                                         std::uint8_t receiver) const noexcept {
    const auto local = local_player_.load(std::memory_order_acquire);
    const auto mode = mode_.load(std::memory_order_acquire);
    const auto flags = local_flags_.load(std::memory_order_acquire);
    return {.query = query,
            .player = player,
            .receiver = receiver,
            .local_player = local,
            .mode = static_cast<std::uint8_t>(mode),
            .local_flags = flags,
            .player_flags = player == local                   ? flags
                            : mode == fc::GameMode::Dedicated ? server_peers_.flags(player)
                                                              : remote_players_.get(player),
            .host_flags = host_flags_.load(std::memory_order_acquire)};
}

void Handler::observe_ui(const voice_diagnostics::UiObservation &observation) const noexcept {
    if (!diagnostics_enabled()) {
        return;
    }
    const auto error = GetLastError();
    diagnostics_.ui(&observation);
    SetLastError(error);
}

void Handler::observe_view() const noexcept {
    if (!diagnostics_enabled()) {
        return;
    }
    const auto error = GetLastError();
    const auto peer = multiplayer_.local();
    const auto info = multiplayer_.info(peer);
    voice_diagnostics::ViewObservation view{
        .session = session_,
        .local_peer = peer.value,
        .host_peer = host_peer_.id.value,
        .local_revision = local_revision_,
        .host_revision = host_revision_,
        .local_player = local_player_.load(std::memory_order_acquire),
        .local_slot = info && info->slot && *info->slot < kMaximumPlayers ? static_cast<std::uint8_t>(*info->slot) : kInvalidPlayer,
        .mode = static_cast<std::uint8_t>(mode_.load(std::memory_order_acquire)),
        .local_flags = local_flags_.load(std::memory_order_acquire),
        .host_flags = host_flags_.load(std::memory_order_acquire),
        .dirty = static_cast<std::uint8_t>(local_state_dirty_.load(std::memory_order_acquire))};
    const auto now = std::chrono::steady_clock::now();
    if (!view_dirty_ && now < next_view_ && view.session == last_view_.session && view.local_peer == last_view_.local_peer &&
        view.host_peer == last_view_.host_peer && view.local_revision == last_view_.local_revision &&
        view.host_revision == last_view_.host_revision && view.local_player == last_view_.local_player &&
        view.local_slot == last_view_.local_slot && view.mode == last_view_.mode && view.local_flags == last_view_.local_flags &&
        view.host_flags == last_view_.host_flags && view.dirty == last_view_.dirty) {
        SetLastError(error);
        return;
    }
    view_dirty_ = false;
    next_view_ = now + std::chrono::seconds{1};
    for (std::uint8_t player = 0; player < kMaximumPlayers; ++player) {
        view.players[player] = remote_players_.get(player);
    }
    last_view_ = view;
    diagnostics_.view(&view);
    SetLastError(error);
}

} // namespace fc_plugins::voice_chat
