#include "handler.hpp"

#include <array>
#include <format>
#include <mutex>

namespace fc_plugins::galaxy_buddies {
namespace {
constexpr std::array kLobbyIdFormat{'%', 'l', 'l', 'u', '\0'};

struct ListenerBinding {
    std::uint32_t type{};
    void *listener{};
};

// Suppresses only this color override; the native palette and shared friendship predicate stay intact.
void guard_buddy_color(fc::CpuContext &context, std::uintptr_t local, std::uintptr_t remote) noexcept {
    // Both retail builds store the player's actual team at +0x134; zero and negative teams are unassigned.
    const auto local_team = local != 0 ? fc::read_field<std::int32_t>(local, 0x134) : 0;
    if (local_team <= 0 || remote == 0 || local_team != fc::read_field<std::int32_t>(remote, 0x134)) {
        context.eax &= ~0xFFU;
        // Skip this five-byte friendship CALL. Its following TEST AL,AL keeps the native base color.
        context.eip += 5;
    }
}
} // namespace

Handler::Handler(const fc::CreateContext &context) noexcept
    : friend_list_listener_(*this), invitation_list_listener_(*this), persona_state_listener_(*this), rich_presence_listener_(*this),
      send_listener_(*this), invitation_listener_(*this), respond_listener_(*this), friend_add_listener_(*this),
      friend_delete_listener_(*this), specific_user_data_listener_(*this), logger_(context.log()) {}

Handler::~Handler() {
    unregister_listeners();
}

void Handler::plan(fc::Plan &plan) {
    auto game = plan.for_image("game");
    const auto layout = select_layout(game.profile());
    if (!layout) {
        game.fail(std::format("No native layout for image profile '{}'", game.profile()));
        return;
    }

    game_.plan(game, *layout);
    game.observe(layout->native_update, fc::after([this](const bool &) noexcept {
                     native_update();
                 }));

    // The shared retail "%ull" format reads 32 bits and appends literal "ll".
    // Its consumers include lobby presence/JOIN and console player-ID hashes.
    game.write(layout->lobby_id_format, kLobbyIdFormat);
    original_player_buddy_ = game.hook(layout->is_player_buddy, [this](std::int32_t local_player, std::int32_t player) noexcept {
        return player_buddy(local_player, player);
    });
    get_player_ = game.function(layout->get_player);
    game.hook_instruction(layout->map_buddy_color, [this](fc::CpuContext &context) noexcept {
        const auto local = get_player_.call(static_cast<std::int32_t>(context.eax)).value_or(nullptr);
        const auto remote = get_player_.call(static_cast<std::int32_t>(context.edx)).value_or(nullptr);
        guard_buddy_color(context, reinterpret_cast<std::uintptr_t>(local), reinterpret_cast<std::uintptr_t>(remote));
    });
    game.hook_instruction(layout->name_buddy_color, [](fc::CpuContext &context) noexcept {
        guard_buddy_color(context, context.esi, context.edi);
    });
    game.hook_instruction(layout->voice_buddy_color, [](fc::CpuContext &context) noexcept {
        const auto local = fc::read_field<std::uint32_t>(fc::frame_pointer(context), -0x0C);
        guard_buddy_color(context, local, context.ebx);
    });
    original_add_lobby_ = game.hook(layout->add_lobby, [this](const GalaxyId *lobby) noexcept {
        add_lobby(lobby);
    });
    original_publish_friends_ = game.hook(layout->publish_friends, [this]() noexcept {
        publish_friends();
    });

    if (layout->needs_peer) {
        auto peer = plan.for_image("peer");
        plan_broker(peer);
    }
}

bool Handler::ensure_listeners() noexcept {
    if (listeners_registered_) {
        return true;
    }

    // The loaded Galaxy DLLs can still be initializing. Wait for their registrar
    // before attempting to register the handler's borrowed listener pointers.
    if (!game_.callbacks_available()) {
        return false;
    }

    const std::array bindings{
        ListenerBinding{kFriendListListener, &friend_list_listener_},
        ListenerBinding{kInvitationListListener, &invitation_list_listener_},
        ListenerBinding{kPersonaStateListener, &persona_state_listener_},
        ListenerBinding{kRichPresenceListener, &rich_presence_listener_},
        ListenerBinding{kSendListener, &send_listener_},
        ListenerBinding{kInvitationListener, &invitation_listener_},
        ListenerBinding{kRespondListener, &respond_listener_},
        ListenerBinding{kFriendAddListener, &friend_add_listener_},
        ListenerBinding{kFriendDeleteListener, &friend_delete_listener_},
        ListenerBinding{kSpecificUserDataListener, &specific_user_data_listener_},
    };

    std::size_t registered_count{};
    // Attempt to unregister earlier entries in reverse order; a later retry attempts the full set.
    for (const auto &binding : bindings) {
        if (!game_.register_listener(binding.type, binding.listener)) {
            for (std::size_t index = registered_count; index > 0; --index) {
                game_.unregister_listener(bindings[index - 1].type, bindings[index - 1].listener);
            }
            if (!listener_warning_claimed_.exchange(true, std::memory_order_acq_rel)) {
                record_listener_failure();
            }

            return false;
        }
        ++registered_count;
    }

    listeners_registered_ = true;
    // Registration can follow a stock completion; read its ready cache without waiting for another callback.
    if (game_.accepted_ready() && update_accepted()) {
        publish_buddies();
    }
    callback_requests_.fetch_or(kRequestRelationships, std::memory_order_release);
    schedule_display_name_refresh();
    return true;
}

void Handler::unregister_listeners() noexcept {
    if (!listeners_registered_) {
        return;
    }

    const std::array bindings{
        ListenerBinding{kFriendListListener, &friend_list_listener_},
        ListenerBinding{kInvitationListListener, &invitation_list_listener_},
        ListenerBinding{kPersonaStateListener, &persona_state_listener_},
        ListenerBinding{kRichPresenceListener, &rich_presence_listener_},
        ListenerBinding{kSendListener, &send_listener_},
        ListenerBinding{kInvitationListener, &invitation_listener_},
        ListenerBinding{kRespondListener, &respond_listener_},
        ListenerBinding{kFriendAddListener, &friend_add_listener_},
        ListenerBinding{kFriendDeleteListener, &friend_delete_listener_},
        ListenerBinding{kSpecificUserDataListener, &specific_user_data_listener_},
    };
    for (std::size_t index = bindings.size(); index > 0; --index) {
        game_.unregister_listener(bindings[index - 1].type, bindings[index - 1].listener);
    }

    listeners_registered_ = false;
}

void Handler::native_update() noexcept {
    // Lua wrappers also hold this mutex; repaint can re-enter them on the same thread.
    std::lock_guard lock{state_mutex_};
    if (!listeners_registered_) {
        const auto now = Clock::now();
        if (now >= next_listener_retry_) {
            if (!ensure_listeners()) {
                next_listener_retry_ = now + kListenerRetryInterval;
            }
        }
    }

    // A replacement accepted-list request clears the native ready byte.
    // Consume its previous completion before servicing requests that could invalidate it.
    const bool accepted_changed = accepted_response_ready_.load(std::memory_order_acquire) && update_accepted();
    service_callback_requests();
    service_display_names();
    reconcile_state(accepted_changed);
}

} // namespace fc_plugins::galaxy_buddies
