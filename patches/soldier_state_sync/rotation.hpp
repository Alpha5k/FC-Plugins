#pragma once

#include <cmath>

namespace fc_plugins::soldier_state_sync {

// Removes table-interpolation scale from the native finite, nonzero sine/cosine pair, retaining its direction.
inline void normalize_rotation(float &sine, float &cosine) noexcept {
    const double length = std::sqrt(static_cast<double>(sine) * sine + static_cast<double>(cosine) * cosine);
    sine = static_cast<float>(sine / length);
    cosine = static_cast<float>(cosine / length);
}

} // namespace fc_plugins::soldier_state_sync
