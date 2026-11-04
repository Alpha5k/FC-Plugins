#pragma once

#include "../game.hpp"
#include "../association.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace fc_plugins::direct_transport::server {

// Retains a destination's original physical slot until its last nested synchronous group call returns.
class GroupDestinations {
  public:
    // Stores the first primary slot; nested calls use it even when the native endpoint has changed.
    bool begin(std::int32_t destination, std::int32_t primary) noexcept {
        if (destination < 0 || destination >= static_cast<std::int32_t>(destinations_.size())) {
            return false;
        }

        auto &entry = destinations_[destination];
        if (entry.depth == (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }

        if (entry.depth == 0) {
            entry.primary = primary;
        }
        ++entry.depth;
        return true;
    }

    // Returns the original primary slot, or -1 when this destination has no active native group.
    std::int32_t primary(std::int32_t destination) const noexcept {
        if (destination < 0 || destination >= static_cast<std::int32_t>(destinations_.size())) {
            return -1;
        }

        const auto &entry = destinations_[destination];
        return entry.depth != 0 ? entry.primary : -1;
    }

    // Uses the active synchronous group's original primary slot; otherwise resolves the current native endpoint.
    template <typename Resolve> std::int32_t resolve(std::int32_t destination, Resolve resolve_current) const noexcept {
        const auto retained = primary(destination);
        return retained >= 0 ? retained : resolve_current(destination);
    }

    // Balances one successful begin without releasing an outer invocation's destination mapping.
    void end(std::int32_t destination) noexcept {
        if (destination < 0 || destination >= static_cast<std::int32_t>(destinations_.size())) {
            return;
        }

        auto &entry = destinations_[destination];
        if (entry.depth != 0 && --entry.depth == 0) {
            entry.primary = -1;
        }
    }

  private:
    struct Entry {
        // Physical association slot selected by the outer call, even if native endpoints later change.
        std::int32_t primary{-1};
        std::uint32_t depth{};
    };

    std::array<Entry, direct_transport::kNativeEndpointCount> destinations_{};
};

// A grouped fragment must still name its original connected association and full internal generation.
inline bool group_destination_matches(const direct_transport::TransmitGroup &group, std::int32_t primary, std::int32_t current_primary,
                                      bool live, std::uint64_t generation) noexcept {
    return live && primary == current_primary && group.belongs_to(static_cast<std::uint8_t>(primary), generation);
}

} // namespace fc_plugins::direct_transport::server
