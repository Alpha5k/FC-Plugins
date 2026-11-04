#include "game.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <span>

namespace fc_plugins::galaxy_buddies {
namespace {

// Byte offsets in the supplied Galaxy 1.112 IFriends/IUser/IGalaxy interfaces.
// These Galaxy.dll translators forward calls to GalaxyPeer exports and borrow SDK-owned storage.
constexpr std::size_t kRequestInvitationListSlot = 0x48 / sizeof(void *);
constexpr std::size_t kInvitationCountSlot = 0x4C / sizeof(void *);
constexpr std::size_t kInvitationByIndexSlot = 0x50 / sizeof(void *);
constexpr std::size_t kRespondToInvitationSlot = 0x54 / sizeof(void *);
constexpr std::size_t kDeleteBuddySlot = 0x58 / sizeof(void *);
constexpr std::size_t kSendInvitationSlot = 0x44 / sizeof(void *);
constexpr std::size_t kLocalPersonaNameSlot = 0x08 / sizeof(void *);
constexpr std::size_t kPersonaNameSlot = 0x14 / sizeof(void *);
constexpr std::size_t kFriendPersonaStateSlot = 0x1C / sizeof(void *);
constexpr std::size_t kRequestRichPresenceSlot = 0x68 / sizeof(void *);
constexpr std::size_t kGetRichPresenceSlot = 0x6C / sizeof(void *);
constexpr std::size_t kUserInterfaceSlot = 0x14 / sizeof(void *);
constexpr std::size_t kGetGalaxyIdSlot = 0x08 / sizeof(void *);
constexpr std::size_t kRequestUserDataSlot = 0x24 / sizeof(void *);
constexpr std::size_t kGetUserDataCopySlot = 0x2C / sizeof(void *);
constexpr std::size_t kSetUserDataSlot = 0x30 / sizeof(void *);
constexpr std::size_t kUserLoggedOnSlot = 0x40 / sizeof(void *);
constexpr std::size_t kErrorNameSlot = 0x04 / sizeof(void *);
constexpr std::size_t kErrorMessageSlot = 0x08 / sizeof(void *);
constexpr std::size_t kErrorTypeSlot = 0x0C / sizeof(void *);
constexpr std::size_t kListenerRegistrarSlot = 0x40 / sizeof(void *);
constexpr std::size_t kRegisterListenerSlot = 0x04 / sizeof(void *);
constexpr std::size_t kUnregisterListenerSlot = 0x08 / sizeof(void *);

constexpr std::size_t kLobbyValueCapacity = 32;
// The user-data key is shared by patched Steam and GOG clients.
constexpr char kDisplayNameKey[] = "FusionCutter.DisplayName";
constexpr char kConnectKey[] = "connect";

using RequestInvitationList = void(__thiscall *)(void *friends);
using InvitationCount = std::uint32_t(__thiscall *)(void *friends);
using InvitationByIndex = void(__thiscall *)(void *friends, std::uint32_t index, GalaxyId &id, std::uint32_t &sent_at);
using SendInvitation = void(__thiscall *)(void *friends, GalaxyId id);
using RespondToInvitation = void(__thiscall *)(void *friends, GalaxyId id, bool accept);
using DeleteBuddy = void(__thiscall *)(void *friends, GalaxyId id);
using LocalPersonaName = const char *(__thiscall *)(void *friends);
using PersonaName = const char *(__thiscall *)(void *friends, GalaxyId id);
using FriendPersonaState = std::uint32_t(__thiscall *)(void *friends, GalaxyId id);
using RequestRichPresence = void(__thiscall *)(void *friends, GalaxyId id);
using GetRichPresence = const char *(__thiscall *)(void *friends, const char *key, GalaxyId id);
using GetUserInterface = void *(__thiscall *)(void *galaxy);
using UserLoggedOn = bool(__thiscall *)(void *user);
using GetGalaxyId = GalaxyId *(__thiscall *)(void *user, GalaxyId *output);
using RequestUserData = void(__thiscall *)(void *user, GalaxyId id);
using GetUserDataCopy = void(__thiscall *)(void *user, const char *key, char *buffer, std::uint32_t buffer_length, GalaxyId id);
using SetUserData = void(__thiscall *)(void *user, const char *key, const char *value);
using ErrorText = const char *(__thiscall *)(GalaxyError * error);
using ErrorType = std::uint32_t(__thiscall *)(GalaxyError *error);
using GetListenerRegistrar = void *(__thiscall *)(void *galaxy);
using RegisterListener = void(__thiscall *)(void *registrar, std::uint32_t type, void *listener);
using UnregisterListener = void(__thiscall *)(void *registrar, std::uint32_t type, void *listener);

// Copies borrowed bytes with a final NUL; truncation may split a UTF-8 character.
template <std::size_t Capacity> void copy_text(std::array<char, Capacity> &output, const char *input) noexcept {
    output.fill('\0');
    if (input == nullptr) {
        return;
    }

    for (std::size_t index = 0; index + 1 < output.size() && input[index] != '\0'; ++index) {
        output[index] = input[index];
    }
}

// Produces a stable fallback when Galaxy has not supplied a persona name.
void write_id_name(std::array<char, GameBridge::kNameCapacity> &output, GalaxyId id) noexcept {
    constexpr char prefix[] = "Galaxy user ";
    output.fill('\0');
    std::copy_n(prefix, sizeof(prefix) - 1, output.begin());
    const auto first = output.data() + sizeof(prefix) - 1;
    std::to_chars(first, output.data() + output.size() - 1, id.value);
}

} // namespace

template <class Function> Function GameBridge::vtable_function(void *object, std::size_t slot) noexcept {
    if (object == nullptr) {
        return nullptr;
    }

    const auto *vtable = fc::read_field<const std::byte *>(object);
    return vtable == nullptr ? nullptr : fc::read_field<Function>(vtable, slot * sizeof(void *));
}

void GameBridge::plan(fc::Plan &plan, const Layout &layout) {
    get_friends_ = plan.function(layout.get_friends);
    get_error_ = plan.function(layout.get_error);
    get_galaxy_instance_ = plan.access(layout.get_galaxy_instance);
    request_accepted_ = plan.function(layout.request_accepted);
    accepted_count_ = plan.function(layout.accepted_count);
    accepted_by_index_ = plan.function(layout.accepted_by_index);
    accepted_name_ = plan.function(layout.accepted_name);
    is_accepted_ = plan.function(layout.is_accepted);
    is_joinable_ = plan.function(layout.is_joinable);
    buddy_connection_ = plan.function(layout.buddy_connection);
    set_special_join_pending_ = plan.function(layout.set_special_join_pending);
    lobby_count_ = plan.function(layout.lobby_count);
    lobby_index_ = plan.function(layout.lobby_index);
    lobby_player_name_ = plan.function(layout.lobby_player_name);
    lobby_display_name_ = plan.function(layout.lobby_display_name);
    lobby_players_ = plan.access(layout.lobby_players);
    accepted_ready_ = plan.access(layout.accepted_ready);
    requested_lobby_ = plan.access(layout.requested_lobby);
    special_join_target_ = plan.access(layout.special_join_target);

    if (layout.local_display_name) {
        local_display_name_ = plan.function(*layout.local_display_name);
        has_local_display_name_ = true;
    }
}

void *GameBridge::galaxy() const {
    const auto getter = get_galaxy_instance_.read();
    return getter && *getter ? (*getter)() : nullptr;
}

void *GameBridge::user() const {
    auto *instance = galaxy();
    const auto get_user = vtable_function<GetUserInterface>(instance, kUserInterfaceSlot);
    return get_user == nullptr ? nullptr : get_user(instance);
}

void *GameBridge::friends() const {
    return get_friends_.call().value_or(nullptr);
}

void *GameBridge::listener_registrar() const {
    auto *instance = galaxy();
    const auto get_registrar = vtable_function<GetListenerRegistrar>(instance, kListenerRegistrarSlot);
    return get_registrar == nullptr ? nullptr : get_registrar(instance);
}

GameBridge::GalaxyCallResult GameBridge::unavailable_call(GalaxyId user) noexcept {
    return {.state = GalaxyCallState::InterfaceUnavailable, .user = user};
}

GameBridge::GalaxyCallResult GameBridge::native_failure(GalaxyId user) noexcept {
    return {.state = GalaxyCallState::NativeFailure, .user = user};
}

GameBridge::GalaxyCallResult GameBridge::call_result(GalaxyId user) const noexcept {
    try {
        auto error_result = get_error_.call();
        if (!error_result) {
            return native_failure(user);
        }

        auto *error = *error_result;
        if (error == nullptr) {
            return {.state = GalaxyCallState::Accepted, .user = user};
        }

        GalaxyCallResult result{.state = GalaxyCallState::GalaxyError, .user = user};
        const auto get_name = vtable_function<ErrorText>(error, kErrorNameSlot);
        const auto get_message = vtable_function<ErrorText>(error, kErrorMessageSlot);
        const auto get_type = vtable_function<ErrorType>(error, kErrorTypeSlot);
        if (get_name != nullptr) {
            copy_text(result.error_name, get_name(error));
        }
        if (get_message != nullptr) {
            copy_text(result.error_message, get_message(error));
        }
        if (get_type != nullptr) {
            result.error_type = get_type(error);
        }

        return result;
    } catch (...) {
        return native_failure(user);
    }
}

bool GameBridge::accepted_ready() const noexcept {
    return accepted_ready_.read().value_or(0) != 0;
}

bool GameBridge::capture_accepted(AcceptedList &output) const noexcept {
    if (!accepted_ready()) {
        return false;
    }

    try {
        AcceptedList captured;
        auto *interface = friends();
        const auto persona_state = vtable_function<FriendPersonaState>(interface, kFriendPersonaStateSlot);
        captured.count = std::min<std::size_t>(accepted_count_.call().value_or(0), captured.entries.size());

        // Preserve native accepted order; connection-string actions resolve these accounts again at execution.
        for (std::size_t index = 0; index < captured.count; ++index) {
            auto &entry = captured.entries[index];
            const auto ordinal = static_cast<std::uint32_t>(index);
            if (!accepted_by_index_.call(&entry.id, ordinal)) {
                return false;
            }
            entry.joinable = is_joinable_.call(ordinal).value_or(false);
            entry.online = persona_state != nullptr && persona_state(interface, entry.id) != 0;
            copy_text(entry.name, accepted_name_.call(ordinal).value_or(nullptr));
            if (entry.name.front() == '\0') {
                write_id_name(entry.name, entry.id);
            }
        }

        // Replace the caller's snapshot after the bounded enumeration completes.
        output = captured;
        return true;
    } catch (...) {
        return false;
    }
}

void GameBridge::refresh_presence(AcceptedList &buddies) const noexcept {
    try {
        auto *interface = friends();
        const auto persona_state = vtable_function<FriendPersonaState>(interface, kFriendPersonaStateSlot);
        if (persona_state == nullptr) {
            return;
        }

        for (std::size_t index = 0; index < buddies.count; ++index) {
            auto &buddy = buddies.entries[index];
            const auto online = persona_state(interface, buddy.id) != 0;
            buddy.online = online;
        }
    } catch (...) {
        return;
    }
}

void GameBridge::refresh_lobbies(AcceptedList &buddies) const noexcept {
    try {
        auto *interface = friends();
        const auto get_presence = vtable_function<GetRichPresence>(interface, kGetRichPresenceSlot);
        if (get_presence == nullptr) {
            return;
        }

        for (std::size_t index = 0; index < buddies.count; ++index) {
            auto &buddy = buddies.entries[index];
            GalaxyId lobby;
            if (const auto *value = buddy.online ? get_presence(interface, kConnectKey, buddy.id) : nullptr; value != nullptr) {
                const auto length = strnlen_s(value, kLobbyValueCapacity);
                const auto *end = value + length;
                const auto parsed = std::from_chars(value, end, lobby.value);
                // connect must fit the byte buffer and parse completely as a nonzero decimal ID.
                if (length == kLobbyValueCapacity || parsed.ec != std::errc{} || parsed.ptr != end || lobby.value == 0) {
                    lobby = {};
                }
            }
            buddy.lobby = lobby;
        }
    } catch (...) {
        return;
    }
}

std::optional<GalaxyId> GameBridge::lobby_player(std::int32_t index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= kLobbyCapacity) {
        return std::nullopt;
    }

