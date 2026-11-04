#include "session.hpp"
#include "transition.hpp"

#include <algorithm>
#include <utility>

namespace fc_plugins::multiplayer_gc {

fc::Result<> Session::prepare(fc::PrepareContext &context, Shell &shell, Transition &transition) {
    shell_ = &shell;
    transition_ = &transition;
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    const auto feature = multiplayer_.feature({std::string{kFeatureId}, fc::FeatureRole::Both});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    const auto item = context.lobby().item(kLobbyItem, kLobbyVersion);
    if (!item) {
        return std::unexpected(item.error());
    }
    item_ = *item;
    return feature_.on_message([this](fc::PeerId peer, fc::Bytes bytes) noexcept {
        try {
            receive(peer, bytes);
        } catch (...) {
            feature_.fail(peer, "Could not copy the campaign message.");
        }
    });
}

fc::Result<> Session::host_campaign(State state, bool cooperative, std::uint8_t faction, Summary summary) {
    if (faction != 1 && faction != 2) {
        return std::unexpected(fc::Error{"Choose the loaded campaign's faction."});
    }
    std::lock_guard lock(commands_mutex_);
    commands_.host = HostRequest{std::move(state), std::move(summary), cooperative, faction};
    commands_.leave = false;
    // BeginLobby may publish before the control thread sees this command.
    shell_->host_intent(true);
    return {};
}

void Session::shell_ready() {
    std::lock_guard lock(commands_mutex_);
    commands_.ready = true;
}

fc::Result<> Session::choose(std::uint8_t faction) {
    if (faction != 1 && faction != 2) {
        return std::unexpected(fc::Error{"Choose one of this campaign's factions."});
    }
    std::lock_guard lock(commands_mutex_);
    commands_.choose = faction;
    return {};
}

void Session::leave() {
    shell_->host_intent(false);
    shell_->configure({});
    std::lock_guard lock(commands_mutex_);
    commands_ = {.leave = true};
}

Poll Session::poll(bool peek) {
    std::lock_guard lock(poll_mutex_);
    auto result = poll_;
    if (!peek) {
        poll_.pending.reset();
        poll_.action.reset();
        poll_.setup.reset();
        poll_.action_done = false;
        poll_.action_accepted = false;
        poll_.action_message.clear();
    }
    return result;
}

Summary Session::summary() {
    std::lock_guard lock(poll_mutex_);
    return poll_.summary;
}

void Session::queue(fc::PeerId peer, const Message &message) {
    auto delivery = std::ranges::find(outgoing_, peer, &Delivery::peer);
    if (delivery == outgoing_.end()) {
        outgoing_.push_back({peer, {}});
        delivery = std::prev(outgoing_.end());
    }
    delivery->values.replace(message);
}

void Session::send_state(fc::PeerId peer, bool campaign) {
    if (peer == local_ || !participants_.find(peer)) {
        return;
    }
    queue(peer, {.kind = MessageKind::Roster, .roster = {snapshot_.players, decision_.commanders}});
    if (campaign) {
        queue(peer, {.kind = MessageKind::CurrentState, .snapshot = snapshot_, .decision = decision_});
        if (awaiting_launch_ && phase_ == Phase::Preparing && battle_.id && participants_.find(peer)->faction) {
            queue(peer, {.kind = MessageKind::BattleSetup, .battle = battle_});
        }
    }
}

void Session::receive(fc::PeerId peer, fc::Bytes bytes) {
    const auto message = decode_message(bytes);
    if (!message || feature_.agreement(peer) != fc::Agreement::Compatible) {
        return;
    }
    if (hosting_) {
        const auto clients = multiplayer_.clients();
        if (std::ranges::find(clients, peer) == clients.end()) {
            return;
        }
        if (message->kind == MessageKind::Ready) {
            if (std::ranges::find(ready_, peer) == ready_.end()) {
                ready_.push_back(peer);
            }
            // An existing participant requests current state after its shell VM is recreated.
            if (participants_.find(peer) && phase_ != Phase::Battle && !returned_) {
                send_state(peer, true);
            }
        } else if (message->kind == MessageKind::Action) {
            accept_action(peer, message->action);
        } else if (message->kind == MessageKind::BattleReady && awaiting_launch_ && message->token == battle_.id &&
                   participants_.find(peer) && participants_.find(peer)->faction && !std::ranges::contains(battle_ready_, peer)) {
            battle_ready_.push_back(peer);
        } else if (message->kind == MessageKind::Choose && participants_.find(peer)) {
            if (phase_ != Phase::Strategic) {
                queue(peer, {.kind = MessageKind::Error, .error = "Faction selection is closed while a battle is being prepared."});
                return;
            }
            const auto result = participants_.choose(peer, message->faction);
            if (!result) {
                queue(peer, {.kind = MessageKind::Error, .error = result.error().message});
            } else {
                // Team acceptance changes roster rows; the campaign pair remains the same.
                reconcile();
                publish_decision();
                for (const auto &player : participants_.list()) {
                    send_state(player.peer, false);
                }
                publish_view(false);
            }
        }
        return;
    }
    if ((!addon_ready_ && !active_) || peer != multiplayer_.host()) {
        return;
    }
    host_ = peer;
    switch (message->kind) {
    case MessageKind::CurrentState: {
        const bool campaign = !active_ || !message->decision.same_context(decision_) || snapshot_.state != message->snapshot.state ||
                              phase_ == Phase::Returning;
        const auto commanders = decision_.commanders;
        decision_ = message->decision;
        decision_.commanders = commanders;
        countdown_received_ = DecisionTimers::Clock::now();
        snapshot_.cooperative = message->snapshot.cooperative;
        snapshot_.host_faction = message->snapshot.host_faction;
        snapshot_.state = message->snapshot.state;
        active_ = true;
        if (phase_ == Phase::Returning) {
            returned_ = false;
            phase_ = Phase::Strategic;
        }
        message_.clear();
        // Lua consumes the campaign pair and presentation together while retaining its independent view.
        publish_view(campaign);
        break;
    }
    case MessageKind::ActionResult:
        finish_action(local_, message->accepted, message->error);
        break;
    case MessageKind::BattleSetup: {
        battle_ = message->battle;
        phase_ = Phase::Preparing;
        std::lock_guard lock(poll_mutex_);
        poll_.setup = battle_;
        break;
    }
    case MessageKind::Roster:
        if (roster_received_ && addon_ready_ && phase_ != Phase::Battle) {
            // Diff complete views; a native name change alone is not a new connection.
            for (const auto &player : snapshot_.players) {
                if (std::ranges::find(message->roster.players, player.slot, &Player::slot) == message->roster.players.end()) {
                    shell_->event(player.name + " left the campaign.");
                }
            }
            for (const auto &player : message->roster.players) {
                if (std::ranges::find(snapshot_.players, player.slot, &Player::slot) == snapshot_.players.end()) {
                    shell_->event(player.name + " joined the campaign.");
                }
            }
        }
        roster_received_ = true;
        snapshot_.players = message->roster.players;
        decision_.commanders = message->roster.commanders;
        publish_view(false);
        break;
    case MessageKind::Error:
        message_ = message->error;
        publish_view(false);
        break;
    default:
        break;
    }
}

bool Session::reconcile() {
    // The host establishes faction rules before a remote Ready can admit any participant.
    // Keep readiness queued while the native owner thread copies the host's name.
    if (!participants_.find(local_)) {
        return false;
    }
    const auto clients = multiplayer_.clients();
    const auto names = shell_->names();
    bool changed{};
    for (const auto &participant : std::vector{participants_.list()}) {
        if (std::ranges::find(clients, participant.peer) == clients.end() ||
            feature_.agreement(participant.peer) != fc::Agreement::Compatible) {
            if (addon_ready_ && phase_ != Phase::Battle) {
                shell_->event(participant.name + " left the campaign.");
            }
            participants_.remove(participant.peer);
            changed = true;
        }
    }
    std::erase_if(ready_, [&](fc::PeerId peer) {
        return std::ranges::find(clients, peer) == clients.end() || feature_.agreement(peer) != fc::Agreement::Compatible;
    });
    for (const auto peer : ready_) {
        const auto info = multiplayer_.info(peer);
        if (!info || !info->slot || *info->slot >= 64) {
            continue;
        }
        const auto name = std::ranges::find(names, *info->slot, &Player::slot);
        if (name == names.end() || name->name.empty()) {
            continue;
        }
        const auto *existing = participants_.find(peer);
        if (!existing && phase_ != Phase::Strategic) {
            continue;
        }
        const auto joined = !existing;
        const auto renamed = existing && existing->name != name->name;
        const auto result = participants_.ready(peer, name->name);
        if (!result) {
            queue(peer, {.kind = MessageKind::Error, .error = result.error().message});
            continue;
        }
        if (joined) {
            shell_->event(name->name + " joined the campaign.");
        }
        changed = changed || joined || renamed;
    }
    const auto previous = snapshot_.players;
    snapshot_.players.clear();
    for (const auto &participant : participants_.list()) {
        const auto info = multiplayer_.info(participant.peer);
        if (info && info->slot && *info->slot < 64) {
            snapshot_.players.push_back(
                {static_cast<std::uint8_t>(*info->slot), participant.faction, participant.peer == local_, participant.name});
        }
    }
    if (const auto *local = participants_.find(local_)) {
        snapshot_.local_faction = local->faction;
    }
    return changed || previous != snapshot_.players;
}

void Session::publish_view(bool campaign) {
    ShellSession shell{.active = active_, .host = hosting_};
    if (const auto info = multiplayer_.info(multiplayer_.local()); info && info->slot && *info->slot < 64) {
        shell.local_slot = static_cast<std::uint8_t>(*info->slot);
    }
    for (const auto &player : snapshot_.players) {
        shell.connected_slots |= std::uint64_t{1} << player.slot;
    }
    if (hosting_) {
        for (const auto peer : multiplayer_.clients()) {
            if (const auto info = multiplayer_.info(peer); info && info->slot && *info->slot < 64) {
                shell.capture_slots |= std::uint64_t{1} << *info->slot;
            }
        }
    }
    // Shell callbacks consume immutable copies only when their roster or policy actually changes.
    const bool players_changed = !published_shell_ || published_shell_->players != snapshot_.players;
    const bool role_changed = !published_shell_ || published_shell_->active != shell.active || published_shell_->host != shell.host;
    if (players_changed || role_changed || published_shell_->local_slot != shell.local_slot ||
        published_shell_->connected_slots != shell.connected_slots || published_shell_->capture_slots != shell.capture_slots) {
        shell.players = snapshot_.players;
        shell_->configure(shell);
        published_shell_ = shell;
    }
    const bool admission_closed = phase_ != Phase::Strategic;
    if (players_changed || role_changed || !published_admission_ || *published_admission_ != admission_closed) {
        transition_->configure({.active = active_, .host = hosting_, .admission_closed = admission_closed, .players = snapshot_.players});
        published_admission_ = admission_closed;
    }
    snapshot_.local_faction = 0;
    const auto local_info = multiplayer_.info(local_);
    if (local_info && local_info->slot) {
        const auto player = std::ranges::find(snapshot_.players, *local_info->slot, &Player::slot);
        if (player != snapshot_.players.end()) {
            snapshot_.local_faction = player->faction;
        }
    }
    std::lock_guard lock(poll_mutex_);
    poll_.phase = active_ ? (snapshot_.local_faction ? "strategic" : "choosing") : "joining";
    if (active_ && phase_ != Phase::Strategic) {
        poll_.phase = phase_ == Phase::Battle ? "battle" : phase_ == Phase::Returning ? "returning" : "preparing";
    }
    poll_.host = hosting_;
    // Poll retains the opaque pair only as one-shot work, rather than duplicating persistent state.
    poll_.snapshot.cooperative = snapshot_.cooperative;
    poll_.snapshot.host_faction = snapshot_.host_faction;
    poll_.snapshot.local_faction = snapshot_.local_faction;
    if (poll_.snapshot.players != snapshot_.players) {
        poll_.snapshot.players = snapshot_.players;
    }
    poll_.message = message_;
    poll_.decision = decision_;
    poll_.summary = summary_;
    const auto now = DecisionTimers::Clock::now();
    if (hosting_) {
        poll_.timer_expired = timers_.turn_expired(now);
        poll_.bonus_expired = timers_.bonus_expired(now);
        poll_.decision.timer_remaining_ms = timers_.turn_remaining(now);
        poll_.decision.bonus_remaining_ms = timers_.bonus_remaining(now);
    } else {
        // Client countdowns interpolate copied host values; only the host resolves expiry.
        const auto elapsed = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(now - countdown_received_).count()));
        poll_.decision.timer_remaining_ms = elapsed < decision_.timer_remaining_ms ? decision_.timer_remaining_ms - elapsed : 0;
        poll_.decision.bonus_remaining_ms = elapsed < decision_.bonus_remaining_ms ? decision_.bonus_remaining_ms - elapsed : 0;
    }
    poll_.local_slot = -1;
    if (const auto info = multiplayer_.info(local_); info && info->slot) {
        poll_.local_slot = static_cast<std::int32_t>(*info->slot);
    }
    poll_.returned = returned_;
    poll_.launch = hosting_ && awaiting_launch_ && phase_ == Phase::Preparing && battle_.id &&
                   std::ranges::all_of(participants_.list(), [this](const Participant &player) {
                       return !player.faction || std::ranges::contains(battle_ready_, player.peer);
                   });
    if (campaign) {
        poll_.pending = snapshot_.state;
    }
}

