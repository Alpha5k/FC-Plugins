#pragma once

#include "schema.hpp"

#include <algorithm>
#include <cstdint>

namespace fc_plugins::network_diagnostics {

// Combines only empty passes at the same authority frontier; QPC bounds and RDTSCP work extrema stay distinct.
inline bool append_empty_receive(trace::ClientReceivePassRecord &summary, std::uint64_t &last_entry_qpc,
                                 const trace::ClientReceivePassRecord &next) noexcept {
    if (next.groups != 0 || next.turn_before != next.turn_after || (summary.calls != 0 && summary.turn_after != next.turn_before)) {
        return false;
    }
    if (summary.calls == 0) {
        summary = next;
    } else {
        const auto gap = next.entered_qpc >= last_entry_qpc ? next.entered_qpc - last_entry_qpc : 0;
        summary.maximum_gap = (std::max)(summary.maximum_gap, gap);
        summary.entered_qpc = (std::min)(summary.entered_qpc, next.entered_qpc);
        summary.returned_qpc = (std::max)(summary.returned_qpc, next.returned_qpc);
        summary.last_pass = next.last_pass;
        summary.duration += next.duration;
        summary.maximum_duration = (std::max)(summary.maximum_duration, next.duration);
        summary.calls += next.calls;
    }
    last_entry_qpc = (std::max)(last_entry_qpc, next.entered_qpc);
    return true;
}

} // namespace fc_plugins::network_diagnostics
