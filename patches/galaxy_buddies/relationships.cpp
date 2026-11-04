#include "handler.hpp"

#include <algorithm>
#include <array>

namespace fc_plugins::galaxy_buddies {
namespace {
// Prefers published user data, then a real lobby name, then a real captured persona
// or retained name for the same account.
template <class Entry, class List> void reconcile_name(Entry &captured, const List &current, GameBridge &game) {
    if (captured.published_name) {
        return;
    }

    const auto current_end = current.entries.begin() + current.count;
    const auto existing = std::find_if(current.entries.begin(), current_end, [user = captured.id](const auto &entry) {
        return entry.id == user;
    });
    if (existing != current_end && existing->published_name) {
        captured.name = existing->name;
        captured.published_name = true;
        return;
    }

    std::array<char, GameBridge::kNameCapacity> lobby_name{};
    if (game.copy_lobby_name(captured.id, lobby_name) && !is_placeholder_name(lobby_name)) {
        captured.name = lobby_name;
        return;
    }

    if (!is_placeholder_name(captured.name)) {
        return;
    }
    if (existing != current_end && !is_placeholder_name(existing->name)) {
        captured.name = existing->name;
    }
}

// Applies display-name precedence to every relationship in a captured list.
template <class CapturedList, class CurrentList>
void reconcile_names(CapturedList &captured, const CurrentList &current, GameBridge &game) {
    for (std::size_t index = 0; index < captured.count; ++index) {
        reconcile_name(captured.entries[index], current, game);
    }
}

// Accepted-cache refresh leaves rich presence separate. Preserve the last parsed
// connect lobby ID until a later cached-presence read replaces it.
void preserve_lobbies(GameBridge::AcceptedList &captured, const GameBridge::AcceptedList &current) {
    for (std::size_t index = 0; index < captured.count; ++index) {
        auto &entry = captured.entries[index];
        const auto current_end = current.entries.begin() + current.count;
        const auto existing = std::find_if(current.entries.begin(), current_end, [user = entry.id](const auto &candidate) {
            return candidate.id == user;
        });
        if (existing != current_end) {
            entry.lobby = existing->lobby;
        }
    }
}

} // namespace

void Handler::add_lobby(const GalaxyId *lobby) noexcept {
    if (game_.should_add_lobby(lobby)) {
        static_cast<void>(original_add_lobby_.call(lobby));
    }
}

void Handler::request_relationships() noexcept {
    if (!listeners_registered_ || !game_.logged_on()) {
        return;
    }

    // Retain this request until the previous completion has been copied successfully.
    if (accepted_response_ready_.load(std::memory_order_acquire)) {
        callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
        return;
    }

    record_accepted_request(game_.request_accepted_buddies());
}

void Handler::service_callback_requests() noexcept {
    if (!listeners_registered_) {
        return;
    }

    const auto requests = callback_requests_.exchange(0, std::memory_order_acq_rel);
    // A relationship request takes precedence; accepted success requests invitations again.
    // These bits are consumed even when a logged-off account prevents submission.
    if ((requests & kRequestRelationships) != 0) {
        request_relationships();
    } else if ((requests & kRequestInvitations) != 0) {
        record_operation(GalaxyOperation::RequestInvitations, game_.request_invitations());
    }

    // Presence notifications reuse accepted account IDs, independently of Lua screen activity.
    if ((requests & kRefreshPresence) != 0) {
        game_.refresh_presence(accepted_);
    }
    if ((requests & kRefreshLobbies) != 0) {
        game_.refresh_lobbies(accepted_);
    }
}

void Handler::request_lobbies() noexcept {
    for (std::size_t index = 0; index < accepted_.count; ++index) {
        const auto result = game_.request_lobby(accepted_.entries[index].id);
        if (!result.accepted()) {
            record_operation(GalaxyOperation::RequestLobby, result);
        }
    }

    // Use cached values now; RichPresenceListener schedules another read when
    // later results arrive. A lobby ID does not establish that a join will succeed.
    game_.refresh_lobbies(accepted_);
}

void Handler::reconcile_state(bool relationships_changed) noexcept {
    // Apply the latest copied caches before individual callback changes. Their overlays
    // retain confirmed additions/removals across stale accepted responses within fixed limits.
    relationships_changed = consume_incoming() || relationships_changed;
    relationships_changed = consume_relationship() || relationships_changed;
    relationships_changed = consume_pending_requests() || relationships_changed;
    if (relationships_changed) {
        publish_buddies();
        schedule_display_name_refresh();
        request_lobbies();
    }

    consume_display_name_updates();
}

bool Handler::consume_incoming() noexcept {
    if (const auto dropped = dropped_incoming_callbacks_.exchange(0, std::memory_order_acq_rel); dropped != 0) {
        logger_.warning("Incoming buddy list callbacks dropped: {}; the callback "
                        "queue was busy",
                        dropped);
        callback_requests_.fetch_or(kRequestInvitations, std::memory_order_release);
    }

    if (!incoming_ready_.load(std::memory_order_acquire) || incoming_lock_.test_and_set(std::memory_order_acquire)) {
        return false;
    }

    // Copy the snapshot and release its callback lock before reading names or reconciling lists.
    auto captured = incoming_mailbox_;
    incoming_ready_.store(false, std::memory_order_release);
    incoming_lock_.clear(std::memory_order_release);

    update_local_user();
    reconcile_names(captured, incoming_, game_);
    reconcile_names(captured, outgoing_, game_);

    // A request that has already produced a confirmed friendship is no longer
    // actionable even if an older invitation snapshot still contains it.
    GameBridge::RequestList filtered;
    for (std::size_t index = 0; index < captured.count; ++index) {
        if (!is_local_user(captured.entries[index].id) && !is_buddy(captured.entries[index].id)) {
            filtered.entries[filtered.count++] = captured.entries[index];
        }
    }

    const auto previous_outgoing = outgoing_;
    // Incoming wins when both directions exist for one account in these snapshots.
    for (std::size_t index = 0; index < filtered.count; ++index) {
        remove_request(outgoing_, filtered.entries[index].id);
    }

    if (filtered == incoming_ && outgoing_ == previous_outgoing) {
        return false;
    }

    incoming_ = filtered;
    return true;
}

bool Handler::consume_relationship() noexcept {
    const auto batch = relationship_mailbox_.consume();
    if (batch.dropped != 0) {
        logger_.warning("Galaxy relationship callbacks dropped: {}; the "
                        "callback queue was busy or full",
                        batch.dropped);
        callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
    }

    if (batch.count == 0) {
        return false;
    }

    update_local_user();
    const auto before_accepted = accepted_;
    const auto before_incoming = incoming_;
    const auto before_outgoing = outgoing_;
    for (std::size_t index = 0; index < batch.count; ++index) {
        const auto record = batch.records[index];
        if (is_local_user(record.user)) {
            // Galaxy can identify the signed-in user in a friend-added callback.
            // Remove self from every relationship view and overlay.
            clear_added(record.user);
            clear_removed(record.user);
            remove_accepted(record.user);
            remove_request(incoming_, record.user);
            remove_request(outgoing_, record.user);
            continue;
        }

        switch (record.change) {
        case RelationshipChange::Added:
            clear_removed(record.user);
            mark_added(record.user);
            add_accepted(record.user);
            remove_request(incoming_, record.user);
            remove_request(outgoing_, record.user);
            break;
        case RelationshipChange::Removed:
            clear_added(record.user);
            mark_removed(record.user);
            remove_accepted(record.user);
            remove_request(outgoing_, record.user);
            break;
        case RelationshipChange::InvitationCleared:
            remove_request(incoming_, record.user);
            break;
        case RelationshipChange::InvitationSent:
            clear_removed(record.user);
            add_outgoing(record.user);
            break;
        }
    }

    return accepted_ != before_accepted || incoming_ != before_incoming || outgoing_ != before_outgoing;
}

bool Handler::consume_pending_requests() noexcept {
    const auto before = outgoing_;
    const auto batch = pending_requests_.consume();
    for (std::size_t index = 0; index < batch.count; ++index) {
        const auto &completion = batch.completions[index];
        const GalaxyId user{completion.user};
        if (completion.state == PendingRequestState::SendSucceeded) {
            add_outgoing(user);
        } else if (completion.state == PendingRequestState::CancelSucceeded) {
            remove_request(outgoing_, user);
        } else if (completion.state == PendingRequestState::SendFailed) {
            logger_.warning("Galaxy could not send the buddy request for user {} (reason {})", user.value, completion.detail);
        } else {
            logger_.warning("Galaxy could not cancel the buddy request for user {} (reason {})", user.value, completion.detail);
        }
    }

    return outgoing_ != before;
}

bool Handler::update_accepted() noexcept {
    // Claim this completion before copying, so a concurrent success remains pending.
    accepted_response_ready_.exchange(false, std::memory_order_acq_rel);
    update_local_user();
    GameBridge::AcceptedList captured;
    if (!game_.capture_accepted(captured)) {
        accepted_response_ready_.store(true, std::memory_order_release);
        return false;
    }

    reconcile_names(captured, accepted_, game_);
    reconcile_names(captured, incoming_, game_);
    reconcile_names(captured, outgoing_, game_);
    preserve_lobbies(captured, accepted_);

    if (local_user_.value != 0) {
        clear_added(local_user_);
        clear_removed(local_user_);
    }

    const auto captured_end = captured.entries.begin() + captured.count;
    const auto contains = [captured_end, &captured](GalaxyId user) noexcept {
        return std::find_if(captured.entries.begin(), captured_end, [user](const auto &entry) {
                   return entry.id == user;
               }) != captured_end;
    };

    // Retire each callback overlay when the copied accepted cache reflects its change.
    // Until then, additions/removals override stale cache entries within the fixed capacity.
    for (std::size_t index = 0; index < added_count_;) {
        if (contains(added_[index])) {
            clear_added(added_[index]);
        } else {
            ++index;
        }
    }
    for (std::size_t index = 0; index < removed_count_;) {
        if (!contains(removed_[index])) {
            clear_removed(removed_[index]);
        } else {
            ++index;
        }
    }

    GameBridge::AcceptedList filtered;
    for (std::size_t index = 0; index < captured.count; ++index) {
        if (!is_local_user(captured.entries[index].id) && !is_removed(captured.entries[index].id)) {
            filtered.entries[filtered.count++] = captured.entries[index];
        }
    }

    // Preserve confirmed additions missing from a stale cache, reusing their copied
    // names and presence when available. Full snapshots can omit extra overlay entries.
    for (std::size_t index = 0; index < added_count_ && filtered.count < filtered.entries.size(); ++index) {
        const auto user = added_[index];
        const auto filtered_end = filtered.entries.begin() + filtered.count;
        const auto already_present = std::find_if(filtered.entries.begin(), filtered_end, [user](const auto &entry) {
            return entry.id == user;
        });
        if (already_present != filtered_end) {
            continue;
        }

        auto &entry = filtered.entries[filtered.count++];
        const auto current_end = accepted_.entries.begin() + accepted_.count;
        const auto current = std::find_if(accepted_.entries.begin(), current_end, [user](const auto &candidate) {
            return candidate.id == user;
        });
        if (current != current_end) {
            entry = *current;
        } else {
            entry.id = user;
            game_.copy_buddy_name(user, entry.name);
        }
    }

    // Accepted snapshots also retire matching process-local outgoing requests,
    // including friendships whose individual callback was delayed or dropped.
    const auto previous_outgoing = outgoing_;
    for (std::size_t index = 0; index < filtered.count; ++index) {
        remove_request(outgoing_, filtered.entries[index].id);
    }

    const auto was_initialized = accepted_initialized_;
    accepted_initialized_ = true;
    if (filtered == accepted_) {
        // Publish an empty first snapshot too, so a stable read can answer false
        // instead of treating the accepted list as uninitialized.
        return !was_initialized || outgoing_ != previous_outgoing;
    }

    accepted_ = filtered;
    return true;
}

void Handler::update_local_user() noexcept {
    if (const auto local = game_.local_user()) {
        local_user_ = *local;
    }
}

bool Handler::is_local_user(GalaxyId user) const noexcept {
    return user.value != 0 && local_user_.value != 0 && user == local_user_;
}

bool Handler::is_buddy(GalaxyId user) const noexcept {
    if (is_local_user(user) || is_removed(user)) {
        return false;
    }

    const auto found = std::find_if(accepted_.entries.begin(), accepted_.entries.begin() + accepted_.count, [user](const auto &entry) {
        return entry.id == user;
    });
    if (found != accepted_.entries.begin() + accepted_.count) {
        return true;
    }

    return !accepted_initialized_ && game_.is_accepted(user);
}

Handler::RelationshipState Handler::relationship_state(GalaxyId user) const noexcept {
    if (is_local_user(user) || user.value == 0) {
        return RelationshipState::None;
    }

    if (is_buddy(user)) {
        return RelationshipState::Accepted;
    }

    const auto contains = [user](const GameBridge::RequestList &requests) noexcept {
        return std::find_if(requests.entries.begin(), requests.entries.begin() + requests.count, [user](const auto &entry) {
                   return entry.id == user;
               }) != requests.entries.begin() + requests.count;
    };
    if (contains(incoming_)) {
        return RelationshipState::Incoming;
    }

    return contains(outgoing_) ? RelationshipState::Outgoing : RelationshipState::None;
}

std::optional<bool> Handler::published_buddy(GalaxyId user) const noexcept {
    // Odd or changed versions cause a retry. Each entry is atomic even when the
    // attempt is abandoned; two unsuccessful attempts leave the native fallback available.
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        const auto before = published_buddy_version_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }

