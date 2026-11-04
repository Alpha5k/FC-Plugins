#pragma once

#include "layout.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::galaxy_buddies {

// Wraps game functions and Galaxy 1.112 interfaces used by buddy callbacks.
// Native objects stay borrowed; snapshots and error text contain copied values.
class GameBridge final {
  public:
    // Bounds snapshots, native player slots and copied text. Text capacities count bytes, including NUL.
    static constexpr std::size_t kRequestCapacity = 32;
    static constexpr std::size_t kNameCapacity = 64;
    static constexpr std::size_t kAcceptedCapacity = 256;
    static constexpr std::size_t kLobbyCapacity = 64;
    static constexpr std::size_t kErrorNameCapacity = 64;
    static constexpr std::size_t kErrorMessageCapacity = 192;

    enum class GalaxyCallState : std::uint8_t {
        // The immediate call left no Galaxy error; asynchronous work may still fail.
        Accepted,
        InterfaceUnavailable,
        NativeFailure,
        GalaxyError,
    };

    // Copies Galaxy's immediate error before another API call can replace its borrowed text.
    // Accepted describes submission or a cached read, depending on the operation.
    struct GalaxyCallResult {
        GalaxyCallState state{GalaxyCallState::InterfaceUnavailable};
        GalaxyId user;
        std::uint32_t error_type{};
        std::array<char, kErrorNameCapacity> error_name{};
        std::array<char, kErrorMessageCapacity> error_message{};

        bool accepted() const noexcept {
            return state == GalaxyCallState::Accepted;
        }
    };

    // Separates the Galaxy read result from whether a nonempty name was copied.
    // An empty value and a missing key both leave present false.
    struct DisplayNameRead {
        GalaxyCallResult result;
        std::array<char, kNameCapacity> name{};
        bool present{};
    };

    struct BuddyRequest {
        GalaxyId id;
        // Seconds as returned by Galaxy; the epoch and enumeration order are unspecified here.
        std::uint32_t sent_at{};
        std::array<char, kNameCapacity> name{};
        // Prevents a later relationship snapshot from restoring a generated name.
        bool published_name{};

        friend bool operator==(const BuddyRequest &, const BuddyRequest &) = default;
    };

    struct RequestList {
        std::array<BuddyRequest, kRequestCapacity> entries{};
        std::size_t count{};

        friend bool operator==(const RequestList &, const RequestList &) = default;
    };

    struct AcceptedBuddy {
        GalaxyId id;
        // Parsed connect presence; zero means this snapshot has no usable advertised lobby ID.
        GalaxyId lobby;
        // Whether the game reported a nonempty private connection string at that ordinal.
        bool joinable{};
        bool online{};
        std::array<char, kNameCapacity> name{};
        // Prevents a later game-cache snapshot from restoring a generated name.
        bool published_name{};

        friend bool operator==(const AcceptedBuddy &, const AcceptedBuddy &) = default;
    };

    // Copies the ready accepted cache for retention while later requests are in flight.
    struct AcceptedList {
        std::array<AcceptedBuddy, kAcceptedCapacity> entries{};
        std::size_t count{};

        friend bool operator==(const AcceptedList &, const AcceptedList &) = default;
    };

    // Declares the game functions and memory views used after activation.
    // Callers must follow the native owners' execution and lifetime requirements.
    void plan(fc::Plan &plan, const Layout &layout);

    // Reports whether the game's accepted cache has completed its current request.
    bool accepted_ready() const noexcept;
    // Copies a completed native accepted cache in its original account order.
    bool capture_accepted(AcceptedList &output) const noexcept;
    // Refreshes the online flags in an existing accepted-buddy snapshot.
    void refresh_presence(AcceptedList &buddies) const noexcept;
    // Reads Galaxy's cached connect values into the accepted-buddy snapshot.
    void refresh_lobbies(AcceptedList &buddies) const noexcept;
    // Resolves an active sparse native player slot to its Galaxy identity.
    std::optional<GalaxyId> lobby_player(std::int32_t index) const noexcept;
    // Retains the native flags callback's exact displayed-name and active-slot validation.
    bool lobby_player_matches(std::int32_t index, std::string_view name) const noexcept;
    // Resolves a private connection string by current accepted account identity, never a retained ordinal.
    std::optional<GalaxyId> buddy_lobby(GalaxyId user) const noexcept;
    // Tests membership in the game's ready accepted cache; unavailable reads return false.
    bool is_accepted(GalaxyId id) const noexcept;

    // Returns the game's immediate accepted-list submission result; completion arrives separately.
    bool request_accepted_buddies() const noexcept;
    // Starts a read whose invitation entries must be copied during its success callback.
    GalaxyCallResult request_invitations() const noexcept;
    // Galaxy exposes invitation entries only during the successful completion
    // callback for RequestFriendInvitationList.
    GalaxyCallResult capture_incoming(RequestList &output) const noexcept;

    // Submits a buddy invitation; acceptance does not confirm its later completion.
    GalaxyCallResult send_invitation(GalaxyId id) const noexcept;
    // Submits acceptance or rejection of an incoming invitation; completion arrives separately.
    GalaxyCallResult respond_to_invitation(GalaxyId id, bool accept) const noexcept;
    // Submits DeleteFriend, used here for both accepted removal and outgoing cancellation.
    GalaxyCallResult delete_buddy(GalaxyId id) const noexcept;

