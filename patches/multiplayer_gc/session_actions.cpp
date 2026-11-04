#include "session.hpp"

#include <algorithm>
#include <utility>

namespace fc_plugins::multiplayer_gc {
namespace {

// These original screens belong to an encounter, including a space battle's ground follow-up.
bool encounter_screen(std::string_view screen) {
    return screen == "ifs_freeform_battle" || screen == "ifs_freeform_battle_mode" || screen == "ifs_freeform_battle_card" ||
           screen == "ifs_freeform_result" || screen == "ifs_freeform_end";
}

} // namespace

void Session::set_decision(Decision decision, State state) {
    std::lock_guard lock(commands_mutex_);
    commands_.decision = Commands::DecisionRequest{std::move(decision), std::move(state)};
}

void Session::update_summary(Summary summary) {
    std::lock_guard lock(commands_mutex_);
    commands_.summary = std::move(summary);
}

fc::Result<> Session::request_action(Action action) {
    if (!valid_action_name(action.name)) {
        return std::unexpected(fc::Error{"That campaign action is not supported."});
    }
    {
        std::lock_guard lock(poll_mutex_);
        if (!poll_.decision.presentation_phase.empty()) {
            return std::unexpected(fc::Error{"Wait for the current fleet action to finish."});
        }
    }
    std::lock_guard lock(commands_mutex_);
    if (commands_.action) {
        return std::unexpected(fc::Error{"Wait for the current campaign action to finish."});
    }
    commands_.action = std::move(action);
    return {};
}

void Session::action_result(std::uint32_t token, bool accepted, std::string message) {
    std::lock_guard lock(commands_mutex_);
    commands_.action_result = Message{.kind = MessageKind::ActionResult, .error = std::move(message), .token = token, .accepted = accepted};
}

fc::Result<> Session::prepare_battle(BattleSetup setup) {
    {
        std::lock_guard lock(poll_mutex_);
        if (!poll_.host) {
            return std::unexpected(fc::Error{"Only the host can prepare the battle."});
        }
    }
    std::lock_guard lock(commands_mutex_);
    commands_.battle = std::move(setup);
    return {};
}

void Session::battle_ready(std::uint32_t id) {
    std::lock_guard lock(commands_mutex_);
    commands_.battle_ready = id;
}

void Session::observe_transition(bool mission, bool shell) {
    if (!active_) {
        return;
    }
    if (mission && phase_ != Phase::Battle) {
        phase_ = Phase::Battle;
        awaiting_launch_ = false;
        addon_ready_ = ready_sent_ = false;
        publish_view(false);
    } else if (shell && phase_ == Phase::Battle) {
        phase_ = Phase::Returning;
        returned_ = true;
        publish_view(false);
    }
}

void Session::finish_action(fc::PeerId peer, bool accepted, std::string message) {
    if (peer == local_) {
        action_waiting_ = false;
        std::lock_guard lock(poll_mutex_);
        poll_.action_done = true;
        poll_.action_accepted = accepted;
        poll_.action_message = accepted ? std::string{} : std::move(message);
    } else {
        queue(peer, {.kind = MessageKind::ActionResult, .error = std::move(message), .accepted = accepted});
    }
}

void Session::accept_action(fc::PeerId peer, Action action) {
    const auto *member = participants_.find(peer);
    const auto faction = member ? action_faction(decision_, action.name, member->faction, peer == local_) : std::nullopt;
    if (!active_ || phase_ == Phase::Battle || !decision_.presentation_phase.empty() || action.decision != decision_.id || !faction ||
        !action_owner(action.name, peer == local_, participants_.commander(*faction) == peer) ||
        (action.name == "choose_bonus" ? timers_.bonus_expired(DecisionTimers::Clock::now())
                                       : timers_.turn_expired(DecisionTimers::Clock::now()))) {
        finish_action(peer, false, "The campaign decision changed. Choose your action again.");
        return;
    }
    if (action_) {
        if (action.name == "choose_bonus" && action_->faction != *faction && !bonus_actions_[*faction - 1]) {
            // The other faction may submit while Lua is applying the first choice; rules still run one at a time.
            bonus_actions_[*faction - 1] = std::pair{peer, std::move(action)};
            return;
        }
        finish_action(peer, false, "Wait for the current campaign action to finish.");
        return;
    }
    action.token = ++next_action_;
    action.faction = *faction;
    action_peer_ = peer;
    action_ = std::move(action);
    std::lock_guard lock(poll_mutex_);
    poll_.action = action_;
}

void Session::dispatch_bonus() {
    for (auto &pending : bonus_actions_) {
        if (!action_ && pending) {
            auto request = std::exchange(pending, std::nullopt);
            accept_action(request->first, std::move(request->second));
        }
    }
}

void Session::update_timers() {
    const auto now = DecisionTimers::Clock::now();
    const bool strategic = phase_ != Phase::Battle && phase_ != Phase::Returning && decision_.strategic_timer_active &&
                           decision_.screen != "ifs_freeform_battle_mode" && decision_.screen != "ifs_freeform_battle_card" &&
                           decision_.screen != "ifs_freeform_result" && decision_.screen != "ifs_freeform_end";
    timers_.update(decision_.turn, decision_.faction,
                   strategic && (decision_.screen == "ifs_freeform_fleet" || decision_.screen == "ifs_freeform_ai"), strategic,
                   phase_ == Phase::Battle || phase_ == Phase::Returning ? 0 : decision_.bonus_pending_mask, turn_timer(), now);
    const auto turn_remaining = timers_.turn_remaining(now);
    const auto bonus_remaining = timers_.bonus_remaining(now);
    // Time passing changes only the local view. Real state updates carry a fresh sample for client interpolation.
    decision_dirty_ = decision_dirty_ || decision_.timer_enabled != timers_.turn_enabled();
    decision_.timer_enabled = timers_.turn_enabled();
    decision_.timer_remaining_ms = turn_remaining;
    decision_.bonus_remaining_ms = bonus_remaining;
}

void Session::publish_metadata() {
    SessionInfo info{.count = static_cast<std::uint8_t>(participants_.accepted_count()),
                     .cooperative = snapshot_.cooperative,
                     .era = summary_.era,
                     .campaign = summary_.campaign,
                     .turn = summary_.turn,
                     .host_faction = snapshot_.host_faction,
                     .factions = summary_.factions,
                     .victory_condition = summary_.victory_condition,
                     .difficulty = summary_.difficulty,
                     .team_damage = summary_.team_damage,
                     .map = phase_ == Phase::Battle && !summary_.map.empty() ? summary_.map : "Galaxy Map",
                     .joinable = phase_ == Phase::Strategic,
                     .maximum = summary_.maximum};
    const auto bytes = encode_metadata(info);
    if (bytes != metadata_ && item_.publish(bytes)) {
        metadata_ = bytes;
    }
}

void Session::publish_decision() {
    const auto previous = decision_.commanders;
    for (std::uint8_t faction = 1; faction <= 2; ++faction) {
        const auto info = multiplayer_.info(participants_.commander(faction));
        decision_.commanders[faction - 1] = info && info->slot ? static_cast<std::int32_t>(*info->slot) : -1;
    }
    roster_dirty_ = roster_dirty_ || previous != decision_.commanders;
    if (!decision_dirty_) {
        return;
    }
    decision_dirty_ = false;
    for (const auto &player : participants_.list()) {
        if (player.peer != local_) {
            queue(player.peer, {.kind = MessageKind::CurrentState, .snapshot = snapshot_, .decision = decision_});
        }
    }
}

void Session::process_commands(Commands &commands) {
    if (hosting_) {
        if (commands.summary) {
            summary_ = std::move(*commands.summary);
        }
        if (commands.decision) {
            auto next = std::move(commands.decision->decision);
            const bool state_changed = snapshot_.state != commands.decision->state;
            snapshot_.state = std::move(commands.decision->state);
            returned_ = false;
            // Bonus and result screens may pass control to the other faction within the same turn.
            if (next.screen == "ifs_freeform_fleet" || next.screen == "ifs_freeform_ai") {
                participants_.begin_turn(next.faction, next.turn);
            }
            const bool changed = !next.same_context(decision_);
            decision_dirty_ = decision_dirty_ || changed || state_changed ||
                              next.strategic_timer_active != decision_.strategic_timer_active ||
                              next.bonus_pending_mask != decision_.bonus_pending_mask || !decision_.id;
            next.id = decision_.id + static_cast<std::uint32_t>(changed || !decision_.id);
            next.commanders = decision_.commanders;
            // Lua publishes decision fields; the persistent deadline owner supplies countdown state.
            next.timer_enabled = decision_.timer_enabled;
            next.timer_remaining_ms = decision_.timer_remaining_ms;
            next.bonus_remaining_ms = decision_.bonus_remaining_ms;
            decision_ = std::move(next);
            summary_.turn = decision_.turn;
            if (phase_ != Phase::Battle) {
                phase_ = encounter_screen(decision_.screen) ? Phase::Preparing : Phase::Strategic;
            }
        }
        if (commands.action_result && action_ && commands.action_result->token == action_->token) {
            finish_action(action_peer_, commands.action_result->accepted, std::move(commands.action_result->error));
            action_.reset();
            action_peer_ = {};
        }
        // Recheck the original owner while Lua receives the queued action; Results keep host ownership.
        if (action_ &&
            (action_->decision != decision_.id || !action_faction(decision_, action_->name, action_->faction, action_peer_ == local_) ||
             !action_owner(action_->name, action_peer_ == local_, participants_.commander(action_->faction) == action_peer_))) {
            finish_action(action_peer_, false, "The campaign decision or Fleet Commander changed before the action was applied.");
            action_.reset();
            std::lock_guard lock(poll_mutex_);
            poll_.action.reset();
        }
        if (commands.action) {
            accept_action(local_, std::move(*commands.action));
        }
        dispatch_bonus();
        if (commands.battle) {
            const auto id = battle_.id + 1;
            battle_ = std::move(*commands.battle);
            battle_.id = id;
            phase_ = Phase::Preparing;
            battle_ready_.clear();
            awaiting_launch_ = true;
            for (const auto &player : participants_.list()) {
                if (player.peer != local_ && player.faction) {
                    queue(player.peer, {.kind = MessageKind::BattleSetup, .battle = battle_});
                }
            }
            std::lock_guard lock(poll_mutex_);
            poll_.setup = battle_;
        }
        if (commands.battle_ready && *commands.battle_ready == battle_.id && !std::ranges::contains(battle_ready_, local_)) {
            battle_ready_.push_back(local_);
        }
        update_timers();
        publish_decision();
    } else {
        if (commands.action && active_ && !action_waiting_) {
            action_waiting_ = true;
            queue(host_, {.kind = MessageKind::Action, .action = std::move(*commands.action)});
        }
        if (commands.battle_ready && *commands.battle_ready == battle_.id) {
            queue(host_, {.kind = MessageKind::BattleReady, .token = battle_.id});
        }
    }
}

} // namespace fc_plugins::multiplayer_gc
