#pragma once

#include "protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fc_plugins::multiplayer_gc {

// Battle-only roles are indexed by native player slot. Zero means absent.
class BattleRoles {
  public:
    bool set(double slot, double role) noexcept {
        if (!(slot >= 0 && slot < roles_.size()) || (role != 1 && role != 2)) {
            return false;
        }
        const auto index = static_cast<std::size_t>(slot);
        if (slot != index || roles_[index]) {
            return false;
        }
        roles_[index] = static_cast<std::uint8_t>(role);
        return true;
    }

    std::uint8_t role(std::uint32_t slot) const noexcept {
        return slot < roles_.size() ? roles_[slot] : 0;
    }

    // All admitted players need a role, and no unassigned visitor may be included.
    bool matches(std::span<const Player> players) const noexcept {
        std::array<bool, 64> admitted{};
        for (const auto &player : players) {
            if (player.faction == 1 || player.faction == 2) {
                if (!role(player.slot)) {
                    return false;
                }
                admitted[player.slot] = true;
            }
        }
        bool assigned{};
        for (std::size_t slot = 0; slot < admitted.size(); ++slot) {
            if ((roles_[slot] != 0) != admitted[slot]) {
                return false;
            }
            assigned |= admitted[slot];
        }
        return assigned;
    }

  private:
    std::array<std::uint8_t, 64> roles_{};
};

// Side codes may resolve to either physical team. Use the resolved numbers for both paths.
inline std::uint32_t battle_team(const Player &player, const std::array<std::uint32_t, 2> &teams,
                                 const std::optional<BattleRoles> &roles) noexcept {
    if (player.faction != 1 && player.faction != 2) {
        return 0;
    }
    const auto side = roles ? roles->role(player.slot) : player.faction;
    return side ? teams[side - 1] : 0;
}

} // namespace fc_plugins::multiplayer_gc
