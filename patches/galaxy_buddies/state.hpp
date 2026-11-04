#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::galaxy_buddies::state {

// Copies callback records for a single consumer. A busy lock or full array drops
// the new record and increments the drop count; neither side waits for the lock.
template <class Record, std::size_t Capacity> class Mailbox final {
  public:
    struct Batch {
        std::array<Record, Capacity> records{};
        std::size_t count{};
        // Records dropped since the last successful consume, including when the lock was busy.
        std::uint32_t dropped{};
    };

    // Copies one callback record or counts a dropped record without blocking its producer.
    void publish(const Record &record) noexcept {
        if (lock_.test_and_set(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (count_ < records_.size()) {
            records_[count_++] = record;
            ready_.store(true, std::memory_order_release);
        } else {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }

        lock_.clear(std::memory_order_release);
    }

    // Drains records and the drop count, or returns an empty batch when the lock is busy.
    Batch consume() noexcept {
        Batch batch;
        if ((!ready_.load(std::memory_order_acquire) && dropped_.load(std::memory_order_acquire) == 0) ||
            lock_.test_and_set(std::memory_order_acquire)) {
            return batch;
        }

        // Copy before clearing readiness; a producer can append after the lock is released.
        batch.count = count_;
        for (std::size_t index = 0; index < count_; ++index) {
            batch.records[index] = records_[index];
        }

        count_ = 0;
        ready_.store(false, std::memory_order_release);
        batch.dropped = dropped_.exchange(0, std::memory_order_acq_rel);
        lock_.clear(std::memory_order_release);
        return batch;
    }

  private:
    std::array<Record, Capacity> records_{};
    std::size_t count_{};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::atomic_bool ready_{};
    std::atomic_uint32_t dropped_{};
};

// Reserving and Completing hide partial writes; the pending and terminal states
// publish their user ID and reason to the next reader with release ordering.
enum class PendingRequestState : std::uint32_t {
    Empty,
    Reserving,
    Completing,
    Sending,
    SendSucceeded,
    SendFailed,
    Canceling,
    CancelSucceeded,
    CancelFailed,
};

enum class ReserveResult : std::uint8_t {
    Reserved,
    Duplicate,
    Full,
};

struct PendingCompletion {
    // Full Galaxy account ID, independent of native player slots or Lua row numbers.
    std::uint64_t user{};
    // Galaxy's numeric failure reason; successful completions use zero.
    std::uint32_t detail{};
    PendingRequestState state{PendingRequestState::Empty};
};

// Reserves callback completion storage before Galaxy starts a send or cancel.
// The owner serializes reserve, release and consume; callbacks may complete concurrently.
template <std::size_t Capacity> class PendingRequests final {
  public:
    struct Batch {
        std::array<PendingCompletion, Capacity> completions{};
        std::size_t count{};
    };

    // Reserves one user before submission; either pending operation blocks another
    // reservation for that user until its completion is consumed or it is released.
    ReserveResult reserve(std::uint64_t user, PendingRequestState state) noexcept {
        for (const auto &slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) != PendingRequestState::Empty &&
                slot.user.load(std::memory_order_relaxed) == user) {
                return ReserveResult::Duplicate;
            }
        }

        // Publish the pending state only after its account ID and reason are initialized.
        for (auto &slot : slots_) {
            auto expected = PendingRequestState::Empty;
            if (!slot.state.compare_exchange_strong(expected, PendingRequestState::Reserving, std::memory_order_acq_rel)) {
                continue;
            }

            slot.user.store(user, std::memory_order_relaxed);
            slot.detail.store(0, std::memory_order_relaxed);
            slot.state.store(state, std::memory_order_release);
            return ReserveResult::Reserved;
        }

        return ReserveResult::Full;
    }

    // Releases the matching request when Galaxy immediately rejects it.
    void release(std::uint64_t user, PendingRequestState state) noexcept {
        for (auto &slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) != state || slot.user.load(std::memory_order_relaxed) != user) {
                continue;
            }

            slot.user.store(0, std::memory_order_relaxed);
            slot.state.store(PendingRequestState::Empty, std::memory_order_release);
            return;
        }
    }

    // Publishes one matching completion; repeated or unrelated callbacks cannot overwrite it.
    bool complete(std::uint64_t user, PendingRequestState pending, PendingRequestState completed, std::uint32_t detail = 0) noexcept {
        for (auto &slot : slots_) {
            // Acquire the reservation before reading the user published with it.
            if (slot.state.load(std::memory_order_acquire) != pending || slot.user.load(std::memory_order_relaxed) != user) {
                continue;
            }

            auto expected = pending;
            // Claim Completing so another callback cannot replace this terminal result.
            if (!slot.state.compare_exchange_strong(expected, PendingRequestState::Completing, std::memory_order_acq_rel)) {
                continue;
            }

            slot.detail.store(detail, std::memory_order_relaxed);
            slot.state.store(completed, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Copies terminal results and releases their slots; pending calls remain reserved.
    Batch consume() noexcept {
        Batch batch;
        for (auto &slot : slots_) {
            const auto current = slot.state.load(std::memory_order_acquire);
            if (current != PendingRequestState::SendSucceeded && current != PendingRequestState::SendFailed &&
                current != PendingRequestState::CancelSucceeded && current != PendingRequestState::CancelFailed) {
                continue;
            }

            batch.completions[batch.count++] = {
                .user = slot.user.load(std::memory_order_relaxed),
                .detail = slot.detail.load(std::memory_order_relaxed),
                .state = current,
            };

            // Empty is published last so reserve cannot reuse a partially cleared slot.
            slot.detail.store(0, std::memory_order_relaxed);
            slot.user.store(0, std::memory_order_relaxed);
            slot.state.store(PendingRequestState::Empty, std::memory_order_release);
        }

        return batch;
    }

  private:
    struct Slot {
        std::atomic_uint64_t user{};
        std::atomic_uint32_t detail{};
        std::atomic<PendingRequestState> state{};
    };

    std::array<Slot, Capacity> slots_{};
};

} // namespace fc_plugins::galaxy_buddies::state