        const auto ready = published_buddies_ready_.load(std::memory_order_relaxed);
        const auto count = std::min(published_buddy_count_.load(std::memory_order_relaxed), published_buddies_.size());
        bool found{};
        for (std::size_t index = 0; index < count; ++index) {
            if (published_buddies_[index].load(std::memory_order_relaxed) == user.value) {
                found = true;
                break;
            }
        }

        const auto after = published_buddy_version_.load(std::memory_order_acquire);
        if (before == after) {
            return ready ? std::optional{found} : std::nullopt;
        }
    }

    return std::nullopt;
}

void Handler::publish_buddies() noexcept {
    // Mark the write with an odd version, then publish an even version after the
    // account IDs, count and initialization flag have been stored.
    published_buddy_version_.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < accepted_.count; ++index) {
        published_buddies_[index].store(accepted_.entries[index].id.value, std::memory_order_relaxed);
    }

    published_buddy_count_.store(accepted_.count, std::memory_order_relaxed);
    published_buddies_ready_.store(accepted_initialized_, std::memory_order_relaxed);
    published_buddy_version_.fetch_add(1, std::memory_order_release);
}

bool Handler::is_removed(GalaxyId user) const noexcept {
    return std::find(removed_.begin(), removed_.begin() + removed_count_, user) != removed_.begin() + removed_count_;
}

