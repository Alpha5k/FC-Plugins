#include "handler.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace fc_plugins::galaxy_buddies {
namespace {

// IDs cross Lua as complete decimal strings; retail numbers cannot represent all eight bytes.
std::optional<GalaxyId> account_id(fc::lua::Value value) {
    const auto text = value.as_string();
    if (!text) {
        return std::nullopt;
    }
    GalaxyId id;
    const auto result = std::from_chars(text->data(), text->data() + text->size(), id.value);
    return result.ec == std::errc{} && result.ptr == text->data() + text->size() && id.value != 0 ? std::optional{id} : std::nullopt;
}

// Copies an ID into a Lua string before the temporary decimal buffer expires.
fc::lua::Value account_value(fc::lua::Frame &frame, GalaxyId id) {
    std::array<char, 20> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), id.value);
    return frame.value(std::string_view{text.data(), result.ptr});
}

// Lua popup visibility uses nil/false and numeric true values.
bool enabled(fc::lua::Value value) {
    return value.as_bool().value_or(value.type() != fc::lua::Type::Nil);
}

// Account actions are available only in the native Galaxy service, independently of SDK sign-in.
bool galaxy_service(fc::lua::Frame &frame) {
    const auto service = frame.call(frame.global("ScriptCB_GetOnlineService"), {});
    if (!service || service->size() != 1) {
        return false;
    }
    const auto name = (*service)[0].as_string();
    return name && *name == "Galaxy";
}

// Defers stock publication until the outer wrapper runs; restores suppression on errors and reentry.
struct FriendsPublicationScope {
    bool &suppressed;
    bool previous;

    explicit FriendsPublicationScope(bool &state) : suppressed(state), previous(std::exchange(state, true)) {}

    ~FriendsPublicationScope() {
        suppressed = previous;
    }
};

// Keeps stock errors and return values intact for forwarded callbacks.
void forward_original(fc::lua::Frame &frame) {
    auto result = frame.original();
    if (result) {
        frame.return_values(result->values());
    } else {
        frame.error(result.error().message);
    }
}

} // namespace

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    auto bindings = fc::lua::get(context);
    if (!bindings) {
        return std::unexpected(bindings.error());
    }

    lua_ = *bindings;

    using fc::lua::BindingMode;

    // Begin/Cancel/Update run the stock callbacks first and return their original values.
    // Relationship wrappers serialize shared state while using only the invoking VM's Frame.
    if (auto result = lua_.bind("ScriptCB_BeginFriends", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    auto original = [this, &frame] {
                                        FriendsPublicationScope scope{suppress_friends_publication_};
                                        return frame.original();
                                    }();
                                    if (!original) {
                                        frame.error(original.error().message);
                                        return;
                                    }

                                    frame.global("FusionCutterFriendsActive", frame.value(true));
                                    callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
                                    schedule_display_name_refresh();
                                    refresh(frame);
                                    frame.return_values(original->values());
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_CancelFriends", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    auto original = frame.original();
                                    if (!original) {
                                        frame.error(original.error().message);
                                        return;
                                    }

                                    std::lock_guard lock{state_mutex_};
                                    frame.global("FusionCutterFriendsActive", frame.value(false));
                                    frame.return_values(original->values());
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_UpdateFriends", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    // Inactive VMs retain the native publisher because no combined refresh replaces it.
                                    if (!enabled(frame.global("FusionCutterFriendsActive"))) {
                                        forward_original(frame);
                                        return;
                                    }

                                    auto original = [this, &frame] {
                                        FriendsPublicationScope scope{suppress_friends_publication_};
                                        return frame.original();
                                    }();
                                    if (!original) {
                                        frame.error(original.error().message);
                                        return;
                                    }

                                    // The native callback has no table consumer after publication; build once after it returns.
                                    refresh(frame);
                                    frame.return_values(original->values());
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_GetLobbyPlayerFlags", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    lobby_flags(frame);
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_LobbyAction", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    lobby_action(frame);
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_GetFriendStateStr", BindingMode::Replace,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    buddy_state(frame);
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_GetFriendActions", BindingMode::Replace,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    buddy_actions(frame);
                                });
        !result) {
        return result;
    }
    if (auto result = lua_.bind("ScriptCB_DoFriendAction", BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    do_buddy_action(frame);
                                });
        !result) {
        return result;
    }
    // The popup stores a copy of the global helper, so wrap its actual callable table member.
    if (auto result = lua_.bind({"Popup_LobbyOpts", "fnActivate"}, BindingMode::Wrap,
                                [this](fc::lua::Frame &frame) {
                                    std::lock_guard lock{state_mutex_};
                                    if (enabled(frame.argument(1))) {
                                        capture_lobby_action(frame);
                                    }
                                    // Input_Accept closes the popup before LobbyAction; retain its token through that close.
                                    forward_original(frame);
                                });
        !result) {
        return result;
    }

    return {};
}

