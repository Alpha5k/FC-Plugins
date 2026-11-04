#pragma once

#include "schema.hpp"

#include <cstdint>
#include <initializer_list>
#include <cstring>

namespace fc_plugins::sound_diagnostics::trace {

// Emit changes to actual descriptor/controller bindings, while retaining remembered flags only as context.
constexpr bool ambient_binding_changed(const AmbientBindingState &before, const AmbientBindingState &after) noexcept {
    return before.properties != after.properties || before.descriptor_type != after.descriptor_type ||
           before.controller_index != after.controller_index || before.controller_flags != after.controller_flags;
}

// Names only the known property wrappers when the source belongs to the current native property object.
constexpr std::uint32_t property_wrapper_offset(std::uint64_t properties, std::uint64_t source) noexcept {
    if (properties) {
        for (const auto offset : {0x120U, 0x138U, 0x154U, 0x130U}) {
            if (source == properties + offset) {
                return offset;
            }
        }
    }
    return 0;
}

// A reused physical source ends the old gap as replacement, even when its new source reports native end.
constexpr std::uint8_t packet_gap_end_stage(const PacketGapRecord &gap, std::uint64_t generation, std::uint64_t physical,
                                            bool native_end) noexcept {
    if (!generation) {
        return 0;
    }
    if (gap.generation != generation || gap.physical != physical) {
        return 4;
    }
    return native_end ? 5 : 2;
}

// Refusal keys keep exact owner/UI occurrences and callback inputs; moving positions do not split an observed denial.
inline bool same_refusal(RequestRecord left, RequestRecord right) noexcept {
    if ((!left.rejection && (left.header.source != SourceKind::Controlled || left.controller_reason != 2)) ||
        (!right.rejection && (right.header.source != SourceKind::Controlled || right.controller_reason != 2)) || left.starts ||
        right.starts || left.callback || right.callback || left.callback_data || right.callback_data || left.parent || right.parent) {
        return false;
    }
    // Without an attributed producer or UI/owner cause, changing turn/scope counters identify recurring polls, not an action.
    if (!left.subject && !right.subject && !left.weapon && !right.weapon && !left.control_cause && !right.control_cause && !left.ui_event &&
        !right.ui_event && !left.ui_callback && !right.ui_callback) {
        left.operation_scope = right.operation_scope = left.read_scope = right.read_scope = 0;
        left.host_turn = right.host_turn = left.prediction_turn = right.prediction_turn = 0;
        left.update_turn = right.update_turn = left.acknowledged_turn = right.acknowledged_turn = 0;
    }
    left.header.sequence = right.header.sequence = 0;
    left.request = right.request = 0;
    left.entry_ticks = right.entry_ticks = left.end_ticks = right.end_ticks = 0;
    left.position_pointer = right.position_pointer = 0;
    std::memset(left.position, 0, sizeof(left.position));
    std::memset(right.position, 0, sizeof(right.position));
    left.position_valid = right.position_valid = 0;
    // These native rejection gates depend on permission/resource state, never on sampled world position.
    return left == right;
}

// Empty stable callbacks may have successful device queries; failed/recovery/work observations remain exact.
inline bool idle_renderer(const RendererUpdateRecord &r) noexcept {
    return !r.requested_bytes && !r.written_bytes && !r.zero_fill_bytes && !r.consumed_bytes && !r.resets && !r.exhausted_writes &&
           !r.cursor_resets && !r.buffered_before && !r.buffered_after && !r.left_to_play_before && !r.left_to_play_after &&
           r.position_before == r.position_after && r.transport_before == r.transport_after && r.next_lock_before == r.next_lock_after &&
           (!r.position_observed || r.position_result >= 0) && (!r.lock_observed || r.lock_result >= 0) &&
           (!r.unlock_observed || r.unlock_result >= 0);
}

// Compare every observed native state field, excluding only callback timing, owner iteration and run metadata.
inline bool same_idle_renderer(RendererUpdateRecord left, RendererUpdateRecord right) noexcept {
    if (!idle_renderer(left) || !idle_renderer(right)) {
        return false;
    }
    left.header.sequence = right.header.sequence = 0;
    left.start_ticks = right.start_ticks = left.duration_ticks = right.duration_ticks = 0;
    left.owner_iteration = right.owner_iteration = 0;
    left.position_ticks = right.position_ticks = left.request_ticks = right.request_ticks = 0;
    left.lock_ticks = right.lock_ticks = left.unlock_ticks = right.unlock_ticks = 0;
    left.callbacks = left.last_start_ticks = left.last_end_ticks = left.last_owner_iteration = 0;
    left.max_service_gap = left.gap_start_ticks = left.gap_end_ticks = left.max_duration = left.duration_start_ticks = 0;
    left.gap_position_ticks = left.gap_request_ticks = left.gap_lock_ticks = left.gap_unlock_ticks = 0;
    left.duration_position_ticks = left.duration_request_ticks = left.duration_lock_ticks = left.duration_unlock_ticks = 0;
    right.callbacks = right.last_start_ticks = right.last_end_ticks = right.last_owner_iteration = 0;
    right.max_service_gap = right.gap_start_ticks = right.gap_end_ticks = right.max_duration = right.duration_start_ticks = 0;
    right.gap_position_ticks = right.gap_request_ticks = right.gap_lock_ticks = right.gap_unlock_ticks = 0;
    right.duration_position_ticks = right.duration_request_ticks = right.duration_lock_ticks = right.duration_unlock_ticks = 0;
    return left == right;
}

} // namespace fc_plugins::sound_diagnostics::trace
