#include "handler.hpp"

#include <algorithm>

namespace fc_plugins::galaxy_buddies {
void Handler::FriendListListener::success() noexcept {
    owner_.on_friend_list_success();
}

void Handler::FriendListListener::failure(std::uint32_t reason) noexcept {
    owner_.record_completion_failure(GalaxyOperation::RequestAccepted, {}, reason);
}

void Handler::InvitationListListener::success() noexcept {
    owner_.on_invitation_list_success();
}

void Handler::InvitationListListener::failure(std::uint32_t reason) noexcept {
    owner_.record_completion_failure(GalaxyOperation::RequestInvitations, {}, reason);
}

void Handler::PersonaStateListener::changed(GalaxyId, std::uint32_t) noexcept {
    // Coalesce per-user notifications into a full accepted-buddy presence read
    // on the next native update; the callback leaves mutable snapshots alone.
    owner_.callback_requests_.fetch_or(kRefreshPresence | kRefreshLobbies, std::memory_order_release);
}

void Handler::RichPresenceListener::updated(GalaxyId) noexcept {
    owner_.callback_requests_.fetch_or(kRefreshLobbies, std::memory_order_release);
}

void Handler::SendListener::success(GalaxyId user) noexcept {
    // Galaxy 1.112 cannot enumerate sent requests, so only a confirmed send may
    // enter the process-local outgoing list.
    if (!owner_.complete_pending_request(user, PendingRequestState::Sending, PendingRequestState::SendSucceeded)) {
        owner_.publish_relationship(RelationshipChange::InvitationSent, user);
    }

    // Sending can accept an invitation already received from that user, so its successful
    // callback is the point at which the relationship lists may have changed.
    owner_.callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
}

void Handler::SendListener::failure(GalaxyId user, std::uint32_t reason) noexcept {
    if (!owner_.complete_pending_request(user, PendingRequestState::Sending, PendingRequestState::SendFailed, reason)) {
        owner_.record_completion_failure(GalaxyOperation::SendInvitation, user, reason);
    }
}

void Handler::InvitationListener::received(GalaxyId, std::uint32_t) noexcept {
    // Galaxy may repeat this notification until the invitation list is read.
    // Request a complete snapshot rather than appending another row for each notification.
    owner_.callback_requests_.fetch_or(kRequestInvitations, std::memory_order_release);
}

void Handler::RespondListener::success(GalaxyId user, bool accepted) noexcept {
    owner_.publish_relationship(accepted ? RelationshipChange::Added : RelationshipChange::InvitationCleared, user);
    owner_.callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
}

void Handler::RespondListener::failure(GalaxyId user, std::uint32_t reason) noexcept {
    owner_.record_completion_failure(GalaxyOperation::RespondInvitation, user, reason);
}

void Handler::FriendAddListener::added(GalaxyId user, std::uint32_t) noexcept {
    owner_.publish_relationship(RelationshipChange::Added, user);
    owner_.callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
}

void Handler::FriendDeleteListener::success(GalaxyId user) noexcept {
    if (!owner_.complete_pending_request(user, PendingRequestState::Canceling, PendingRequestState::CancelSucceeded)) {
        owner_.publish_relationship(RelationshipChange::Removed, user);
    }

    owner_.callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
}

void Handler::FriendDeleteListener::failure(GalaxyId user, std::uint32_t reason) noexcept {
    if (!owner_.complete_pending_request(user, PendingRequestState::Canceling, PendingRequestState::CancelFailed, reason)) {
        owner_.record_completion_failure(GalaxyOperation::RemoveRelationship, user, reason);
    }
}

void Handler::SpecificUserDataListener::updated(GalaxyId user) noexcept {
    owner_.publish_user_data_callback(user);
}

void Handler::on_friend_list_success() noexcept {
    // The game's listener marks its Galaxy-owned accepted cache ready in the same dispatch.
    // Defer readback until native update so listener order does not decide readiness.
    accepted_response_ready_.store(true, std::memory_order_release);
    callback_requests_.fetch_or(kRequestInvitations, std::memory_order_release);
}

void Handler::on_invitation_list_success() noexcept {
    // Galaxy exposes invitation entries during success dispatch; copy them before returning.
    GameBridge::RequestList captured;
    const auto result = game_.capture_incoming(captured);
    if (!result.accepted()) {
        record_operation(GalaxyOperation::RequestInvitations, result);
        return;
    }

    publish_incoming(captured);
}

void Handler::publish_incoming(const GameBridge::RequestList &incoming) noexcept {
    if (incoming_lock_.test_and_set(std::memory_order_acquire)) {
        dropped_incoming_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Keep only the latest full snapshot; the native update consumes its copied entries.
    incoming_mailbox_ = incoming;
    incoming_ready_.store(true, std::memory_order_release);
    incoming_lock_.clear(std::memory_order_release);
}

void Handler::publish_relationship(RelationshipChange change, GalaxyId user) noexcept {
    relationship_mailbox_.publish({.change = change, .user = user});
}

bool Handler::reserve_pending_request(GalaxyId user, PendingRequestState pending) noexcept {
    const auto result = pending_requests_.reserve(user.value, pending);
    if (result == state::ReserveResult::Reserved) {
        return true;
    }

    if (result == state::ReserveResult::Full) {
        record_request_capacity();
    }

    return false;
}

void Handler::release_pending_request(GalaxyId user, PendingRequestState pending) noexcept {
    pending_requests_.release(user.value, pending);
}

bool Handler::complete_pending_request(GalaxyId user, PendingRequestState pending, PendingRequestState completed,
                                       std::uint32_t detail) noexcept {
    return pending_requests_.complete(user.value, pending, completed, detail);
}

void Handler::publish_user_data_callback(GalaxyId user) noexcept {
    if (user_data_lock_.test_and_set(std::memory_order_acquire)) {
        dropped_user_data_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Repeated completions need only one cached read per account on the native update.
    const auto duplicate = std::find(user_data_mailbox_.begin(), user_data_mailbox_.begin() + user_data_count_, user);
    if (duplicate == user_data_mailbox_.begin() + user_data_count_) {
        if (user_data_count_ < user_data_mailbox_.size()) {
            user_data_mailbox_[user_data_count_++] = user;
        } else {
            dropped_user_data_callbacks_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    user_data_ready_.store(user_data_count_ != 0, std::memory_order_release);
    user_data_lock_.clear(std::memory_order_release);
}

} // namespace fc_plugins::galaxy_buddies