    // Checks that the current Galaxy registrar exposes registration and removal slots.
    bool callbacks_available() const noexcept;
    // Borrows listener storage; keep it alive until callbacks have stopped.
    // A true result means the void registration call returned without checking Galaxy's error.
    bool register_listener(std::uint32_t type, void *listener) const noexcept;
    // Removes a listener through the current registrar, which may differ from the one used to register it.
    void unregister_listener(std::uint32_t type, void *listener) const noexcept;
    // Copies the cached persona or a stable identifier when no name is available.
    void copy_buddy_name(GalaxyId id, std::array<char, kNameCapacity> &output) const noexcept;

    // Reports whether Galaxy authentication permits account requests.
    bool logged_on() const noexcept;
    // Reads the authenticated Galaxy identity used to reject self-invitations.
    std::optional<GalaxyId> local_user() const noexcept;
    // Submits the local display name to Galaxy account user data; callers confirm it by readback.
    GalaxyCallResult publish_display_name(const char *name) const noexcept;
    // Requests one remote name for callback or cached-completion verification.
    GalaxyCallResult request_display_name(GalaxyId id) const noexcept;
    // Galaxy 1.112 has no separate availability query. This read handles both
    // callback completion and requests satisfied from its existing cache.
    DisplayNameRead read_display_name(GalaxyId id = {}) const noexcept;
    // Copies Steam's platform name or GOG's local Galaxy persona.
    bool copy_local_display_name(std::array<char, kNameCapacity> &output) const noexcept;
    // Copies a matching active player's native lobby name.
    bool copy_lobby_name(GalaxyId id, std::array<char, kNameCapacity> &output) const noexcept;

    // Requests one user's rich presence; refresh_lobbies later parses its cached connect value.
    GalaxyCallResult request_lobby(GalaxyId user) const noexcept;
    // Copies the target before publishing native special-join readiness. No find operation is allocated here.
    bool queue_lobby_join(GalaxyId lobby) const noexcept;
    // Targeted joins keep only the requested lobby; an ordinary server search
    // has a zero request and keeps every result.
    bool should_add_lobby(const GalaxyId *lobby) const noexcept;

  private:
    // Reads one vtable slot, checking only for null objects and tables.
    // Callers must supply a live object with the expected Galaxy 1.112 interface layout.
    template <class Function> static Function vtable_function(void *object, std::size_t slot) noexcept;
    // Resolves borrowed Galaxy.dll interfaces and the current registrar. FriendsTranslator
    // forwards to GalaxyPeer exports; its allocation is separate from the peer's private context.
    void *galaxy() const;
    void *user() const;
    void *friends() const;
    void *listener_registrar() const;
    // Copies the error left by the immediately preceding Galaxy call; no error means Accepted.
    GalaxyCallResult call_result(GalaxyId user = {}) const noexcept;
    // Builds a result when the required interface or user is unavailable.
    static GalaxyCallResult unavailable_call(GalaxyId user = {}) noexcept;
    // Builds a result when a prepared native call fails or a caught C++ exception interrupts it.
    static GalaxyCallResult native_failure(GalaxyId user = {}) noexcept;
    // Returns a borrowed cached persona; callers copy it before another facade call.
    const char *persona_name(void *friends, GalaxyId id) const noexcept;
    fc::NativeFunction<GetFriends> get_friends_;
    fc::NativeFunction<GetGalaxyError> get_error_;
    fc::Memory<const GetGalaxyInstance> get_galaxy_instance_;
    fc::NativeFunction<RequestAcceptedBuddies> request_accepted_;
    fc::NativeFunction<AcceptedBuddyCount> accepted_count_;
    fc::NativeFunction<AcceptedBuddyByIndex> accepted_by_index_;
    fc::NativeFunction<AcceptedBuddyName> accepted_name_;
    fc::NativeFunction<IsAcceptedBuddy> is_accepted_;
    fc::NativeFunction<IsBuddyJoinable> is_joinable_;
    fc::NativeFunction<BuddyConnection> buddy_connection_;
    fc::NativeFunction<SetSpecialJoinPending> set_special_join_pending_;
    fc::NativeFunction<LobbyPlayerCount> lobby_count_;
    fc::NativeFunction<LobbyPlayerIndex> lobby_index_;
    fc::NativeFunction<LobbyPlayerName> lobby_player_name_;
    fc::NativeFunction<LobbyDisplayName> lobby_display_name_;
    fc::Memory<const LobbyPlayer> lobby_players_;
    fc::Memory<const std::uint8_t> accepted_ready_;
    fc::Memory<const GalaxyId> requested_lobby_;
    fc::Memory<std::byte> special_join_target_;
    fc::NativeFunction<LocalDisplayName> local_display_name_;
    bool has_local_display_name_{};
};

// Recognizes empty and generated names so refreshes can retain a better user-data or player name.
inline bool is_placeholder_name(const std::array<char, GameBridge::kNameCapacity> &name) {
    const std::string_view value{name.data()};
    if (value.empty() || value == "[unknown]" || value.starts_with("Galaxy user ")) {
        return true;
    }

    constexpr std::string_view steam_prefix = "SteamUser.";
    if (!value.starts_with(steam_prefix) || value.size() == steam_prefix.size()) {
        return false;
    }

    return std::all_of(value.begin() + steam_prefix.size(), value.end(), [](char character) {
        return character >= '0' && character <= '9';
    });
}

} // namespace fc_plugins::galaxy_buddies