    try {
        // Native slots can be sparse; player count is not a slot upper bound.
        const auto count = std::clamp(lobby_count_.call().value_or(0), 0, static_cast<std::int32_t>(kLobbyCapacity));
        bool present{};
        for (std::int32_t ordinal = 0; ordinal < count; ++ordinal) {
            if (lobby_index_.call(ordinal).value_or(-1) == index) {
                present = true;
                break;
            }
        }
        if (!present) {
            return std::nullopt;
        }

        // The view starts at the ID within a NetGame record. Presence and identity are separate
        // native reads, so this lookup assumes the slot remains stable between them.
        const auto player = lobby_players_.read(static_cast<std::size_t>(index));
        const auto id = player ? player->id : GalaxyId{};
        return id.value == 0 ? std::nullopt : std::optional{id};
    } catch (...) {
        return std::nullopt;
    }
}

bool GameBridge::is_accepted(GalaxyId id) const noexcept {
    try {
        return is_accepted_.call(&id).value_or(false);
    } catch (...) {
        return false;
    }
}

bool GameBridge::request_accepted_buddies() const noexcept {
    try {
        // gf::friends::start clears the game-owned ready byte before requesting the SDK cache.
        return request_accepted_.call().value_or(false);
    } catch (...) {
        return false;
    }
}

