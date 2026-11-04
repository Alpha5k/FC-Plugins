#include "handler.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <expected>
#include <format>

namespace fc_plugins::collision_history_fix {
namespace {

// Retail Soldier embeds CollisionObject at +C and Controllable at +240; player index is +314.
constexpr std::size_t kCollisionObjectOffset = 0xC;
constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerOffset = 0x314;
// CollisionObject +7C/+80/+84 are Soldier +88/+8C/+90, the last three distinct collision objects.
constexpr std::size_t kContactsOffset = 0x88;
// CollisionObject vtable +38 returns its complete GameObject; GameObject vtable +4 selects its class.
constexpr std::size_t kGameObjectGetterOffset = 0x38;
constexpr std::size_t kClassGetterOffset = 0x4;
using GetGameObject = void *(__thiscall *)(void *object) noexcept;

// Preserves game-thread error state while FC observers inspect native identities.
struct AmbientErrors {
    DWORD win32{GetLastError()};
    int winsock{WSAGetLastError()};

    // Win32 is restored last because Winsock can share its native error storage.
    ~AmbientErrors() {
        WSASetLastError(winsock);
        SetLastError(win32);
    }
};

// Copies exactly the twelve-byte priority list, without dereferencing borrowed collision objects.
Contacts capture(const void *soldier) noexcept {
    return fc::read_field<Contacts>(soldier, kContactsOffset);
}

// Replaces only the historical priority slots; native movement and contact responses remain native.
void restore(void *soldier, const Contacts &contacts) noexcept {
    fc::write_field(soldier, kContactsOffset, contacts);
}

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    get_joystick_index_ = plan.function(layout->get_joystick_index);
    is_skipped_ = plan.function(layout->is_skipped);
    accept_pair_memory_ =
        plan.access<const std::byte>(layout->accept_pair.rva, layout->accept_pair.evidence.bytes.size(), layout->accept_pair.evidence);
    soldier_class_getter_memory_ = plan.access<const std::byte>(
        layout->soldier_class_getter.rva, layout->soldier_class_getter.evidence.bytes.size(), layout->soldier_class_getter.evidence);
    prediction_turn_memory_ = plan.access(layout->prediction_turn);
    update_turn_memory_ = plan.access(layout->update_turn);
    local_prediction_turn_memory_ = plan.access(layout->local_prediction_turn);
    max_update_steps_memory_ = plan.access(layout->max_update_steps);
    local_prediction_pass_memory_ = plan.access(layout->local_prediction_pass);
    late_prediction_pass_memory_ = plan.access(layout->late_prediction_pass);
    network_enabled_memory_ = plan.access(layout->network_enabled);
    network_client_active_memory_ = plan.access(layout->network_client_active);
    for (const auto &site : layout->contexts) {
        plan.check(site, site.evidence.bytes.size());
    }

    plan.hook_instruction(layout->pair_admission, [this](fc::CpuContext &context) noexcept {
        const AmbientErrors errors;
        admit_pair(context);
    });
    collision_response_ = plan.hook(layout->collision_response, [this](void *object, void *result, void *other, void *restrictor) noexcept {
        return collision_response(object, result, other, restrictor);
    });

