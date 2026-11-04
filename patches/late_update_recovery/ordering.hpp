#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fc_plugins::late_update_recovery {

// One receive pass is capped so sorting and native decoding remain bounded inside ReceiveClient.
inline constexpr std::size_t kMaximumBatch = 512;

// Measure each turn from the first update so a batch that crosses zero still sorts from oldest to newest.
// Queued turns must span less than half the 32-bit turn range for their order to remain unambiguous.
template <class Update, std::size_t Extent> constexpr void order_by_turn(std::span<Update, Extent> updates) noexcept {
    if (updates.empty()) {
        return;
    }

    const auto reference_turn = updates.front().turn;
    std::sort(updates.begin(), updates.end(), [reference_turn](const Update &left, const Update &right) noexcept {
        const auto left_distance = static_cast<std::uint32_t>(left.turn - reference_turn);
        const auto right_distance = static_cast<std::uint32_t>(right.turn - reference_turn);

        return std::bit_cast<std::int32_t>(left_distance) < std::bit_cast<std::int32_t>(right_distance);
    });
}

} // namespace fc_plugins::late_update_recovery
