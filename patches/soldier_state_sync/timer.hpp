#pragma once

#include <cstdint>

namespace fc_plugins::soldier_state_sync {

// Native Read has already replaced these coarse bits; discard the old fraction before adding its midpoint.
inline constexpr std::uint32_t kReceivedCoarseTimerMask = 0x1F000000;

} // namespace fc_plugins::soldier_state_sync