    plan.observe(layout->predict_turn, fc::before([this]() noexcept {
                     const AmbientErrors errors;
                     before_predict_turn();
                 }));
    plan.observe(layout->predict, fc::before([this]() noexcept {
                     const AmbientErrors errors;
                     before_predict();
                 }),
                 fc::after([this](bool) noexcept {
                     const AmbientErrors errors;
                     after_predict();
                 }));
    plan.observe<Invocation>(layout->read, fc::before([this](void *soldier, void *, void *, Invocation &invocation) noexcept {
                                 const AmbientErrors errors;
                                 invocation.previous = read_invocation_;
                                 read_invocation_ = &invocation;
                                 if (!prediction_active()) {
                                     return;
                                 }
                                 invocation.identity = identity(soldier);
                                 if (!invocation.identity.valid()) {
                                     return;
                                 }
                                 for (auto *previous = invocation.previous; previous != nullptr; previous = previous->previous) {
                                     if (previous->identity.soldier == invocation.identity.soldier) {
                                         invocation.identity = {};
                                         return;
                                     }
                                 }
                                 // ReadObject restores movement for the update frontier, independently of input acknowledgement.
                                 invocation.turn = *update_turn_;
                                 if (const auto contacts = history_.rewind(invocation.identity, invocation.turn)) {
                                     restore(soldier, *contacts);
                                 }
                             }),
                             fc::after([this](void *soldier, void *, void *, const Invocation &invocation) noexcept {
                                 const AmbientErrors errors;
                                 if (invocation.identity.valid()) {
                                     // A completed reader owns the seed; PredictTurn must not restore its old pre-read values.
                                     history_.capture(invocation.identity, invocation.turn, capture(soldier));
                                 }
                                 read_invocation_ = invocation.previous;
                             }));
    plan.observe<Invocation>(layout->update, fc::before([this](void *controllable, float, Invocation &invocation) noexcept {
                                 const AmbientErrors errors;
                                 invocation.previous = update_invocation_;
                                 update_invocation_ = &invocation;
                                 if (!prediction_active() || controllable == nullptr) {
                                     return;
                                 }
                                 auto *soldier = static_cast<std::byte *>(controllable) - kControllableOffset;
                                 invocation.identity = identity(soldier);
                                 if (!invocation.identity.valid()) {
                                     return;
                                 }
                                 for (auto *previous = invocation.previous; previous != nullptr; previous = previous->previous) {
                                     if (previous->identity.soldier == invocation.identity.soldier) {
                                         invocation.identity = {};
                                         return;
                                     }
                                 }
                                 invocation.turn = *prediction_turn_;
                                 // The current pass flag is assigned after PredictTurn. A live pass can establish a missing baseline.
                                 if (*local_prediction_pass_ != 0) {
                                     history_.start_frontier(invocation.identity, invocation.turn, capture(soldier));
                                 } else {
                                     history_.capture(invocation.identity, invocation.turn, capture(soldier));
                                 }
                             }),
                             fc::after([this](void *, float, bool result, const Invocation &invocation) noexcept {
                                 const AmbientErrors errors;
                                 if (invocation.identity.valid()) {
                                     if (result) {
                                         history_.advanced(invocation.identity, invocation.turn);
                                     } else {
                                         retire_soldier(invocation.identity.soldier);
                                     }
                                 }
                                 update_invocation_ = invocation.previous;
                             }));
    plan.observe(layout->reset, fc::before([this](void *soldier) noexcept {
                     const AmbientErrors errors;
                     retire_soldier(reinterpret_cast<std::uintptr_t>(soldier));
                 }));
    plan.observe(layout->destroy_collision_object, fc::before([this](void *object) noexcept {
                     const AmbientErrors errors;
                     retire_collision_object(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(object)));
                 }));
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    game_ = context.game();
    // Borrowed views remain valid for the installed handler's lifetime.
    const auto bind_view = []<class T>(const fc::Memory<T> &memory, T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }
        pointer = view->data();
        return {};
    };
    if (auto result = bind_view(accept_pair_memory_, accept_pair_); !result) {
        return result;
    }
    if (auto result = bind_view(soldier_class_getter_memory_, soldier_class_getter_); !result) {
        return result;
    }
    if (auto result = bind_view(prediction_turn_memory_, prediction_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(update_turn_memory_, update_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(local_prediction_turn_memory_, local_prediction_turn_); !result) {
        return result;
    }
    if (auto result = bind_view(max_update_steps_memory_, max_update_steps_); !result) {
        return result;
    }
    if (auto result = bind_view(local_prediction_pass_memory_, local_prediction_pass_); !result) {
        return result;
    }
    if (auto result = bind_view(late_prediction_pass_memory_, late_prediction_pass_); !result) {
        return result;
    }
    if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
        return result;
    }
    return bind_view(network_client_active_memory_, network_client_active_);
}

bool Handler::historical_collision() const noexcept {
    return *network_enabled_ != 0 && *network_client_active_ != 0 && *local_prediction_pass_ == 0 && *late_prediction_pass_ == 0;
}

void *Handler::soldier_object(void *object) const noexcept {
    if (object == nullptr) {
        return nullptr;
    }
    const auto *table = fc::read_field<const std::byte *>(object, 0);
    const auto getter = fc::read_field<GetGameObject>(table, kGameObjectGetterOffset);
    auto *soldier = getter(object);
    if (soldier == nullptr) {
        return nullptr;
    }
    const auto *soldier_table = fc::read_field<const std::byte *>(soldier, 0);
    return fc::read_field<const std::byte *>(soldier_table, kClassGetterOffset) == soldier_class_getter_ ? soldier : nullptr;
}

void Handler::admit_pair(fc::CpuContext &context) const noexcept {
    if ((context.ebx & 0xFFU) != 0 || static_cast<std::int32_t>(context.ecx) > static_cast<std::int32_t>(context.edi) ||
        context.ecx == context.edi || !historical_collision()) {
        return;
    }
    auto *current = soldier_object(reinterpret_cast<void *>(context.edi));
    auto *other = soldier_object(reinterpret_cast<void *>(context.ecx));
    if (current == nullptr || other == nullptr) {
        return;
    }
    // The manager already admitted the active owner. A skipped Soldier cannot process the opposite address order.
    float delta{};
    if (is_skipped_.call(other, &delta).value_or(false)) {
        // Resume after both native branches; the callback sequence overwrites flags before using them.
        context.eip = reinterpret_cast<std::uint32_t>(accept_pair_);
    }
}