bool Handler::is_confirmed_added(GalaxyId user) const noexcept {
    return std::find(added_.begin(), added_.begin() + added_count_, user) != added_.begin() + added_count_;
}

void Handler::mark_added(GalaxyId user) noexcept {
    if (user.value == 0 || is_local_user(user) || is_confirmed_added(user)) {
        return;
    }

    if (added_count_ < added_.size()) {
        added_[added_count_++] = user;
        return;
    }

    // When full, overwrite older additions in a ring while waiting for the accepted cache to catch up.
    added_[added_cursor_] = user;
    added_cursor_ = (added_cursor_ + 1) % added_.size();
}

void Handler::clear_added(GalaxyId user) noexcept {
    const auto found = std::find(added_.begin(), added_.begin() + added_count_, user);
    if (found == added_.begin() + added_count_) {
        return;
    }

    std::move(found + 1, added_.begin() + added_count_, found);
    --added_count_;
    added_[added_count_] = {};
}

void Handler::mark_removed(GalaxyId user) noexcept {
    if (user.value == 0 || is_local_user(user) || is_removed(user)) {
        return;
    }

    if (removed_count_ < removed_.size()) {
        removed_[removed_count_++] = user;
        return;
    }

    // When full, overwrite older removal overlays in a ring.
    removed_[removed_cursor_] = user;
    removed_cursor_ = (removed_cursor_ + 1) % removed_.size();
}

