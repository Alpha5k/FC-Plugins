#pragma once

#include "layout.hpp"

#include <fc/plugin.hpp>

namespace fc_plugins::shot_sequence {

// Corrects cooldown completion and preserves local pattern indices during client replay.
class Handler {
  public:
    explicit Handler(fc::Role role) noexcept : role_(role) {}

    // Plans cooldown correction for both roles and pattern preservation for client launches.
    void plan(fc::Plan &plan);
    // Registers cooldown agreement and binds client pattern flags for the installed lifetime.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // FC displaces the short JC and following seven-byte TEST; native code never enters that TEST directly.
    void plan_cooldown(fc::Plan &plan, const Layout &layout);
    // Retains native ownership and pass flags, and hooks the single pattern store.
    void plan_pattern(fc::Plan &plan, const Layout &layout);
    // Borrows prepared native flags for the installed handler's lifetime.
    fc::Result<> prepare_pattern();
    // Local and hosted games use corrected timing. Joining clients require agreement with their current host.
    bool applies() const noexcept;

    fc::Role role_;
    fc::Game game_;
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::Memory<const std::uint8_t> network_enabled_memory_;
    fc::Memory<const std::uint8_t> client_world_memory_;
    fc::Memory<const std::uint8_t> local_turn_memory_;
    const std::uint8_t *network_enabled_{};
    const std::uint8_t *client_world_{};
    const std::uint8_t *local_turn_{};
};

} // namespace fc_plugins::shot_sequence
