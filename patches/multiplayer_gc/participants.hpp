#pragma once

#include <fc/multiplayer.hpp>

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fc_plugins::multiplayer_gc {

// One connected player whose addon is ready to participate in this campaign.
struct Participant {
    fc::PeerId peer;
    std::string name;
    std::uint8_t faction{}; // Zero waits for a choice; one and two are the campaign's faction indices.
};

// Holds the host's accepted players in join order. The caller owns connection and addon-readiness checks.
class Participants {
  public:
    // Starts with the host on the loaded campaign's faction, which must be one or two.
    void start(fc::PeerId host, std::string name, std::uint8_t faction, bool cooperative);

    // Adds a ready player, assigning co-op players to the host. Repeated readiness preserves the accepted faction.
    fc::Result<> ready(fc::PeerId peer, std::string name);

    // Accepts an available faction once per visit; choosing the already accepted faction succeeds again.
    fc::Result<> choose(fc::PeerId peer, std::uint8_t faction);

    // Removes a departing player and their assignment, leaving the remaining join order intact.
    void remove(fc::PeerId peer);

    // Rotates this faction's commander once when its next turn begins; the first turn keeps the first member.
    void begin_turn(std::uint8_t faction, std::uint32_t turn);

    // Returns the current commander, or an empty peer when the faction is controlled by AI.
    fc::PeerId commander(std::uint8_t faction) const;

    // Discards the session roster and hosting settings.
    void clear();

    // Returns the current participant, or null when the peer has not completed addon readiness.
    // The returned pointer remains valid until this roster changes.
    const Participant *find(fc::PeerId peer) const;

    // Counts assigned campaign players; visitors choosing a faction do not occupy an advertised participant place.
    std::size_t accepted_count() const;

    const std::vector<Participant> &list() const {
        return players_;
    }

    bool cooperative() const {
        return cooperative_;
    }

    std::uint8_t host_faction() const {
        return host_faction_;
    }

  private:
    // Chooses the next faction member in join order, wrapping after the last member.
    fc::PeerId next_commander(std::uint8_t faction, fc::PeerId previous) const;

    std::vector<Participant> players_;
    std::array<fc::PeerId, 2> commanders_{};
    std::array<std::optional<std::uint32_t>, 2> turns_{};
    bool cooperative_{};
    std::uint8_t host_faction_{};
};

} // namespace fc_plugins::multiplayer_gc
