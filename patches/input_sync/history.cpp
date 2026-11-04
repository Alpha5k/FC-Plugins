#include "history.hpp"

#include <algorithm>

namespace fc_plugins::input_sync {

void History::synchronize(std::uint32_t turns_per_second, Players players) {
    if (turns_per_second != turns_per_second_) {
        rows_.assign(static_cast<std::size_t>(turns_per_second) * 3, {});
        turns_per_second_ = turns_per_second;
        seed_pending_ = !sampled_;
    }
    if (players != players_) {
        invalidate();
        players_ = players;
        seed_pending_ = !sampled_;
    }
}

void History::invalidate() noexcept {
    for (auto &entry : rows_) {
        entry.valid = false;
    }
}

void History::reset() noexcept {
    invalidate();
    seed_pending_ = true;
    sampled_ = false;
}

bool History::eligible(std::uint32_t turn, std::uint32_t authority) const noexcept {
    return authority != 0 && turn >= authority && turn - authority < rows_.size();
}

void History::store(std::uint32_t turn, const Row &row) noexcept {
    auto &entry = rows_[turn % rows_.size()];
    entry = {.turn = turn, .valid = true, .row = row};
}

void History::capture(std::span<const Row, 32> native_rows, std::uint32_t turn, std::uint32_t authority, std::uint32_t turns_per_second,
                      Players players) {
    synchronize(turns_per_second, players);
    if (!eligible(turn, authority)) {
        return;
    }

    if (seed_pending_) {
        // The current row has already replaced turn-32, so only 31 preceding rows remain in the native ring.
        const auto preceding = std::min(std::uint32_t{31}, turn - authority);
        for (auto previous = turn - preceding; previous < turn; ++previous) {
            store(previous, native_rows[previous % 32]);
        }
        seed_pending_ = false;
    }

    store(turn, native_rows[turn % 32]);
    sampled_ = true;
}

const Row *History::lookup(std::uint32_t turn, std::uint32_t authority, std::uint32_t turns_per_second, Players players) {
    synchronize(turns_per_second, players);
    if (!eligible(turn, authority)) {
        return nullptr;
    }

    const auto &entry = rows_[turn % rows_.size()];
    return entry.valid && entry.turn == turn ? &entry.row : nullptr;
}

} // namespace fc_plugins::input_sync