std::optional<std::int32_t> Handler::number(fc::lua::Value value) {
    auto numeric = value.as_number();
    if (!numeric) {
        const auto text = value.as_string();
        if (!text) {
            return std::nullopt;
        }

        // String arguments must be complete decimals; numeric arguments below truncate fractions.
        std::int32_t parsed{};
        const auto result = std::from_chars(text->data(), text->data() + text->size(), parsed);
        return result.ec == std::errc{} && result.ptr == text->data() + text->size() ? std::optional{parsed} : std::nullopt;
    }

    if (!std::isfinite(*numeric) || *numeric < std::numeric_limits<std::int32_t>::min() ||
        *numeric > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }

    return static_cast<std::int32_t>(*numeric);
}

std::optional<std::size_t> Handler::selected_buddy(fc::lua::Frame &frame) const {
    const auto layout = frame.global("gsfriends_listbox_layout");
    if (layout.type() != fc::lua::Type::Table) {
        return std::nullopt;
    }

    const auto selection = number(layout.get("SelectedIdx"));

    const auto contents = frame.global("friends_listbox_contents");
    return selection && *selection > 0 && contents.type() == fc::lua::Type::Table ? action_buddy(contents.get(*selection)) : std::nullopt;
}

std::optional<std::size_t> Handler::action_buddy(fc::lua::Value row) const {
    if (row.type() != fc::lua::Type::Table) {
        return std::nullopt;
    }
    const auto user = account_id(row.get("account_id"));
    const auto owner = account_id(row.get("account_owner"));
    const auto current = game_.local_user();
    if (!user || !owner || !current || *owner != *current) {
        return std::nullopt;
    }
    std::size_t ordinal{};
    const auto find = [&](const auto &list) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < list.count; ++index, ++ordinal) {
            if (list.entries[index].id == *user) {
                return ordinal;
            }
        }
        return std::nullopt;
    };
    if (const auto selected = find(accepted_)) {
        return selected;
    }
    if (const auto selected = find(incoming_)) {
        return selected;
    }
    return find(outgoing_);
}

void Handler::publish_friends() noexcept {
    std::lock_guard lock{state_mutex_};
    if (!suppress_friends_publication_) {
        static_cast<void>(original_publish_friends_.call());
    }
}