void Session::clear(bool keep_host_intent) {
    item_.clear();
    participants_.clear();
    ready_.clear();
    outgoing_.clear();
    hosting_ = addon_ready_ = ready_sent_ = observed_session_ = active_ = false;
    host_ = local_ = {};
    session_ = 0;
    snapshot_ = {};
    roster_received_ = false;
    summary_ = {};
    message_.clear();
    phase_ = Phase::Strategic;
    decision_ = {};
    battle_ = {};
    battle_ready_.clear();
    action_.reset();
    bonus_actions_ = {};
    action_peer_ = {};
    action_waiting_ = returned_ = decision_dirty_ = awaiting_launch_ = false;
    roster_dirty_ = false;
    published_shell_.reset();
    published_admission_.reset();
    timers_ = {};
    countdown_received_ = {};
    metadata_.clear();
    if (!keep_host_intent) {
        shell_->host_intent(false);
    }
    shell_->configure({});
    transition_->configure({});
    transition_->disarm();
    std::lock_guard lock(poll_mutex_);
    poll_ = {.phase = "closed"};
}

void Session::update() noexcept {
    try {
        observe_transition(transition_->mission(), transition_->shell());
        Commands commands;
        {
            std::lock_guard lock(commands_mutex_);
            commands = std::exchange(commands_, {});
        }
        if (commands.leave) {
            clear();
            if (!commands.host) {
                return;
            }
        }
        if (commands.host) {
            // A fresh host request replaces the previous native session, including a queued Leave.
            // Keep the synchronous publication gate set while BeginLobby creates the new session.
            clear(true);
            hosting_ = true;
            summary_ = std::move(commands.host->summary);
            snapshot_ = {.cooperative = commands.host->cooperative,
                         .host_faction = commands.host->faction,
                         .local_faction = commands.host->faction,
                         .state = std::move(commands.host->state)};
        }
        addon_ready_ = addon_ready_ || commands.ready;
        const auto game = game_.current();
        if (observed_session_ && (!game || game->session != session_ || (!game->is_host() && !game->is_client()))) {
            clear();
            return;
        }
        if (!game || (!game->is_host() && !game->is_client())) {
            if (hosting_ || addon_ready_) {
                publish_view(false);
            }
            return;
        }
        observed_session_ = true;
        session_ = game->session;
        host_ = multiplayer_.host();
        local_ = multiplayer_.local();
        if (hosting_ && game->is_host()) {
            const auto was_active = active_;
            if (participants_.list().empty() && local_.value) {
                const auto info = multiplayer_.info(local_);
                const auto names = shell_->names();
                const auto name = info && info->slot ? std::ranges::find(names, *info->slot, &Player::slot) : names.end();
                if (name != names.end() && !name->name.empty()) {
                    participants_.start(local_, name->name, snapshot_.host_faction, snapshot_.cooperative);
                    ready_.push_back(local_);
                }
            }
            active_ = (active_ || addon_ready_) && !participants_.list().empty();
            const auto started = active_ && !was_active;
            const auto before = participants_.list();
            auto changed = reconcile();
            process_commands(commands);
            if (commands.choose) {
                const auto result = participants_.choose(local_, *commands.choose);
                if (!result) {
                    message_ = result.error().message;
                } else {
                    changed = reconcile() || changed;
                }
            }
            if (changed || started || commands.host || roster_dirty_) {
                for (const auto &participant : participants_.list()) {
                    const auto previously = std::ranges::find(before, participant.peer, &Participant::peer);
                    send_state(participant.peer, started || previously == before.end());
                }
            }
            roster_dirty_ = false;
            // A replacement FC peer can reuse identical visible slot/name fields.
            if (changed) {
                published_shell_.reset();
            }
            publish_metadata();
            publish_view(started);
        } else if (game->is_client() && addon_ready_ && host_.value) {
            if (!ready_sent_ && feature_.agreement(host_) == fc::Agreement::Compatible) {
                queue(host_, {.kind = MessageKind::Ready});
                ready_sent_ = true;
            }
            if (commands.choose && active_) {
                queue(host_, {.kind = MessageKind::Choose, .faction = *commands.choose});
            }
            process_commands(commands);
            publish_view(false);
        }
        const auto clients = multiplayer_.clients();
        std::erase_if(outgoing_, [&](Delivery &delivery) {
            if ((hosting_ && std::ranges::find(clients, delivery.peer) == clients.end()) || (!hosting_ && delivery.peer != host_)) {
                return true;
            }
            const auto refused = delivery.values.flush([&](const auto &bytes) {
                return feature_.send(delivery.peer, bytes);
            });
            if (refused == fc::SendResult::TooLarge || refused == fc::SendResult::Failed || refused == fc::SendResult::Incompatible) {
                message_ = "The campaign could not be sent to a connected player.";
                feature_.fail(delivery.peer, message_);
            }
            return delivery.values.empty();
        });
    } catch (...) {
        message_ = "Could not update this campaign session.";
    }
}
} // namespace fc_plugins::multiplayer_gc
