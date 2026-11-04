#include "handler.hpp"

#include <Windows.h>

namespace fc_plugins::galaxy_buddies {

void Handler::record_listener_failure() noexcept {
    const auto last_error = GetLastError();
    logger_.warning("Could not register the Galaxy buddy callbacks; registration will be retried");
    SetLastError(last_error);
}

void Handler::record_request_capacity() noexcept {
    const auto last_error = GetLastError();
    logger_.warning("Could not start another buddy request because all pending operation slots are occupied");
    SetLastError(last_error);
}

void Handler::record_join_failure(GalaxyId lobby) noexcept {
    const auto last_error = GetLastError();
    logger_.warning("Could not start joining Galaxy lobby {} for the selected buddy", lobby.value);
    SetLastError(last_error);
}

void Handler::record_accepted_request(bool submitted) noexcept {
    if (!submitted) {
        const auto last_error = GetLastError();
        logger_.warning("Could not submit an accepted buddy list request through the game");
        SetLastError(last_error);
    }
}

void Handler::record_operation(GalaxyOperation operation, const GameBridge::GalaxyCallResult &result) noexcept {
    // Accepted submissions can still fail through Galaxy listeners; accepted cached reads are already complete.
    if (result.accepted()) {
        return;
    }

    // Logging can call Win32; preserve the last error left by the callback or native caller.
    const auto last_error = GetLastError();
    const char *action{};
    switch (operation) {
    case GalaxyOperation::RequestAccepted:
        action = "request accepted buddies";
        break;
    case GalaxyOperation::RequestInvitations:
        action = "request incoming buddy invitations";
        break;
    case GalaxyOperation::SendInvitation:
        action = "send a buddy request";
        break;
    case GalaxyOperation::AcceptInvitation:
        action = "accept a buddy request";
        break;
    case GalaxyOperation::DeclineInvitation:
        action = "decline a buddy request";
        break;
    case GalaxyOperation::RespondInvitation:
        action = "respond to a buddy request";
        break;
    case GalaxyOperation::RemoveRelationship:
        action = "remove a buddy or cancel an outgoing request";
        break;
    case GalaxyOperation::PublishDisplayName:
        action = "publish the local display name";
        break;
    case GalaxyOperation::RequestDisplayName:
        action = "load a display name";
        break;
    case GalaxyOperation::RequestLobby:
        action = "load a buddy's lobby presence";
        break;
    }

    const auto user = result.user.value;
    switch (result.state) {
    case GameBridge::GalaxyCallState::Accepted:
        break;
    case GameBridge::GalaxyCallState::InterfaceUnavailable:
        if (user == 0) {
            logger_.warning("Could not {} because the Galaxy interface is unavailable", action);
        } else {
            logger_.warning("Could not {} for user {} because the Galaxy interface "
                            "is unavailable",
                            action, user);
        }
        break;
    case GameBridge::GalaxyCallState::NativeFailure:
        if (user == 0) {
            logger_.warning("Could not {} because the Galaxy call failed", action);
        } else {
            logger_.warning("Could not {} for user {} because the Galaxy call failed", action, user);
        }
        break;
    case GameBridge::GalaxyCallState::GalaxyError: {
        const auto *name = result.error_name.front() == '\0' ? "unnamed error" : result.error_name.data();
        const auto *message = result.error_message.front() == '\0' ? "no description provided" : result.error_message.data();
        if (user == 0) {
            logger_.warning("Galaxy could not {}: {} (type {}): {}", action, name, result.error_type, message);
        } else {
            logger_.warning("Galaxy could not {} for user {}: {} (type {}): {}", action, user, name, result.error_type, message);
        }
        break;
    }
    }

    SetLastError(last_error);
}

void Handler::record_completion_failure(GalaxyOperation operation, GalaxyId user, std::uint32_t detail) noexcept {
    const auto last_error = GetLastError();
    const char *action = "complete the buddy operation";
    switch (operation) {
    case GalaxyOperation::RequestAccepted:
        action = "load accepted buddies";
        break;
    case GalaxyOperation::RequestInvitations:
        action = "load incoming buddy requests";
        break;
    case GalaxyOperation::SendInvitation:
        action = "send the buddy request";
        break;
    case GalaxyOperation::RespondInvitation:
    case GalaxyOperation::AcceptInvitation:
    case GalaxyOperation::DeclineInvitation:
        action = "respond to the buddy request";
        break;
    case GalaxyOperation::RemoveRelationship:
        action = "remove the buddy or cancel the outgoing request";
        break;
    case GalaxyOperation::PublishDisplayName:
    case GalaxyOperation::RequestDisplayName:
    case GalaxyOperation::RequestLobby:
        break;
    }
    if (user.value == 0) {
        logger_.warning("Galaxy could not {} (reason {})", action, detail);
    } else {
        logger_.warning("Galaxy could not {} for user {} (reason {})", action, user.value, detail);
    }
    SetLastError(last_error);
}

} // namespace fc_plugins::galaxy_buddies