void Handler::refresh(fc::lua::Frame &frame) {
    const auto previous = frame.global("friends_listbox_contents");
    auto layout = frame.global("gsfriends_listbox_layout");
    const auto selection = layout.type() == fc::lua::Type::Table ? number(layout.get("SelectedIdx")) : std::nullopt;
    const auto selected_row = selection && previous.type() == fc::lua::Type::Table ? previous.get(*selection) : frame.nil();
    const auto selected_id = selected_row.type() == fc::lua::Type::Table ? account_id(selected_row.get("account_id")) : std::nullopt;
    const auto owner = game_.local_user().value_or(GalaxyId{});
    auto contents = frame.table();
    std::size_t ordinal{};
    std::size_t retained_selection{};
    bool changed = previous.type() != fc::lua::Type::Table;
    // Compare retained row fields before publishing; selection follows the account when rows move.
    const auto add = [&](const auto &list, const char *relationship) {
        for (std::size_t index = 0; index < list.count; ++index) {
            const auto &entry = list.entries[index];
            const auto id = account_value(frame, entry.id);
            const auto account = account_value(frame, owner);
            auto old = previous.type() == fc::lua::Type::Table ? previous.get(ordinal + 1) : frame.nil();
            const auto matches = [&](const char *key, fc::lua::Value value) {
                if (old.type() != fc::lua::Type::Table) {
                    return false;
                }
                if (const auto text = value.as_string()) {
                    const auto existing = old.get(key).as_string();
                    return existing && *existing == *text;
                }
                const auto existing = old.get(key).as_bool();
                const auto boolean = value.as_bool();
                return existing && boolean && *existing == *boolean;
            };
            auto row = frame.table();
            const auto set = [&](const char *key, fc::lua::Value value) {
                changed = !matches(key, value) || changed;
                row.set(key, value);
            };
            set("name", frame.value(entry.name.data()));
            set("account_id", id);
            set("account_owner", account);
            set("relationship", frame.value(relationship));
            if constexpr (requires { entry.online; }) {
                set("online", frame.value(entry.online));
                set("joinable", frame.value(entry.joinable));
                set("lobby_id", account_value(frame, entry.lobby));
            }
            row.set("VoiceIcon", frame.value(0));
            row.set("StateIcon", frame.value(0));
            contents.set(++ordinal, row);
            if (selected_id && entry.id == *selected_id) {
                retained_selection = ordinal;
            }
        }
    };
    add(accepted_, "accepted");
    add(incoming_, "incoming");
    add(outgoing_, "outgoing");
    changed = changed || (previous.type() == fc::lua::Type::Table && previous.get(ordinal + 1).type() != fc::lua::Type::Nil);
    if (!changed) {
        return;
    }
    frame.global("friends_listbox_contents", contents);
    if (layout.type() == fc::lua::Type::Table && retained_selection != 0) {
        layout.set("SelectedIdx", frame.value(retained_selection));
        layout.set("CursorIdx", frame.value(retained_selection));
    }

    // Repaint may invoke a state wrapper again, which is why state_mutex_ is recursive.
    const auto screen = frame.global("ifs_mpgs_friends");
    if (screen.type() == fc::lua::Type::Table) {
        const auto repaint = screen.get("RepaintListbox");
        if (repaint.type() == fc::lua::Type::Function) {
            if (auto result = frame.call(repaint, {screen}); !result) {
                frame.error(result.error().message);
            }
        }
    }
}

void Handler::lobby_flags(fc::lua::Frame &frame) {
    auto native = frame.original();
    if (!native) {
        frame.error(native.error().message);
        return;
    }

    if (native->size() != 5) {
        frame.return_values(native->values());
        return;
    }

    // Argument 1 is a native player slot, independent of the combined buddy-list ordinal.
    const auto index = number(frame.argument(1));
    const auto displayed_name = frame.argument(0).as_string();
    const auto player =
        index && displayed_name && game_.lobby_player_matches(*index, *displayed_name) ? game_.lobby_player(*index) : std::nullopt;
    const auto relationship = player ? relationship_state(*player) : RelationshipState::None;
    const char *name = "";
    if (relationship == RelationshipState::Accepted) {
        name = "accepted";
    } else if (relationship == RelationshipState::Incoming) {
        name = "incoming";
    } else if (relationship == RelationshipState::Outgoing) {
        name = "outgoing";
    }
    const auto local = game_.local_user();
    const auto can_add = player && local && *player != *local && game_.logged_on() && galaxy_service(frame) && !enabled((*native)[3]) &&
                         relationship == RelationshipState::None;

    // Retail's fifth flag tests the unsupported LAN/GameSpy account path. Retain its exact
    // name/slot checks above and native mute, boot and guest values while supplying Galaxy eligibility.
    frame.return_values({(*native)[0], frame.value(relationship == RelationshipState::Accepted), (*native)[2], (*native)[3],
                         frame.value(can_add), frame.value(name)});
}

void Handler::capture_lobby_action(fc::lua::Frame &frame) {
    auto popup = frame.argument(0);
    if (popup.type() != fc::lua::Type::Table) {
        return;
    }
    popup.set("FusionCutterBuddyTarget", frame.nil());
    const auto layout = frame.global("lobby_listbox_layout");
    const auto contents = frame.global("ifs_mp_lobby_listbox_contents");
    if (layout.type() != fc::lua::Type::Table || contents.type() != fc::lua::Type::Table) {
        return;
    }
    const auto selected = number(layout.get("SelectedIdx"));
    auto row = selected ? contents.get(*selected) : frame.nil();
    const auto slot = number(popup.get("playerIndex"));
    if (row.type() != fc::lua::Type::Table || !slot || number(row.get("indexstr")) != slot) {
        return;
    }
    const auto name = row.get("namestr").as_string();
    const auto player = name && game_.lobby_player_matches(*slot, *name) ? game_.lobby_player(*slot) : std::nullopt;
    const auto owner = game_.local_user();
    if (!player || !owner) {
        return;
    }
    auto target = frame.table();
    target.set("account_id", account_value(frame, *player));
    target.set("account_owner", account_value(frame, *owner));
    target.set("player_slot", frame.value(*slot));
    target.set("name", frame.value(*name));
    popup.set("FusionCutterBuddyTarget", target);
}