bool Handler::collision_response(void *object, void *result, void *other, void *restrictor) const noexcept {
    {
        const AmbientErrors errors;
        if (historical_collision()) {
            auto *soldier = soldier_object(object);
            auto *other_soldier = soldier_object(other);
            if (soldier != nullptr && other_soldier != nullptr) {
                float delta{};
                const auto skipped = is_skipped_.call(soldier, &delta);
                const auto other_skipped = is_skipped_.call(other_soldier, &delta);
                if (skipped && *skipped && other_skipped && !*other_skipped) {
                    // CheckForCollision ignores this result; the skipped actor retains every native response field.
                    return false;
                }
            }
        }
    }
    return collision_response_.call(object, result, other, restrictor).value_or(false);
}

void Handler::before_predict() noexcept {
    if (*network_enabled_ == 0 || *network_client_active_ == 0) {
        return;
    }

    // Predict divides by this count. Use at least one when there are no pending turns or the frontier is behind.
    const auto frontier = *local_prediction_turn_;
    const auto update = *update_turn_;
    *max_update_steps_ = frontier > update ? frontier - update : 1;
}

void Handler::before_predict_turn() noexcept {
    if (!prediction_active()) {
        return;
    }
    // Pass flags describe the preceding iteration here; the native prediction turn names this boundary.
    const auto turn = *prediction_turn_;
    for (std::size_t local_player{}; local_player < kLocalPlayerCount; ++local_player) {
        const auto tracked = history_.tracked(local_player);
        if (!tracked.valid()) {
            continue;
        }
        auto *soldier = reinterpret_cast<void *>(tracked.soldier);
        if (identity(soldier) != tracked) {
            retire_soldier(tracked.soldier);
            continue;
        }
        if (history_.needs_rewind(tracked, turn)) {
            if (const auto contacts = history_.rewind(tracked, turn)) {
                restore(soldier, *contacts);
            }
        }
        history_.capture(tracked, turn, capture(soldier));
    }
}

void Handler::after_predict() noexcept {
    if (!prediction_active() || update_invocation_ != nullptr || read_invocation_ != nullptr) {
        return;
    }
    for (std::size_t local_player{}; local_player < kLocalPlayerCount; ++local_player) {
        const auto tracked = history_.tracked(local_player);
        if (!tracked.valid()) {
            continue;
        }
        auto *soldier = reinterpret_cast<void *>(tracked.soldier);
        if (identity(soldier) != tracked) {
            retire_soldier(tracked.soldier);
            continue;
        }
        // Predict can report false after world work; a known resident boundary still supplies a valid list.
        history_.complete(tracked, *prediction_turn_, capture(soldier));
    }
}

SoldierIdentity Handler::identity(void *soldier) const noexcept {
    if (soldier == nullptr) {
        return {};
    }
    const auto player = fc::read_field<std::int32_t>(soldier, kPlayerOffset);
    if (player < 0 || player >= 64) {
        return {};
    }
    const auto local_player = get_joystick_index_.call(player);
    if (!local_player || *local_player < 0 || *local_player >= static_cast<std::int32_t>(kLocalPlayerCount)) {
        return {};
    }
    return {
        .soldier = reinterpret_cast<std::uintptr_t>(soldier), .player = player, .local_player = static_cast<std::uint8_t>(*local_player)};
}

bool Handler::prediction_active() noexcept {
    const auto state = game_.current();
    if (!state || state->session == 0 || (state->mode != fc::GameMode::Client && state->mode != fc::GameMode::Host)) {
        clear_tracking();
        session_ = 0;
        return false;
    }
    if (session_ != state->session) {
        clear_tracking();
        session_ = state->session;
    }
    return *network_enabled_ != 0 && *network_client_active_ != 0 && *prediction_turn_ > 0 && *update_turn_ >= 0;
}

void Handler::retire_soldier(std::uintptr_t soldier) noexcept {
    history_.clear_soldier(soldier);
    for (auto *head : {update_invocation_, read_invocation_}) {
        for (auto *invocation = head; invocation != nullptr; invocation = invocation->previous) {
            if (invocation->identity.soldier == soldier) {
                invocation->identity = {};
            }
        }
    }
}

void Handler::retire_collision_object(std::uint32_t object) noexcept {
    if (object == 0) {
        return;
    }
    // Soldier's common base destructor receives Soldier+C; remove its retained identity before accessing survivors.
    retire_soldier(object - kCollisionObjectOffset);
    history_.forget_object(object);
    for (std::size_t local_player{}; local_player < kLocalPlayerCount; ++local_player) {
        const auto tracked = history_.tracked(local_player);
        if (!tracked.valid()) {
            continue;
        }
        auto *soldier = reinterpret_cast<void *>(tracked.soldier);
        auto contacts = capture(soldier);
        // Native lists also retain raw pointers. Retire the matching live slot so later captures cannot resurrect it.
        History::forget_contact(contacts, object);
        restore(soldier, contacts);
    }
}

void Handler::clear_tracking() noexcept {
    history_.clear_all();
    for (auto *head : {update_invocation_, read_invocation_}) {
        for (auto *invocation = head; invocation != nullptr; invocation = invocation->previous) {
            invocation->identity = {};
        }
    }
}

} // namespace fc_plugins::collision_history_fix
