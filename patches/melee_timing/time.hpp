#pragma once

namespace fc_plugins::melee_timing {

// Converts a relative condition time to the queue's absolute clock without rounding an absolute intermediate.
inline float absolute_time(float clock, float relative_time, float age) noexcept {
    const float relative_offset = relative_time - age;
    return clock + relative_offset;
}

} // namespace fc_plugins::melee_timing
