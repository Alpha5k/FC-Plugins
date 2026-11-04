#pragma once

#include "broker.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "state.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <mutex>
#include <fc/lua/bindings.hpp>

namespace fc_plugins::galaxy_buddies {

// Maintains bounded Galaxy relationship/name snapshots for modified buddy screens
// and publishes accepted account IDs for native HUD, voice and player consumers.
class Handler final {
  public:
    // Binds listener storage to this handler and retains the patch logger.
    explicit Handler(const fc::CreateContext &context) noexcept;
    // Unregisters through the current registrar before destroying listener storage.
    // Galaxy retains raw pointers; this call alone does not establish callback quiescence.
    ~Handler();

    // Declares native buddy operations and the supported Steam broker hook for activation.
    void plan(fc::Plan &plan);
    // Registers wrappers that use the invoking Lua VM; no Frame values survive a call.
    fc::Result<> prepare(fc::PrepareContext &context);
    // Services game-owned caches and Galaxy facades after the game state update, on its native thread.
    void native_update() noexcept;

  private:
    // Selects optional broker startup operations for the required peer image.
    void plan_broker(fc::Plan &peer);
    // Captures the borrowed context before original completion, then starts its native broker.
    void finish_sign_in(void *operation) noexcept;

    // Identifies the Galaxy action described by submission and completion failures.
    enum class GalaxyOperation : std::uint8_t {
        RequestAccepted,
        RequestInvitations,
        SendInvitation,
        AcceptInvitation,
        DeclineInvitation,
        RespondInvitation,
        RemoveRelationship,
        PublishDisplayName,
        RequestDisplayName,
        RequestLobby,
    };

    // Describes a relationship change copied from a Galaxy callback. The native update
    // applies the change and reconciles the authoritative lists when needed.
    enum class RelationshipChange : std::uint8_t {
        Added,
        Removed,
        InvitationCleared,
        InvitationSent,
    };

    using PendingRequestState = state::PendingRequestState;

    struct RelationshipRecord {
        RelationshipChange change{RelationshipChange::Added};
        GalaxyId user;
    };

    // Values correspond to the relationship icons retained in lobby_icons.
    enum class RelationshipState : std::uint8_t {
        None,
        Accepted,
        Incoming,
        Outgoing,
    };

    // These are the facade listener IDs in the bundled Galaxy 1.112
    // ABI. Newer SDK headers assign different meanings to the same values.
    static constexpr std::uint32_t kFriendListListener = 23;
    static constexpr std::uint32_t kPersonaStateListener = 24;
    static constexpr std::uint32_t kRichPresenceListener = 27;
    static constexpr std::uint32_t kSendListener = 46;
    static constexpr std::uint32_t kInvitationListListener = 47;
    static constexpr std::uint32_t kInvitationListener = 48;
    static constexpr std::uint32_t kRespondListener = 49;
    static constexpr std::uint32_t kFriendAddListener = 50;
    static constexpr std::uint32_t kFriendDeleteListener = 51;
    static constexpr std::uint32_t kSpecificUserDataListener = 25;
    using Clock = std::chrono::steady_clock;
    static constexpr auto kListenerRetryInterval = std::chrono::seconds{1};
    static constexpr auto kDisplayNameRetryInterval = std::chrono::milliseconds{500};
    static constexpr std::uint32_t kDisplayNameRetryLimit = 16;
    static constexpr std::uint32_t kRequestInvitations = 1U << 0U;
    static constexpr std::uint32_t kRequestRelationships = 1U << 1U;
    static constexpr std::uint32_t kRefreshPresence = 1U << 2U;
    static constexpr std::uint32_t kRefreshLobbies = 1U << 3U;
    static constexpr std::size_t kRemovedCapacity = 32;
    static constexpr std::size_t kUserDataCallbackCapacity = 32;
    static constexpr std::size_t kEventMailboxCapacity = 32;
    static constexpr std::size_t kPendingRequestCapacity = 16;