GameBridge::GalaxyCallResult GameBridge::request_invitations() const noexcept {
    try {
        auto *interface = friends();
        const auto request = vtable_function<RequestInvitationList>(interface, kRequestInvitationListSlot);
        if (request == nullptr) {
            return unavailable_call();
        }

        // Capture the immediate submission error before any other Galaxy operation replaces it.
        request(interface);
        return call_result();
    } catch (...) {
        return native_failure();
    }
}

const char *GameBridge::persona_name(void *interface, GalaxyId id) const noexcept {
    try {
        const auto get_name = vtable_function<PersonaName>(interface, kPersonaNameSlot);
        return get_name == nullptr ? nullptr : get_name(interface, id);
    } catch (...) {
        // A caught C++ exception leaves the caller to use a stable identifier.
        return nullptr;
    }
}

GameBridge::GalaxyCallResult GameBridge::capture_incoming(RequestList &output) const noexcept {
    try {
        auto *interface = friends();
        const auto get_count = vtable_function<InvitationCount>(interface, kInvitationCountSlot);
        const auto get_by_index = vtable_function<InvitationByIndex>(interface, kInvitationByIndexSlot);
        if (get_count == nullptr || get_by_index == nullptr) {
            return unavailable_call();
        }

        RequestList captured;
        // Preserve native enumeration order; Galaxy's timestamp does not establish list ordering.
        const auto invitation_count = get_count(interface);
        if (auto result = call_result(); !result.accepted()) {
            return result;
        }

        captured.count = std::min<std::size_t>(invitation_count, captured.entries.size());
        for (std::size_t index = 0; index < captured.count; ++index) {
            auto &entry = captured.entries[index];
            get_by_index(interface, static_cast<std::uint32_t>(index), entry.id, entry.sent_at);
            if (auto result = call_result(entry.id); !result.accepted()) {
                return result;
            }

            copy_text(entry.name, persona_name(interface, entry.id));
            if (entry.name.front() == '\0') {
                write_id_name(entry.name, entry.id);
            }
        }

        // The invitation cache belongs to this success callback; retain only the copied records.
        output = captured;
        return {.state = GalaxyCallState::Accepted};
    } catch (...) {
        return native_failure();
    }
}