bool Handler::player_buddy(std::int32_t local_player, std::int32_t player) noexcept {
    // Resolve the current occupant before querying the published account IDs.
    const auto user = game_.lobby_player(player);
    if (user && user->value) {
        if (const auto buddy = published_buddy(*user)) {
            return *buddy;
        }
    }

    return original_player_buddy_.call(local_player, player).value_or(false);
}

void Handler::lobby_action(fc::lua::Frame &frame) {
    const auto action = frame.argument(2).as_string();
    if (!action || (*action != "friend" && *action != "delfriend")) {
        forward_original(frame);
        return;
    }
    auto popup = frame.global("Popup_LobbyOpts");
    auto target = popup.type() == fc::lua::Type::Table ? popup.get("FusionCutterBuddyTarget") : frame.nil();
    if (target.type() != fc::lua::Type::Table || !game_.logged_on() || !galaxy_service(frame)) {
        return;
    }
    const auto index = number(frame.argument(0));
    const auto name = frame.argument(1).as_string();
    const auto selected = account_id(target.get("account_id"));
    const auto owner = account_id(target.get("account_owner"));
    const auto current_owner = game_.local_user();
    const auto current = index && name && game_.lobby_player_matches(*index, *name) ? game_.lobby_player(*index) : std::nullopt;
    const auto selected_name = target.get("name").as_string();
    if (!index || number(target.get("player_slot")) != index || !selected || !owner || !current_owner || *owner != *current_owner ||
        !current || *current != *selected || *current == *owner || !name || !selected_name || *selected_name != *name) {
        return;
    }
    // Native guest rejection remains authoritative for both directions of the account action.
    auto flags = frame.call(frame.global("ScriptCB_GetLobbyPlayerFlags"), {frame.argument(1), frame.argument(0)});
    if (!flags || flags->size() < 5 || enabled((*flags)[3])) {
        return;
    }
    const auto state = relationship_state(*selected);
    if (*action == "delfriend") {
        if (state == RelationshipState::Accepted) {
            record_operation(GalaxyOperation::RemoveRelationship, game_.delete_buddy(*selected));
        }
    } else if (state == RelationshipState::None) {
        // Reuse the flags wrapper's native validation and Galaxy eligibility at execution too.
        if (!enabled((*flags)[4])) {
            return;
        }
        const auto player = selected;
        if (!reserve_pending_request(*player, PendingRequestState::Sending)) {
            return;
        }

        const auto result = game_.send_invitation(*player);
        if (!result.accepted()) {
            release_pending_request(*player, PendingRequestState::Sending);
        }

        record_operation(GalaxyOperation::SendInvitation, result);
    }
}

void Handler::buddy_state(fc::lua::Frame &frame) {
    const auto selected = selected_buddy(frame);
    const char *text = "";
    if (!selected) {
        text = accepted_.count + incoming_.count + outgoing_.count == 0 ? "No buddies" : "";
    } else if (*selected < accepted_.count) {
        text = accepted_.entries[*selected].online ? "Online" : "Offline";
    } else if (*selected < accepted_.count + incoming_.count) {
        text = "Buddy request received";
    } else {
        text = "Buddy request sent";
    }

    // The modified state label uses IFText_fnSetUString; row names use plain byte strings.
    auto result = frame.call(frame.global("ScriptCB_tounicode"), {frame.value(text)});
    if (result) {
        frame.return_values(result->values());
    } else {
        frame.error(result.error().message);
    }
}