void Handler::clear_removed(GalaxyId user) noexcept {
    const auto found = std::find(removed_.begin(), removed_.begin() + removed_count_, user);
    if (found == removed_.begin() + removed_count_) {
        return;
    }

    std::move(found + 1, removed_.begin() + removed_count_, found);
    --removed_count_;
    removed_[removed_count_] = {};
}

void Handler::add_accepted(GalaxyId user) noexcept {
    accepted_initialized_ = true;
    if (user.value == 0 || is_local_user(user) || is_buddy(user) || accepted_.count >= accepted_.entries.size()) {
        return;
    }

    auto &entry = accepted_.entries[accepted_.count++];
    entry.id = user;
    if (!game_.copy_lobby_name(user, entry.name)) {
        game_.copy_buddy_name(user, entry.name);
    }

    reconcile_name(entry, incoming_, game_);
    reconcile_name(entry, outgoing_, game_);
}

void Handler::add_outgoing(GalaxyId user) noexcept {
    const auto incoming_end = incoming_.entries.begin() + incoming_.count;
    const auto incoming = std::find_if(incoming_.entries.begin(), incoming_end, [user](const auto &entry) {
        return entry.id == user;
    });
    const auto outgoing_end = outgoing_.entries.begin() + outgoing_.count;
    const auto existing = std::find_if(outgoing_.entries.begin(), outgoing_end, [user](const auto &entry) {
        return entry.id == user;
    });
    if (user.value == 0 || is_local_user(user) || is_buddy(user) || incoming != incoming_end || existing != outgoing_end ||
        outgoing_.count >= outgoing_.entries.size()) {
        return;
    }

    auto &entry = outgoing_.entries[outgoing_.count++];
    entry.id = user;
    if (!game_.copy_lobby_name(user, entry.name)) {
        game_.copy_buddy_name(user, entry.name);
    }
}

void Handler::remove_accepted(GalaxyId user) noexcept {
    const auto end = accepted_.entries.begin() + accepted_.count;
    const auto found = std::find_if(accepted_.entries.begin(), end, [user](const auto &entry) {
        return entry.id == user;
    });
    if (found == end) {
        return;
    }

    std::move(found + 1, end, found);
    --accepted_.count;
    accepted_.entries[accepted_.count] = {};
}

void Handler::remove_request(GameBridge::RequestList &requests, GalaxyId user) noexcept {
    const auto end = requests.entries.begin() + requests.count;
    const auto found = std::find_if(requests.entries.begin(), end, [user](const auto &entry) {
        return entry.id == user;
    });
    if (found == end) {
        return;
    }

    std::move(found + 1, end, found);
    --requests.count;
    requests.entries[requests.count] = {};
}

} // namespace fc_plugins::galaxy_buddies