GameBridge::GalaxyCallResult GameBridge::send_invitation(GalaxyId id) const noexcept {
    try {
        auto *interface = friends();
        const auto send = vtable_function<SendInvitation>(interface, kSendInvitationSlot);
        if (send == nullptr) {
            return unavailable_call(id);
        }

        send(interface, id);
        return call_result(id);
    } catch (...) {
        return native_failure(id);
    }
}

GameBridge::GalaxyCallResult GameBridge::respond_to_invitation(GalaxyId id, bool accept) const noexcept {
    try {
        auto *interface = friends();
        const auto respond = vtable_function<RespondToInvitation>(interface, kRespondToInvitationSlot);
        if (respond == nullptr) {
            return unavailable_call(id);
        }

        respond(interface, id, accept);
        return call_result(id);
    } catch (...) {
        return native_failure(id);
    }
}

GameBridge::GalaxyCallResult GameBridge::delete_buddy(GalaxyId id) const noexcept {
    try {
        auto *interface = friends();
        const auto remove = vtable_function<DeleteBuddy>(interface, kDeleteBuddySlot);
        if (remove == nullptr || id.value == 0) {
            return unavailable_call(id);
        }

        remove(interface, id);
        return call_result(id);
    } catch (...) {
        return native_failure(id);
    }
}