    class FriendListListener final {
      public:
        explicit FriendListListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~FriendListListener() = default;
        // Schedules readback of the completed accepted cache or reports its failure.
        virtual void success() noexcept;
        virtual void failure(std::uint32_t reason) noexcept;

      private:
        Handler &owner_;
    };

    class InvitationListListener final {
      public:
        explicit InvitationListListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~InvitationListListener() = default;
        // Copies the callback-scoped invitation cache or reports its failure.
        virtual void success() noexcept;
        virtual void failure(std::uint32_t reason) noexcept;

      private:
        Handler &owner_;
    };

    // Defers Galaxy persona events to one native-update-owned presence refresh.
    class PersonaStateListener final {
      public:
        explicit PersonaStateListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~PersonaStateListener() = default;
        // Coalesces persona notifications into a native-thread presence read.
        virtual void changed(GalaxyId user, std::uint32_t state) noexcept;

      private:
        Handler &owner_;
    };

    // Defers remote `connect` updates to the snapshot owned by the native update.
    class RichPresenceListener final {
      public:
        explicit RichPresenceListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~RichPresenceListener() = default;
        // Schedules native-thread readback of cached connect values.
        virtual void updated(GalaxyId user) noexcept;

      private:
        Handler &owner_;
    };

    class SendListener final {
      public:
        explicit SendListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~SendListener() = default;
        // Completes a reserved send or queues an unmatched confirmed send; failures report their reason.
        virtual void success(GalaxyId user) noexcept;
        virtual void failure(GalaxyId user, std::uint32_t reason) noexcept;

      private:
        Handler &owner_;
    };

    class InvitationListener final {
      public:
        explicit InvitationListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~InvitationListener() = default;
        // Requests a complete invitation snapshot after a new notification.
        virtual void received(GalaxyId user, std::uint32_t sent_at) noexcept;

      private:
        Handler &owner_;
    };

    class RespondListener final {
      public:
        explicit RespondListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~RespondListener() = default;
        // Publishes the confirmed response or reports its failure.
        virtual void success(GalaxyId user, bool accepted) noexcept;
        virtual void failure(GalaxyId user, std::uint32_t reason) noexcept;

      private:
        Handler &owner_;
    };

    class FriendAddListener final {
      public:
        explicit FriendAddListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~FriendAddListener() = default;
        // Publishes a confirmed friendship and schedules cache reconciliation.
        virtual void added(GalaxyId user, std::uint32_t direction) noexcept;

      private:
        Handler &owner_;
    };

    class FriendDeleteListener final {
      public:
        explicit FriendDeleteListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~FriendDeleteListener() = default;
        // Completes a reserved cancellation or publishes a confirmed removal.
        virtual void success(GalaxyId user) noexcept;
        virtual void failure(GalaxyId user, std::uint32_t reason) noexcept;

      private:
        Handler &owner_;
    };

    class SpecificUserDataListener final {
      public:
        explicit SpecificUserDataListener(Handler &owner) noexcept : owner_(owner) {}

        virtual ~SpecificUserDataListener() = default;
        // Copies the completed identity for native-thread display-name readback.
        virtual void updated(GalaxyId user) noexcept;

      private:
        Handler &owner_;
    };