void Handler::buddy_actions(fc::lua::Frame &frame) {
    auto actions = frame.table();
    const auto selected = selected_buddy(frame);
    if (selected) {
        if (*selected < accepted_.count) {
            const auto &buddy = accepted_.entries[*selected];
            if (buddy.lobby.value || buddy.joinable) {
                actions.set("JOIN", frame.value(1));
            }
            actions.set("REMOVE", frame.value(1));
        } else if (*selected < accepted_.count + incoming_.count) {
            actions.set("ACCEPT", frame.value(1));
            actions.set("DECLINE", frame.value(1));
        } else {
            actions.set("CANCEL", frame.value(1));
        }
    }

    frame.return_values({actions});
}

void Handler::do_buddy_action(fc::lua::Frame &frame) {
    const auto action_value = frame.argument(0).as_string();
    if (!action_value || (*action_value != "JOIN" && *action_value != "REMOVE" && *action_value != "ACCEPT" && *action_value != "DECLINE" &&
                          *action_value != "CANCEL")) {
        forward_original(frame);
        return;
    }
    // Modified friends Lua retains this row when the confirmation opens. Old callers use the current row.
    const auto token = frame.argument(2);
    const auto selected = token.type() == fc::lua::Type::Nil ? selected_buddy(frame) : action_buddy(token);
    if (!selected || !game_.logged_on()) {
        return;
    }

    bool handled{};
    bool lobby_join{};
    auto operation = GalaxyOperation::RemoveRelationship;
    GameBridge::GalaxyCallResult operation_result;
    if (*selected < accepted_.count) {
        const auto &buddy = accepted_.entries[*selected];
        if (*action_value == "JOIN") {
            handled = true;
            lobby_join = true;
            const auto lobby = buddy.lobby.value != 0 ? std::optional{buddy.lobby} : game_.buddy_lobby(buddy.id);
            if (!lobby) {
                record_join_failure({});
                return;
            }
            const auto shell = frame.call(frame.global("ScriptCB_GetShellActive"), {});
            if (!shell || shell->size() != 1) {
                if (!shell) {
                    frame.error(shell.error().message);
                }
                return;
            }
            const auto in_shell = enabled((*shell)[0]);
            if (in_shell) {
                // Stock CancelQuickmatch stops and deletes the previous operation before clearing its slot.
                const auto canceled = frame.call(frame.global("ScriptCB_CancelQuickmatch"), {});
                if (!canceled) {
                    frame.error(canceled.error().message);
                    return;
                }
            }
            if (!game_.queue_lobby_join(*lobby)) {
                record_join_failure(*lobby);
                return;
            }
            if (in_shell) {
                const auto started = frame.call(frame.global("ScriptCB_BeginJoinSpecial"), {});
                if (!started) {
                    frame.error(started.error().message);
                    return;
                }
            }
            // In-game Lua now leaves; retail mp_main sees the pending target and owns the subsequent join.
            frame.return_values({frame.value(true)});
        } else if (*action_value == "REMOVE") {
            handled = true;
            operation_result = game_.delete_buddy(buddy.id);
        }
    } else if (*selected < accepted_.count + incoming_.count) {
        const auto incoming_index = *selected - accepted_.count;
        if (incoming_index < incoming_.count && (*action_value == "ACCEPT" || *action_value == "DECLINE")) {
            handled = true;
            const auto accept = *action_value == "ACCEPT";
            operation = accept ? GalaxyOperation::AcceptInvitation : GalaxyOperation::DeclineInvitation;
            operation_result = game_.respond_to_invitation(incoming_.entries[incoming_index].id, accept);
        }
    } else {
        const auto outgoing_index = *selected - accepted_.count - incoming_.count;
        if (outgoing_index < outgoing_.count && *action_value == "CANCEL") {
            // Cancel uses DeleteFriend too; its reserved completion removes the outgoing row later.
            handled = true;
            const auto user = outgoing_.entries[outgoing_index].id;
            if (!reserve_pending_request(user, PendingRequestState::Canceling)) {
                return;
            }

            operation_result = game_.delete_buddy(user);
            if (!operation_result.accepted()) {
                release_pending_request(user, PendingRequestState::Canceling);
            }
        }
    }

    if (!handled) {
        auto result = frame.original();
        if (result) {
            frame.return_values(result->values());
        } else {
            frame.error(result.error().message);
        }
        return;
    }

    if (!lobby_join) {
        record_operation(operation, operation_result);
    }

    return;
}

} // namespace fc_plugins::galaxy_buddies
