#include "participants.hpp"

#include <algorithm>
#include <utility>

namespace fc_plugins::multiplayer_gc {

void Participants::start(fc::PeerId host, std::string name, std::uint8_t faction, bool cooperative) {
    clear();
    players_ = {{host, std::move(name), faction}};
    cooperative_ = cooperative;
    host_faction_ = faction;
    commanders_[faction - 1] = host;
}

fc::Result<> Participants::ready(fc::PeerId peer, std::string name) {
    const auto current = std::ranges::find(players_, peer, &Participant::peer);
    if (current != players_.end()) {
        current->name = std::move(name);
        return {};
    }

    // The host already occupies one place; count all accepted players before admitting another.
    if (cooperative_ && std::ranges::count(players_, host_faction_, &Participant::faction) >= 8) {
        return std::unexpected(fc::Error{"This campaign is full."});
    }
    players_.push_back({peer, std::move(name), cooperative_ ? host_faction_ : std::uint8_t{0}});
    if (cooperative_ && !commanders_[host_faction_ - 1].value) {
        commanders_[host_faction_ - 1] = peer;
    }
    return {};
}

fc::Result<> Participants::choose(fc::PeerId peer, std::uint8_t faction) {
    const auto current = std::ranges::find(players_, peer, &Participant::peer);
    if (current == players_.end()) {
        return std::unexpected(fc::Error{"The campaign is not ready. Wait before choosing a faction."});
    }
    if (faction != 1 && faction != 2) {
        return std::unexpected(fc::Error{"Choose one of this campaign's factions."});
    }
    if (current->faction == faction) {
        return {};
    }
    if (current->faction != 0) {
        return std::unexpected(fc::Error{"Leave and rejoin to choose another faction."});
    }

    if (std::ranges::count(players_, faction, &Participant::faction) >= 8) {
        return std::unexpected(fc::Error{"That faction is full. Choose the other faction or wait for a place."});
    }
    current->faction = faction;
    if (!commanders_[faction - 1].value) {
        commanders_[faction - 1] = peer;
    }
    return {};
}

void Participants::remove(fc::PeerId peer) {
    // Select the successor before erasing the departing member's position in join order.
    for (std::uint8_t faction = 1; faction <= 2; ++faction) {
        if (commanders_[faction - 1] == peer) {
            const auto next = next_commander(faction, peer);
            commanders_[faction - 1] = next == peer ? fc::PeerId{} : next;
        }
    }
    std::erase_if(players_, [peer](const Participant &player) {
        return player.peer == peer;
    });
}

fc::PeerId Participants::next_commander(std::uint8_t faction, fc::PeerId previous) const {
    fc::PeerId first{};
    bool passed = !previous.value;
    for (const auto &player : players_) {
        if (player.faction != faction) {
            continue;
        }
        if (!first.value) {
            first = player.peer;
        }
        if (passed) {
            return player.peer;
        }
        passed = player.peer == previous;
    }
    return first;
}

void Participants::begin_turn(std::uint8_t faction, std::uint32_t turn) {
    if (faction < 1 || faction > 2) {
        return;
    }
    auto &last = turns_[faction - 1];
    if (last && *last != turn) {
        commanders_[faction - 1] = next_commander(faction, commanders_[faction - 1]);
    }
    last = turn;
}

fc::PeerId Participants::commander(std::uint8_t faction) const {
    return faction >= 1 && faction <= 2 ? commanders_[faction - 1] : fc::PeerId{};
}

void Participants::clear() {
    players_.clear();
    commanders_ = {};
    turns_ = {};
    cooperative_ = false;
    host_faction_ = 0;
}

const Participant *Participants::find(fc::PeerId peer) const {
    const auto current = std::ranges::find(players_, peer, &Participant::peer);
    return current == players_.end() ? nullptr : &*current;
}

std::size_t Participants::accepted_count() const {
    return static_cast<std::size_t>(std::ranges::count_if(players_, [](const auto &player) {
        return player.faction != 0;
    }));
}

} // namespace fc_plugins::multiplayer_gc
