#pragma once

#include "protocol.hpp"

#include <fc/multiplayer.hpp>

#include <array>
#include <mutex>
#include <optional>

namespace fc_plugins::direct_transport {

// Keeps the FC connection identity with the Galaxy user ID and native endpoint slot.
struct Peer {
    // FC assigns a new ID on reconnect, even when the user and slot repeat.
    fc::PeerId id;
    // Resolved user ID; a joining client's native host endpoint instead contains the lobby ID.
    std::uint64_t galaxy_id{};
    // Physical client endpoints are 0–63; the joining client's host endpoint is 64.
    std::uint8_t slot{};
};

// Copies FC peer changes and controls for the native thread. Taking a copy tries the mutex once.
class ControlMailbox {
  public:
    struct Publication {
        Peer peer;
        fc::Agreement agreement{fc::Agreement::Unavailable};
        // One unread record per kind, indexed Offer, Ready, Commit; storage owns the copied key.
        std::array<ParsedControl, 3> controls{};
        std::array<bool, 3> present{};
        // A peer change or unread control makes this slot eligible for take_if.
        bool changed{};
    };

    // Copies an FC peer change; replacements discard the previous connection's unread controls.
    void publish(Peer peer, fc::Agreement agreement) noexcept {
        if (peer.slot >= slots_.size()) {
            return;
        }

        const std::lock_guard lock{mutex_};
        auto &slot = slots_[peer.slot];
        if (slot.peer.id != peer.id || slot.peer.galaxy_id != peer.galaxy_id) {
            slot = {};
        }
        slot.peer = peer;
        slot.agreement = agreement;
        slot.changed = true;
    }

    // Withdraws only the ended FC peer, so late callbacks cannot erase a reused native slot.
    void withdraw(fc::PeerId peer) noexcept {
        const std::lock_guard lock{mutex_};
        for (auto &slot : slots_) {
            if (slot.peer.id == peer) {
                slot.agreement = fc::Agreement::Unavailable;
                slot.present = {};
                slot.changed = true;
            }
        }
    }

    // Retains one copy per protocol kind; repeats cannot accumulate behind a paused native service.
    void receive(fc::PeerId peer, const ParsedControl &control) noexcept {
        const auto kind = static_cast<std::size_t>(control.kind) - 1;
        if (kind >= 3) {
            return;
        }

        const std::lock_guard lock{mutex_};
        for (auto &slot : slots_) {
            if (slot.peer.id == peer && slot.agreement == fc::Agreement::Compatible) {
                // Keep the first message until native validation; a duplicate cannot replace its connection ID or key.
                if (!slot.present[kind]) {
                    slot.controls[kind] = control;
                    slot.present[kind] = true;
                    slot.changed = true;
                }
                return;
            }
        }
    }

    // Tries once on the native thread; contention defers this publication to the next receive opportunity.
    std::optional<Publication> take(std::size_t index) noexcept {
        return take_if(index, [](const Publication &) noexcept {
            return true;
        });
    }

    // Returns a copy only when accept approves it, then clears its changed flag and unread controls.
    // Calls accept under the mailbox lock; rejection or contention leaves the slot unchanged.
    template <typename Accept> std::optional<Publication> take_if(std::size_t index, Accept accept) noexcept {
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock || index >= slots_.size() || !slots_[index].changed || !accept(slots_[index])) {
            return std::nullopt;
        }

        auto result = slots_[index];
        slots_[index].changed = false;
        slots_[index].present = {};
        return result;
    }

  private:
    // Native physical clients occupy 0–63; the joined client's host occupies 64.
    std::array<Publication, 65> slots_{};
    std::mutex mutex_;
};

} // namespace fc_plugins::direct_transport