    // Accepts finite int32-range numbers (truncating fractions) or whole decimal strings.
    static std::optional<std::int32_t> number(fc::lua::Value value);
    // Converts the invoking VM's one-based selection into accepted + incoming + outgoing.
    std::optional<std::size_t> selected_buddy(fc::lua::Frame &frame) const;
    // Resolves a retained row's decimal account token against the current relationships and local account.
    std::optional<std::size_t> action_buddy(fc::lua::Value row) const;
    // Publishes changed combined rows and retains the selected account on the invoking VM.
    void refresh(fc::lua::Frame &frame);
    // Defers native publication during wrapped Begin/UpdateFriends; preserves original calls elsewhere.
    void publish_friends() noexcept;
    // Captures the displayed lobby account when the retained native popup opens.
    void capture_lobby_action(fc::lua::Frame &frame);
    // Preserves native mute, boot and guest results; replaces buddy/can-add and appends state.
    void lobby_flags(fc::lua::Frame &frame);
    // Sends or deletes the captured lobby account after checking its current slot and displayed name.
    void lobby_action(fc::lua::Frame &frame);
    // Returns the selected relationship's state as the game's Unicode value.
    void buddy_state(fc::lua::Frame &frame);
    // Returns the selected relationship's applicable action keys.
    void buddy_actions(fc::lua::Frame &frame);
    // Uses a retained account token for actions and queues JOIN through the native special-join flow.
    void do_buddy_action(fc::lua::Frame &frame);
    // Maps the current native player slot to an account for HUD/voice buddy consumers.
    // Falls back to the native predicate when mapping or snapshot reads are unavailable.
    bool player_buddy(std::int32_t local_player, std::int32_t player) noexcept;
    // Prevents an explicit buddy join from selecting an unrelated lobby that
    // happened to appear first in Galaxy's unfiltered server results.
    void add_lobby(const GalaxyId *lobby) noexcept;

    // Attempts to register all listeners, unregistering earlier entries if a call fails.
    bool ensure_listeners() noexcept;
    // Attempts to remove all listeners through the current Galaxy registrar.
    void unregister_listeners() noexcept;
    // Requests accepted buddies first; success schedules invitations.
    void request_relationships() noexcept;
    // Coalesces callback notifications into native-thread Galaxy reads.
    void service_callback_requests() noexcept;
    // Requests each accepted user's current connect presence.
    void request_lobbies() noexcept;
    // Starts bounded retries for current user-data display names.
    void schedule_display_name_refresh() noexcept;
    // Services scheduled user-data reads and bounded retries.
    void service_display_names() noexcept;
    // Applies copied completions and publishes changes, including the accepted list copied before requests.
    void reconcile_state(bool relationships_changed) noexcept;
    // Reconciles the latest invitation snapshot copied during its completion callback.
    bool consume_incoming() noexcept;
    // Applies confirmed relationship changes and retires matching outgoing entries.
    bool consume_relationship() noexcept;
    // Copies Send and Cancel results and releases their slots on the native update.
    // Applies those copied results to the process-local outgoing list.
    bool consume_pending_requests() noexcept;
    // Copies and reconciles a completed accepted list; failed capture keeps completion pending.
    // Returns whether publication or visible relationships changed.
    bool update_accepted() noexcept;
    // Reads user-data completions for their exact user IDs.
    void consume_display_name_updates() noexcept;
    // Refreshes every visible relationship from Galaxy's cached user data.
    void refresh_display_names() noexcept;
    // Applies one available published name to all matching relationship entries.
    void refresh_display_name(GalaxyId user) noexcept;
    // Confirms the pending local name before treating publication as complete.
    void verify_published_name() noexcept;
    // Requests user data for visible relationships, optionally skipping resolved names.
    void request_display_names(bool unresolved_only) noexcept;
    // Automatic name retries report one representative failure per refresh.
    void record_display_name_request(const GameBridge::GalaxyCallResult &result) noexcept;
    // Publishes the current local platform name through Galaxy user data.
    void publish_display_name() noexcept;

