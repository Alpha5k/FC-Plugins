#include "handler.hpp"

#include <algorithm>
#include <array>

namespace fc_plugins::galaxy_buddies {
void Handler::schedule_display_name_refresh() noexcept {
    display_name_refresh_active_ = true;
    display_name_initial_request_ = true;
    display_name_retries_remaining_ = kDisplayNameRetryLimit;
    display_name_request_warning_recorded_ = false;
    display_name_publish_warning_recorded_ = false;
    next_display_name_refresh_ = {};
}

void Handler::service_display_names() noexcept {
    if (!listeners_registered_ || !display_name_refresh_active_) {
        return;
    }

    const auto now = Clock::now();
    if (now < next_display_name_refresh_) {
        return;
    }

    if (!game_.logged_on()) {
        // Logged-off waits consume the same retry budget, at the one-second listener interval.
        if (display_name_retries_remaining_ == 0) {
            display_name_refresh_active_ = false;
            return;
        }

        --display_name_retries_remaining_;
        next_display_name_refresh_ = now + kListenerRetryInterval;
        return;
    }

    // Read cached values before issuing another request. Galaxy 1.112 can
    // satisfy an unchanged user-data request without another callback.
    publish_display_name();
    refresh_display_names();

    const auto resolved = [](const auto &entry) {
        return entry.published_name;
    };
    const auto all_resolved = std::all_of(accepted_.entries.begin(), accepted_.entries.begin() + accepted_.count, resolved) &&
                              std::all_of(incoming_.entries.begin(), incoming_.entries.begin() + incoming_.count, resolved) &&
                              std::all_of(outgoing_.entries.begin(), outgoing_.entries.begin() + outgoing_.count, resolved);

    if (display_name_initial_request_) {
        // Screen entry and relationship changes request every visible name once.
        // Later attempts request unresolved names, with at most 16 retries scheduled 500 ms apart.
        request_display_names(false);
        display_name_initial_request_ = false;
        next_display_name_refresh_ = now + kDisplayNameRetryInterval;
        return;
    }

    if (all_resolved && !publish_pending_) {
        display_name_refresh_active_ = false;
        return;
    }
    if (display_name_retries_remaining_ == 0) {
        display_name_refresh_active_ = false;
        return;
    }

    request_display_names(true);
    --display_name_retries_remaining_;
    next_display_name_refresh_ = now + kDisplayNameRetryInterval;
}

void Handler::consume_display_name_updates() noexcept {
    if (!user_data_ready_.load(std::memory_order_acquire) || user_data_lock_.test_and_set(std::memory_order_acquire)) {
        return;
    }

    // Copy the account IDs and release the callback lock before reading Galaxy or logging.
    // Callbacks can enqueue the next batch while this one is processed.
    std::array<GalaxyId, kUserDataCallbackCapacity> callbacks;
    const auto callback_count = user_data_count_;
    std::copy_n(user_data_mailbox_.begin(), callback_count, callbacks.begin());
    user_data_count_ = 0;
    user_data_ready_.store(false, std::memory_order_release);
    user_data_lock_.clear(std::memory_order_release);

    if (const auto dropped = dropped_user_data_callbacks_.exchange(0, std::memory_order_acq_rel); dropped != 0) {
        logger_.warning("Display name callbacks dropped: {}; the callback "
                        "queue was busy or full",
                        dropped);
    }

    if (const auto local = game_.local_user()) {
        local_user_ = *local;
    }

    for (std::size_t index = 0; index < callback_count; ++index) {
        const auto user = callbacks[index];
        const auto local_callback = user.value == 0 || (local_user_.value != 0 && user == local_user_);
        // Zero identifies a local completion too; other IDs refresh only that account's rows.
        if (local_callback) {
            verify_published_name();
        } else {
            refresh_display_name(user);
        }
    }
}

void Handler::refresh_display_names() noexcept {
    for (std::size_t index = 0; index < accepted_.count; ++index) {
        refresh_display_name(accepted_.entries[index].id);
    }
    for (std::size_t index = 0; index < incoming_.count; ++index) {
        refresh_display_name(incoming_.entries[index].id);
    }
    for (std::size_t index = 0; index < outgoing_.count; ++index) {
        refresh_display_name(outgoing_.entries[index].id);
    }
}

void Handler::refresh_display_name(GalaxyId user) noexcept {
    const auto accepted_end = accepted_.entries.begin() + accepted_.count;
    const auto accepted = std::find_if(accepted_.entries.begin(), accepted_end, [user](const auto &entry) {
        return entry.id == user;
    });
    const auto incoming_end = incoming_.entries.begin() + incoming_.count;
    const auto incoming = std::find_if(incoming_.entries.begin(), incoming_end, [user](const auto &entry) {
        return entry.id == user;
    });
    const auto outgoing_end = outgoing_.entries.begin() + outgoing_.count;
    const auto outgoing = std::find_if(outgoing_.entries.begin(), outgoing_end, [user](const auto &entry) {
        return entry.id == user;
    });
    if (accepted == accepted_end && incoming == incoming_end && outgoing == outgoing_end) {
        return;
    }

    // Published user data outranks persona/lobby fallback names. Missing or generated
    // values leave the retained name intact so a later retry can improve it.
    const auto read = game_.read_display_name(user);
    if (!read.result.accepted()) {
        return;
    }
    if (!read.present) {
        return;
    }
    if (is_placeholder_name(read.name)) {
        return;
    }

    const auto apply = [&read](auto &entry) noexcept {
        if (!entry.published_name || entry.name != read.name) {
            entry.name = read.name;
            entry.published_name = true;
        }
    };
    if (accepted != accepted_end) {
        apply(*accepted);
    }
    if (incoming != incoming_end) {
        apply(*incoming);
    }
    if (outgoing != outgoing_end) {
        apply(*outgoing);
    }
}

void Handler::verify_published_name() noexcept {
    if (!publish_pending_) {
        return;
    }

    // Successful SetUserData submission becomes confirmed only after an exact cached readback.
    const auto read = game_.read_display_name();
    if (!read.result.accepted()) {
        return;
    }
    if (!read.present) {
        return;
    }
    if (read.name != pending_publish_name_) {
        return;
    }

    published_name_ = read.name;
    pending_publish_name_.fill('\0');
    publish_pending_ = false;
}

void Handler::request_display_names(bool unresolved_only) noexcept {
    if (!game_.logged_on()) {
        return;
    }

    const auto request = [this](GalaxyId user) noexcept {
        record_display_name_request(game_.request_display_name(user));
    };
    for (std::size_t index = 0; index < accepted_.count; ++index) {
        if (!unresolved_only || !accepted_.entries[index].published_name) {
            request(accepted_.entries[index].id);
        }
    }
    for (std::size_t index = 0; index < incoming_.count; ++index) {
        if (!unresolved_only || !incoming_.entries[index].published_name) {
            request(incoming_.entries[index].id);
        }
    }
    for (std::size_t index = 0; index < outgoing_.count; ++index) {
        if (!unresolved_only || !outgoing_.entries[index].published_name) {
            request(outgoing_.entries[index].id);
        }
    }
}

void Handler::record_display_name_request(const GameBridge::GalaxyCallResult &result) noexcept {
    if (!result.accepted() && !display_name_request_warning_recorded_) {
        display_name_request_warning_recorded_ = true;
        record_operation(GalaxyOperation::RequestDisplayName, result);
    }
}

void Handler::publish_display_name() noexcept {
    std::array<char, GameBridge::kNameCapacity> current;
    if (!game_.copy_local_display_name(current)) {
        return;
    }
    if (is_placeholder_name(current)) {
        return;
    }
    if (current == published_name_) {
        return;
    }

    if (const auto local = game_.local_user()) {
        local_user_ = *local;
    }

    if (publish_pending_ && current == pending_publish_name_) {
        // Recheck an in-flight value before submitting it again; request the local cache if needed.
        verify_published_name();
        if (!publish_pending_) {
            return;
        }
        if (local_user_.value != 0) {
            record_display_name_request(game_.request_display_name(local_user_));
        }
        return;
    }

    const auto result = game_.publish_display_name(current.data());
    if (!result.accepted()) {
        if (!display_name_publish_warning_recorded_) {
            display_name_publish_warning_recorded_ = true;
            record_operation(GalaxyOperation::PublishDisplayName, result);
        }
        return;
    }

    pending_publish_name_ = current;
    publish_pending_ = true;

    verify_published_name();
    if (!publish_pending_) {
        return;
    }

    // SetUserData may suppress its callback when the same value already exists.
    // Request the local data so callback or cached readback can confirm the submitted value.
    if (local_user_.value != 0) {
        record_display_name_request(game_.request_display_name(local_user_));
    }
}

} // namespace fc_plugins::galaxy_buddies