bool GameBridge::callbacks_available() const noexcept {
    try {
        auto *registrar = listener_registrar();
        return vtable_function<RegisterListener>(registrar, kRegisterListenerSlot) != nullptr &&
               vtable_function<UnregisterListener>(registrar, kUnregisterListenerSlot) != nullptr;
    } catch (...) {
        return false;
    }
}

bool GameBridge::register_listener(std::uint32_t type, void *listener) const noexcept {
    try {
        auto *registrar = listener_registrar();
        const auto register_callback = vtable_function<RegisterListener>(registrar, kRegisterListenerSlot);
        if (register_callback == nullptr || listener == nullptr) {
            return false;
        }

        // The registrar borrows the listener pointer. This void call supplies no registration result.
        register_callback(registrar, type, listener);
        return true;
    } catch (...) {
        return false;
    }
}

void GameBridge::unregister_listener(std::uint32_t type, void *listener) const noexcept {
    try {
        auto *registrar = listener_registrar();
        const auto unregister_callback = vtable_function<UnregisterListener>(registrar, kUnregisterListenerSlot);
        if (unregister_callback != nullptr && listener != nullptr) {
            unregister_callback(registrar, type, listener);
        }
    } catch (...) {
        // Removal is best effort through the current registrar; a caught C++ exception is ignored.
    }
}

void GameBridge::copy_buddy_name(GalaxyId id, std::array<char, kNameCapacity> &output) const noexcept {
    try {
        copy_text(output, persona_name(friends(), id));
        if (output.front() == '\0') {
            write_id_name(output, id);
        }
    } catch (...) {
        write_id_name(output, id);
    }
}

bool GameBridge::logged_on() const noexcept {
    try {
        auto *interface = user();
        const auto logged_on = vtable_function<UserLoggedOn>(interface, kUserLoggedOnSlot);
        return logged_on != nullptr && logged_on(interface);
    } catch (...) {
        return false;
    }
}

std::optional<GalaxyId> GameBridge::local_user() const noexcept {
    try {
        auto *interface = user();
        const auto get_id = vtable_function<GetGalaxyId>(interface, kGetGalaxyIdSlot);
        GalaxyId id;
        if (get_id == nullptr || get_id(interface, &id) == nullptr || id.value == 0) {
            return std::nullopt;
        }
        return id;
    } catch (...) {
        return std::nullopt;
    }
}

GameBridge::GalaxyCallResult GameBridge::publish_display_name(const char *name) const noexcept {
    try {
        auto *interface = user();
        const auto logged_on = vtable_function<UserLoggedOn>(interface, kUserLoggedOnSlot);
        const auto set_data = vtable_function<SetUserData>(interface, kSetUserDataSlot);
        if (logged_on == nullptr || set_data == nullptr || !logged_on(interface) || name == nullptr || *name == '\0') {
            return unavailable_call();
        }

        set_data(interface, kDisplayNameKey, name);
        return call_result();
    } catch (...) {
        return native_failure();
    }
}

GameBridge::GalaxyCallResult GameBridge::request_display_name(GalaxyId id) const noexcept {
    try {
        auto *interface = user();
        const auto request = vtable_function<RequestUserData>(interface, kRequestUserDataSlot);
        if (request == nullptr || id.value == 0) {
            return unavailable_call(id);
        }

        // IUser+0x24 requests data for this eight-byte user ID; completion arrives separately.
        request(interface, id);
        return call_result(id);
    } catch (...) {
        return native_failure(id);
    }
}

GameBridge::DisplayNameRead GameBridge::read_display_name(GalaxyId id) const noexcept {
    DisplayNameRead read{.result = unavailable_call(id)};
    try {
        auto *interface = user();
        const auto get_data = vtable_function<GetUserDataCopy>(interface, kGetUserDataCopySlot);
        if (get_data == nullptr) {
            return read;
        }

        // IUser+0x2C copies the specified user's cached data into caller storage.
        get_data(interface, kDisplayNameKey, read.name.data(), static_cast<std::uint32_t>(read.name.size()), id);
        read.name.back() = '\0';
        read.result = call_result(id);
        read.present = read.result.accepted() && read.name.front() != '\0';
        return read;
    } catch (...) {
        return {.result = native_failure(id)};
    }
}

