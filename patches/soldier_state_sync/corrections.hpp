#pragma once

#include <fc/plugin.hpp>

namespace fc_plugins::soldier_state_sync {

// Owns movement rotation agreement and the returned native sine/cosine corrections.
class GroundRotation final {
  public:
    // Normalizes the returned Soldier pairs before ground, air and jet hover matrix construction.
    void plan(fc::Plan &plan);

    // Registers both roles so hosts advertise corrected movement and joining clients can require agreement.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // Local and hosted games own movement. Joining clients require agreement with their current host.
    bool applies() const noexcept;

    fc::Game game_;
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
};

// Clears stale timer fractions without changing native state conversion or the zero-coarse branch.
void plan_trigger(fc::Plan &plan);

} // namespace fc_plugins::soldier_state_sync
