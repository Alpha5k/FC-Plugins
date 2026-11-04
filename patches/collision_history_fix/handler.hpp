#pragma once

#include "history.hpp"
#include "layout.hpp"

#include <fc/game.hpp>
#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <cstddef>
#include <cstdint>

namespace fc_plugins::collision_history_fix {

// Rewinds local contact priority and lets historical local collisions leave skipped Soldiers unchanged.
class Handler final {
  public:
    // Reserves qualified movement, collision and lifetime hooks for the selected retail image.
    void plan(fc::Plan &plan);
    // Resolves native globals before observers execute on the game thread.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    struct Invocation {
        Invocation *previous{};
        SoldierIdentity identity;
        std::int32_t turn{-1};
    };

    // Allows ordinary remote objects to advance once per pending historical turn.
    void before_predict() noexcept;
    // Saves or restores tracked soldiers before controller input and collision simulation begin.
    void before_predict_turn() noexcept;
    // Saves a known final boundary after every synchronous world update inside Predict.
    void after_predict() noexcept;
    // Captures the native player/local-slot assignment directly from a complete borrowed Soldier.
    SoldierIdentity identity(void *soldier) const noexcept;
    // Uses the current session and native client-world selection, preserving history across host-world switches.
    bool prediction_active() noexcept;
    // Identifies historical client-world work using the native pass flags at the collision boundary.
    bool historical_collision() const noexcept;
    // Retrieves a complete Soldier through CollisionObject's getter and verifies its native class getter.
    void *soldier_object(void *object) const noexcept;
    // Grants an omitted pair to its active owner when the other Soldier's update is skipped.
    void admit_pair(fc::CpuContext &context) const noexcept;
    // Omits only the skipped Soldier's historical response; other calls retain the original boolean result.
    bool collision_response(void *object, void *result, void *other, void *restrictor) const noexcept;
    // Invalidates retained checkpoints and nested calls before a soldier resets or is freed.
    void retire_soldier(std::uintptr_t soldier) noexcept;
    // Removes a destroyed CollisionObject from saved and resident lists before native detachment/free.
    void retire_collision_object(std::uint32_t object) noexcept;
    // Releases session history and disarms any active calls that retained the previous session.
    void clear_tracking() noexcept;

    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::NativeFunction<IsSkipped> is_skipped_;
    fc::NativeFunction<CollisionResponse> collision_response_;
    fc::Memory<const std::byte> accept_pair_memory_;
    fc::Memory<const std::byte> soldier_class_getter_memory_;
    const std::byte *accept_pair_{};
    const std::byte *soldier_class_getter_{};
    fc::Memory<const std::int32_t> prediction_turn_memory_;
    fc::Memory<const std::int32_t> update_turn_memory_;
    fc::Memory<const std::int32_t> local_prediction_turn_memory_;
    fc::Memory<std::int32_t> max_update_steps_memory_;
    fc::Memory<const std::uint8_t> local_prediction_pass_memory_;
    fc::Memory<const std::uint8_t> late_prediction_pass_memory_;
    fc::Memory<const std::uint8_t> network_enabled_memory_;
    fc::Memory<const std::uint8_t> network_client_active_memory_;
    const std::int32_t *prediction_turn_{};
    const std::int32_t *update_turn_{};
    const std::int32_t *local_prediction_turn_{};
    std::int32_t *max_update_steps_{};
    const std::uint8_t *local_prediction_pass_{};
    const std::uint8_t *late_prediction_pass_{};
    const std::uint8_t *network_enabled_{};
    const std::uint8_t *network_client_active_{};

    // Observer-owned records retain nested native calls and can be disarmed by reset/destruction.
    Invocation *update_invocation_{};
    Invocation *read_invocation_{};
    History history_;
    fc::Game game_;
    fc::SessionId session_{};
};

} // namespace fc_plugins::collision_history_fix