bool GameBridge::copy_local_display_name(std::array<char, kNameCapacity> &output) const noexcept {
    output.fill('\0');
    try {
        const char *name{};
        if (has_local_display_name_) {
            name = local_display_name_.call().value_or(nullptr);
        } else {
            auto *interface = friends();
            const auto get_name = vtable_function<LocalPersonaName>(interface, kLocalPersonaNameSlot);
            if (get_name != nullptr) {
                name = get_name(interface);
            }
        }

        copy_text(output, name);
        return output.front() != '\0';
    } catch (...) {
        output.fill('\0');
        return false;
    }
}

bool GameBridge::copy_lobby_name(GalaxyId id, std::array<char, kNameCapacity> &output) const noexcept {
    output.fill('\0');
    if (id.value == 0) {
        return false;
    }

    try {
        // Enumerate active ordinals, then use the matching sparse slot for both its ID and name.
        const auto count = std::clamp(lobby_count_.call().value_or(0), 0, static_cast<std::int32_t>(kLobbyCapacity));
        for (std::int32_t ordinal = 0; ordinal < count; ++ordinal) {
            const auto index = lobby_index_.call(ordinal).value_or(-1);
            if (index < 0 || index >= static_cast<std::int32_t>(kLobbyCapacity)) {
                continue;
            }
            if (lobby_players_.read(static_cast<std::size_t>(index)).value_or(LobbyPlayer{}).id == id) {
                copy_text(output, lobby_player_name_.call(index).value_or(nullptr));
                return output.front() != '\0';
            }
        }
    } catch (...) {
        output.fill('\0');
    }

    return false;
}

GameBridge::GalaxyCallResult GameBridge::request_lobby(GalaxyId user) const noexcept {
    try {
        auto *interface = friends();
        const auto request = vtable_function<RequestRichPresence>(interface, kRequestRichPresenceSlot);
        if (request == nullptr || user.value == 0) {
            return unavailable_call(user);
        }

        request(interface, user);
        return call_result(user);
    } catch (...) {
        return native_failure(user);
    }
}

bool GameBridge::lobby_player_matches(std::int32_t index, std::string_view name) const noexcept {
    if (!lobby_player(index)) {
        return false;
    }
    const auto current = lobby_display_name_.call(index).value_or(nullptr);
    return current != nullptr && name == current;
}

std::optional<GalaxyId> GameBridge::buddy_lobby(GalaxyId user) const noexcept {
    if (!accepted_ready() || user.value == 0) {
        return std::nullopt;
    }
    const auto count = std::min<std::size_t>(accepted_count_.call().value_or(0), kAcceptedCapacity);
    for (std::uint32_t index = 0; index < count; ++index) {
        GalaxyId current;
        if (!accepted_by_index_.call(&current, index) || current != user) {
            continue;
        }
        const auto connection = buddy_connection_.call(index).value_or(nullptr);
        GalaxyId lobby;
        // Match retail's decimal connection-string parsing while retaining all 64 ID bits.
        if (connection != nullptr && sscanf_s(connection, "%llu", &lobby.value) == 1 && lobby.value != 0) {
            return lobby;
        }
        break;
    }
    return std::nullopt;
}

bool GameBridge::queue_lobby_join(GalaxyId lobby) const noexcept {
    if (lobby.value == 0) {
        return false;
    }

    // Retail shell entry checks pending before OpenNetShell, then BeginJoinSpecial copies this value.
    // Its existing callback owns allocation, current-operation assignment and pending-flag clearing.
    return special_join_target_.copy_from(std::as_bytes(std::span{&lobby, 1})).has_value() &&
           set_special_join_pending_.call(true).has_value();
}

bool GameBridge::should_add_lobby(const GalaxyId *lobby) const noexcept {
    // The native requested ID controls filtering; the game also owns clearing it between searches.
    const auto requested = requested_lobby_.read();
    return lobby == nullptr || !requested || requested->value == 0 || *lobby == *requested;
}

} // namespace fc_plugins::galaxy_buddies