    // Tracks the authenticated Galaxy identity used to reject self-invitations.
    void update_local_user() noexcept;
    // Matches a nonzero account ID against the last successfully read local account.
    bool is_local_user(GalaxyId user) const noexcept;
    // Uses reconciled relationships, with native accepted-cache fallback until initialized.
    bool is_buddy(GalaxyId user) const noexcept;
    // Returns the accepted, incoming, or process-local outgoing relationship.
    RelationshipState relationship_state(GalaxyId user) const noexcept;
    // Reads a stable accepted-ID publication in at most two attempts; unavailable
    // or changing snapshots return nullopt so the native hook can use its fallback.
    std::optional<bool> published_buddy(GalaxyId user) const noexcept;
    // Publishes accepted account IDs and initialization state for native buddy consumers.
    void publish_buddies() noexcept;
    // Tests whether a confirmed addition still overrides a stale accepted cache.
    bool is_confirmed_added(GalaxyId user) const noexcept;
    // Tests whether a confirmed removal still suppresses a stale accepted-cache entry.
    bool is_removed(GalaxyId user) const noexcept;
    // Temporarily retains a confirmed addition until the native cache catches up.
    void mark_added(GalaxyId user) noexcept;
    // Retires a confirmed addition reflected in the completed cache.
    void clear_added(GalaxyId user) noexcept;
    // Suppresses a confirmed removal while the native cache still contains it.
    void mark_removed(GalaxyId user) noexcept;
    // Retires a removal once the authoritative cache no longer contains it.
    void clear_removed(GalaxyId user) noexcept;
    // Adds a confirmed accepted relationship with the best available display name.
    void add_accepted(GalaxyId user) noexcept;
    // Adds a confirmed send to Galaxy 1.112's session-only outgoing view.
    void add_outgoing(GalaxyId user) noexcept;
    // Removes one accepted relationship without changing unrelated entries.
    void remove_accepted(GalaxyId user) noexcept;
    // Removes one matching invitation from a compact snapshot.
    static void remove_request(GameBridge::RequestList &requests, GalaxyId user) noexcept;

    // Defers accepted-cache readback until the native update, after the game's listener runs.
    void on_friend_list_success() noexcept;
    // Copies invitations during success dispatch, while their Galaxy cache is available.
    void on_invitation_list_success() noexcept;
    // Replaces the pending invitation snapshot, or counts a drop if its lock is busy.
    void publish_incoming(const GameBridge::RequestList &incoming) noexcept;
    // Copies one account change into the bounded callback mailbox.
    void publish_relationship(RelationshipChange change, GalaxyId user) noexcept;
    // Send and Cancel reserve a slot before calling Galaxy because its SDK
    // cannot reconstruct outgoing requests after a missed callback.
    bool reserve_pending_request(GalaxyId user, PendingRequestState pending) noexcept;
    // Releases the matching request when Galaxy immediately rejects it.
    void release_pending_request(GalaxyId user, PendingRequestState pending) noexcept;
    // Records only a completion matching the reserved user and operation.
    bool complete_pending_request(GalaxyId user, PendingRequestState pending, PendingRequestState completed,
                                  std::uint32_t detail = 0) noexcept;
    // Copies completed user IDs into the native-thread name mailbox.
    void publish_user_data_callback(GalaxyId user) noexcept;
    // Reports an asynchronous Galaxy failure with its user and reason, preserving Win32 last error.
    void record_completion_failure(GalaxyOperation operation, GalaxyId user = {}, std::uint32_t detail = 0) noexcept;
    // Reports an immediate call failure using its copied Galaxy error, preserving Win32 last error.
    void record_operation(GalaxyOperation operation, const GameBridge::GalaxyCallResult &result) noexcept;
    // Records a rejected accepted-list request without changing the visible cache.
    void record_accepted_request(bool submitted) noexcept;
    // Reports the handler's first callback-registration failure.
    void record_listener_failure() noexcept;
    // Reports a refused reservation when all pending-operation slots are occupied.
    void record_request_capacity() noexcept;
    // Reports a native join that could not be started for the requested lobby.
    void record_join_failure(GalaxyId lobby) noexcept;

    GameBridge game_;
    fc::lua::Bindings lua_;
    fc::NativeFunction<IsPlayerBuddy> original_player_buddy_;
    fc::NativeFunction<GetPlayer> get_player_;
    fc::NativeFunction<AddDiscoveredLobby> original_add_lobby_;
    fc::NativeFunction<PublishFriends> original_publish_friends_;
    // Scoped to synchronous original Begin/UpdateFriends calls under state_mutex_; no Lua Frame is retained.
    bool suppress_friends_publication_{};
    fc::NativeFunction<OperationContext> operation_context_;
    fc::NativeFunction<StartSessionBroker> start_session_broker_;
    fc::NativeFunction<FinishSignIn> original_finish_sign_in_;
    std::atomic_bool missing_broker_context_reported_{};
    // Native updates and Lua wrappers share snapshots; repaint can re-enter a state wrapper.
    std::recursive_mutex state_mutex_;
    GameBridge::AcceptedList accepted_;
    GameBridge::RequestList incoming_;
    // Galaxy 1.112 cannot enumerate sent requests. This process-local list
    // contains only invitations confirmed during the current process.
    GameBridge::RequestList outgoing_;
    // A completed snapshot, including an empty one, ends is_buddy's native accepted-cache fallback.
    bool accepted_initialized_{};
    // Records registration success without retaining registrar identity or account generation.
    bool listeners_registered_{};
    Clock::time_point next_listener_retry_{};
    Clock::time_point next_display_name_refresh_{};
    std::uint32_t display_name_retries_remaining_{};
    bool display_name_refresh_active_{};
    bool display_name_initial_request_{};

