#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::update_scheduling {

// Publishes full connection identities from control callbacks to the native scheduler.
template <std::size_t Count> class SlotPeers final {
  public:
    // Associates a current native slot with its connection identity.
    void publish(std::size_t slot, std::uint64_t peer) noexcept {
        if (slot < peers_.size()) {
            peers_[slot].store(peer, std::memory_order_release);
        }
    }

    // Removes only the ended identity, preserving a newer occupant of the same slot.
    void withdraw(std::uint64_t peer) noexcept {
        for (auto &slot : peers_) {
            auto expected = peer;
            slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
        }
    }

    // Loads the published identity, or returns zero for an empty or out-of-range slot.
    std::uint64_t current(std::size_t slot) const noexcept {
        return slot < peers_.size() ? peers_[slot].load(std::memory_order_acquire) : 0;
    }

  private:
    std::array<std::atomic_uint64_t, Count> peers_{};
};

} // namespace fc_plugins::update_scheduling
