#include "handler.hpp"
#include "pipeline_layout.hpp"

#include <bit>

namespace fc_plugins::network_diagnostics {
std::uint32_t Handler::codec_position(std::uint32_t packet, std::uint32_t encoding) noexcept {
    const auto &state = thread_state();
    if (encoding != 0 && state.destination != nullptr && state.destination->packet == packet) {
        return state.destination->bits;
    }
    if (encoding == 0 && state.read_update != nullptr && state.read_update->record.group == packet) {
        return state.read_update->bits;
    }
    return UINT32_MAX;
}

void Handler::plan_velocity(fc::Plan &plan) {
    const auto locations = select_pipeline_layout(plan.profile());
    scalar_raw_memory_ = plan.access(locations.scalar_raw);
    direction_mode_memory_ = plan.access(locations.direction_mode);
    velocity_factor_memory_ = plan.access(locations.velocity_factor);

    // Counting existing primitive calls gives a payload bit position without running another serializer or reader.
    const fc::FunctionLocation<WriteBits> writer = locations.write_bits;
    plan.observe(writer, fc::before([this](void *packet, std::uint32_t value, std::int32_t bits) noexcept {
                     auto &state = thread_state();
                     if (state.destination != nullptr && state.destination->packet == native::pointer_id(packet)) {
                         state.destination->bits += static_cast<std::uint32_t>(bits);
                     }
                     if (state.velocity_active && state.velocity.packet == native::pointer_id(packet) && state.velocity.operation == 1) {
                         observe_velocity_bits(value, static_cast<std::uint32_t>(bits));
                     }
                 }));
    const fc::FunctionLocation<ReadBits> reader = locations.read_bits;
    plan.observe(reader, fc::before([](void *packet, std::uint32_t *, std::int32_t bits) noexcept {
                     auto &state = thread_state();
                     if (state.read_update != nullptr && state.read_update->record.group == native::pointer_id(packet)) {
                         state.read_update->bits += static_cast<std::uint32_t>(bits);
                     }
                 }),
                 fc::after([this](void *packet, std::uint32_t *output, std::int32_t bits) noexcept {
                     const auto &state = thread_state();
                     if (state.velocity_active && state.velocity.packet == native::pointer_id(packet) && state.velocity.operation == 2) {
                         observe_velocity_bits(*output, static_cast<std::uint32_t>(bits));
                     }
                 }));

    const auto begin = [this, &plan](fc::CodeLocation site, bool encoding) {
        plan.observe_instruction(site, fc::before([this, encoding](const fc::CpuContext &context) noexcept {
                                     begin_velocity(context, encoding);
                                 }));
    };
    const auto finish = [this, &plan](fc::CodeLocation site) {
        plan.observe_instruction(site, fc::before([this](const fc::CpuContext &) noexcept {
                                     finish_velocity();
                                 }));
    };
    begin(locations.velocity_write_begin, true);
    finish(locations.velocity_write_end);
    begin(locations.velocity_read_begin, false);
    finish(locations.velocity_read_end);
    plan.observe_instruction(locations.velocity_magnitude_read, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 if (state.velocity_active) {
                                     state.velocity.magnitude = native::read<float>(context.ebp, -4);
                                 }
                             }));
    plan.observe_instruction(locations.velocity_magnitude_write, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 if (state.velocity_active && state.velocity.operation == 1 && state.velocity_primitive == 4) {
                                     state.velocity.magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low));
                                 }
                             }));
}

void Handler::begin_velocity(const fc::CpuContext &context, bool encoding) noexcept {
    auto &state = thread_state();
    const auto object = encoding ? context.esi : context.edi;
    const auto packet = context.ecx;
    const auto native_class = context.eax;
    state.velocity = {.object = object,
                      .packet = packet,
                      .input_turn = static_cast<std::uint32_t>(*host_turn_),
                      .operation = encoding ? 1U : 2U,
                      .maximum = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[2].low))};
    if (encoding && state.destination != nullptr && state.destination->packet == packet) {
        state.velocity.bit_offset = state.destination->bits;
        state.velocity.bit_offset_known = 1;
    } else if (!encoding && state.read_update != nullptr && state.read_update->record.group == packet) {
        state.velocity.bit_offset = state.read_update->bits;
        state.velocity.bit_offset_known = 1;
        state.velocity.input_turn = state.read_update->record.advertised_turn;
    }
    state.velocity_bounds = {.object = object,
                             .packet = packet,
                             .native_class = native_class,
                             .operation = state.velocity.operation,
                             .class_fields = {native::read<float>(native_class, 0x6B4), native::read<float>(native_class, 0x69C),
                                              native::read<float>(native_class, 0x7C0)},
                             .factor = *velocity_factor_,
                             .maximum = state.velocity.maximum,
                             .scalar_raw = *scalar_raw_,
                             .direction_setting = *direction_mode_};
    state.velocity_pointer = context.edx;
    if (encoding) {
        state.velocity.velocity = native::read<std::array<float, 3>>(context.edx);
    }
    state.velocity_primitive = 0;
    state.velocity_active = true;
}

void Handler::observe_velocity_bits(std::uint32_t value, std::uint32_t bits) noexcept {
    auto &state = thread_state();
    auto &record = state.velocity;
    const auto index = state.velocity_primitive++;
    const auto mask = bits == 32 ? UINT32_MAX : (1U << bits) - 1U;
    value &= mask;
    record.bits_after += bits;
    if (index == 0) {
        record.present = value;
    } else if (index <= 3) {
        record.direction_codes[index - 1] = value;
        record.direction_bits[index - 1] = bits;
    } else if (index == 4) {
        record.magnitude_bits = bits;
        record.magnitude_code = value;
        record.raw = bits == 32;
        record.saturated = !record.raw && value == mask;
    }
}

void Handler::finish_velocity() noexcept {
    auto &state = thread_state();
    if (!state.velocity_active) {
        return;
    }
    if (state.velocity.operation == 2) {
        state.velocity.velocity = native::read<std::array<float, 3>>(state.velocity_pointer);
    }
    state.velocity.bits_after += state.velocity.bit_offset;
    write(trace::RecordKind::VelocityBounds, state.velocity_bounds, state.velocity.object);
    write(trace::RecordKind::VelocityCodec, state.velocity, state.velocity.object);
    state.velocity_active = false;
}

} // namespace fc_plugins::network_diagnostics