    FriendListListener friend_list_listener_;
    InvitationListListener invitation_list_listener_;
    PersonaStateListener persona_state_listener_;
    RichPresenceListener rich_presence_listener_;
    SendListener send_listener_;
    InvitationListener invitation_listener_;
    RespondListener respond_listener_;
    FriendAddListener friend_add_listener_;
    FriendDeleteListener friend_delete_listener_;
    SpecificUserDataListener specific_user_data_listener_;

    // Local user-data value confirmed by cached readback; pending holds the submitted value.
    std::array<char, GameBridge::kNameCapacity> published_name_{};
    std::array<char, GameBridge::kNameCapacity> pending_publish_name_{};
    GalaxyId local_user_{};
    bool publish_pending_{};

    // Confirmed callbacks temporarily override a stale game cache. Matching snapshots
    // retire overlays; full rings replace older entries within the fixed capacity.
    std::array<GalaxyId, kRemovedCapacity> removed_{};
    std::size_t removed_count_{};
    std::size_t removed_cursor_{};
    std::array<GalaxyId, kRemovedCapacity> added_{};
    std::size_t added_count_{};
    std::size_t added_cursor_{};

    // Native buddy consumers read atomic account IDs with an odd/even version check.
    // Readers reject versions that change during a read; native slot-to-account reads remain game-owned.
    std::array<std::atomic_uint64_t, GameBridge::kAcceptedCapacity> published_buddies_{};
    std::atomic_size_t published_buddy_count_{};
    std::atomic_uint32_t published_buddy_version_{};
    std::atomic_bool published_buddies_ready_{};
    // Accepted success schedules readback after the game has dispatched its own listener.
    std::atomic_bool accepted_response_ready_{};

    // Latest invitation snapshot copied inside callback dispatch, guarded by a nonblocking lock.
    std::atomic_flag incoming_lock_ = ATOMIC_FLAG_INIT;
    std::atomic_bool incoming_ready_{};
    std::atomic_uint32_t dropped_incoming_callbacks_{};
    GameBridge::RequestList incoming_mailbox_;

    state::Mailbox<RelationshipRecord, kEventMailboxCapacity> relationship_mailbox_;

    // Galaxy 1.112 cannot enumerate sent requests. Reserve these slots before
    // Send or Cancel so their completions cannot be lost to mailbox pressure.
    state::PendingRequests<kPendingRequestCapacity> pending_requests_;

    // The specific-user-data listener can complete several requests in one
    // Galaxy pump. This bounded mailbox retains their IDs for exact readback.
    std::atomic_flag user_data_lock_ = ATOMIC_FLAG_INIT;
    std::atomic_bool user_data_ready_{};
    std::array<GalaxyId, kUserDataCallbackCapacity> user_data_mailbox_{};
    std::size_t user_data_count_{};
    std::atomic_uint32_t dropped_user_data_callbacks_{};

    static_assert(std::atomic_bool::is_always_lock_free);
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    // Callbacks coalesce cache/request work for the next native update through these bits.
    std::atomic_uint32_t callback_requests_{};
    std::atomic_bool listener_warning_claimed_{};
    bool display_name_request_warning_recorded_{};
    bool display_name_publish_warning_recorded_{};
    fc::Logger logger_;
};

} // namespace fc_plugins::galaxy_buddies
