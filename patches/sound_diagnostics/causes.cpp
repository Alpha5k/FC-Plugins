#include "handler.hpp"
#include "aggregation.hpp"

#include <Windows.h>
#include <bit>
#include <cstring>
#include <utility>

namespace fc_plugins::sound_diagnostics {
namespace {

// Every observation restores the native thread error after copying its borrowed scalar operands.
struct ErrorGuard {
    DWORD error{GetLastError()};

    ~ErrorGuard() {
        SetLastError(error);
    }
};

// Copies a qualified live native field without retaining an object or invoking its methods.
template <class T> T field(const void *object, std::ptrdiff_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::byte *>(object) + offset, sizeof(value));
    return value;
}

// Converts an observed x86 address into a borrowed key.
void *pointer(std::uint64_t address) noexcept {
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(address));
}

// Copies only the registered ambient wrapper and its independent controller state.
trace::AmbientBindingState binding_state(const void *weapon) noexcept {
    return {.properties = field<std::uint32_t>(weapon, 0xD8),
            .controller_index = field<std::uint16_t>(weapon, 0xCC),
            .descriptor_type = field<std::uint8_t>(weapon, 0xDC),
            .controller_flags = field<std::uint8_t>(weapon, 0xCE),
            .state_flags = field<std::uint8_t>(weapon, 0x179),
            .sound_masks = field<std::uint8_t>(weapon, 0x17D)};
}

// Copies the native wrapper result, including deferred hashes, without resolving another resource.
void descriptor(trace::ProducerRecord &record, void *wrapper) noexcept {
    record.wrapper = reinterpret_cast<std::uintptr_t>(wrapper);
    record.descriptor = field<std::uint32_t>(wrapper, 0);
    record.descriptor_type = field<std::uint8_t>(wrapper, 4);
    if (record.descriptor && record.descriptor_type) {
        record.definition_id = record.descriptor_type == 4
                                   ? static_cast<std::uint32_t>(record.descriptor)
                                   : field<std::uint32_t>(pointer(record.descriptor), record.descriptor_type == 3 ? 0x10 : 4);
    } else {
        record.definition_id = 0;
    }
}

// Copies state/clock operands from the qualified producer object before or after its native decision.
void producer_state(trace::ProducerRecord &record, void *object) noexcept {
    if (record.producer == trace::NativeProducer::GroundReturn) {
        record.current_state = field<std::int32_t>(object, 0x744);
        record.previous_state = field<std::int32_t>(object, 0x748);
        record.current_clock = field<float>(object, 0x74C);
        record.native_flags = field<std::uint32_t>(object, 0x1FC);
    } else if (record.producer == trace::NativeProducer::Toggle) {
        record.current_state = field<std::uint8_t>(object, 0x155);
        record.current_clock = field<float>(object, 0x15C);
    }
}

} // namespace

thread_local Handler::ProducerInvocation *Handler::producer_{};
thread_local Handler::BindingInvocation *Handler::binding_{};

// Declares only qualified writers and selected native sound producers.
void Handler::plan_causes(fc::Plan &plan, const Layout &layout) {
    plan_footsteps(plan, layout);
    plan.observe<BindingInvocation>(
        layout.binding_assign,
        fc::before([this, rva = layout.binding_assign.rva](const fc::NativeContext &native, void *wrapper, void *source,
                                                           BindingInvocation &invocation) noexcept {
            begin_binding(wrapper, source, trace::BindingOperation::Assign, native.return_address(), rva, invocation);
        }),
        fc::after([this](void *wrapper, void *, void *, const BindingInvocation &invocation) noexcept {
            end_binding(wrapper, invocation);
        }));
    plan.observe<ProducerInvocation>(
        layout.effect_emit,
        fc::before([this, rva = layout.effect_emit.rva](const fc::NativeContext &native, void *effect, std::uint32_t position,
                                                        std::uint32_t owner, std::uint32_t third, std::uint32_t fourth,
                                                        ProducerInvocation &invocation) noexcept {
            begin_producer(effect, trace::NativeProducer::Effect, native.return_address(), rva, {position, owner, third, fourth},
                           invocation);
        }),
        fc::after([this](void *effect, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                         const ProducerInvocation &invocation) noexcept {
            end_producer(effect, 0, invocation);
        }));
    plan.observe<ProducerInvocation>(layout.ground_return,
                                     fc::before([this, rva = layout.ground_return.rva](const fc::NativeContext &native, void *base,
                                                                                       ProducerInvocation &invocation) noexcept {
                                         begin_producer(static_cast<std::byte *>(base) - 0x240, trace::NativeProducer::GroundReturn,
                                                        native.return_address(), rva, {}, invocation);
                                     }),
                                     fc::after([this](void *base, std::uint32_t result, const ProducerInvocation &invocation) noexcept {
                                         end_producer(static_cast<std::byte *>(base) - 0x240, result, invocation);
                                     }));
    for (const auto &[location, kind] :
         {std::pair{layout.toggle, trace::NativeProducer::Toggle}, std::pair{layout.fire_audio, trace::NativeProducer::FireAudio}}) {
        plan.observe<ProducerInvocation>(location,
                                         fc::before([this, rva = location.rva, kind](const fc::NativeContext &native, void *object,
                                                                                     bool input, ProducerInvocation &invocation) noexcept {
                                             begin_producer(object, kind, native.return_address(), rva,
                                                            {static_cast<std::uint32_t>(input), 0, 0, 0}, invocation);
                                         }),
                                         fc::after([this](void *object, bool, const ProducerInvocation &invocation) noexcept {
                                             end_producer(object, 0, invocation);
                                         }));
    }
    plan.observe_instruction(layout.ground_commit, fc::before([this, rva = layout.ground_commit.rva](const fc::CpuContext &cpu) noexcept {
                                 producer_instruction(cpu, trace::NativeProducer::GroundReturn, 4, 0, rva);
                             }));
    for (std::size_t index = 0; index < 2; ++index) {
        plan.observe_instruction(layout.ground_sounds[index],
                                 fc::before([this, index, rva = layout.ground_sounds[index].rva](const fc::CpuContext &cpu) noexcept {
                                     producer_instruction(cpu, trace::NativeProducer::GroundReturn, 3, index == 0 ? 0x70 : 0x50, rva);
                                 }));
        plan.observe_instruction(layout.toggle_sounds[index],
                                 fc::before([this, index, rva = layout.toggle_sounds[index].rva](const fc::CpuContext &cpu) noexcept {
                                     producer_instruction(cpu, trace::NativeProducer::Toggle, 3, index == 0 ? 0x26C : 0x274, rva);
                                 }));
    }
}

// Filters the destination through the existing Hero registry before borrowing native fields.
void Handler::begin_binding(void *wrapper, void *source, trace::BindingOperation operation, std::uintptr_t caller, std::uint32_t rva,
                            BindingInvocation &invocation) noexcept {
    const ErrorGuard guard;
    invocation.previous = binding_;
    if (!trace_.enabled() || !hero_ready_.load(std::memory_order_acquire)) {
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(wrapper);
    auto *weapon = pointer(address - 0xD8U);
    // This arithmetic only supplies a registry key; weapon_scope rejects unbound resources without dereferencing it.
    const auto scope = hero_.weapon_scope(weapon);
    if (!scope.subject || scope.weapon != reinterpret_cast<std::uintptr_t>(weapon)) {
        return;
    }
    invocation.observed = true;
    binding_ = &invocation;
    auto &record = invocation.record;
    record.site = image_base_ + rva;
    record.caller = caller;
    record.control_cause = scopes_.control_cause;
    record.subject = scope.subject;
    record.generation = scope.generation;
    record.operation_scope = scope.operation_scope;
    record.read_scope = scope.read_scope;
    record.wrapper = address;
    record.property_object = field<std::uint32_t>(weapon, 0x64);
    record.source = reinterpret_cast<std::uintptr_t>(source);
    record.before = binding_state(weapon);
    record.operation = operation;
    record.host_turn = scope.host_turn;
    record.prediction_turn = scope.prediction_turn;
    record.update_turn = scope.update_turn;
    record.acknowledged_turn = scope.acknowledged_turn;
    record.phase = scope.phase;
    if (source) {
        record.source_before_pointer = field<std::uint32_t>(source, 0);
        record.source_before_type = field<std::uint8_t>(source, 4);
        record.source_offset = trace::property_wrapper_offset(record.property_object, record.source);
    }
}

// Publishes every completed write, including empty-to-empty assignments, after lifetime validation.
void Handler::end_binding(void *, const BindingInvocation &invocation) noexcept {
    const ErrorGuard guard;
    binding_ = invocation.previous;
    if (!invocation.observed || !trace_.enabled()) {
        return;
    }
    auto record = invocation.record;
    auto *weapon = pointer(record.wrapper - 0xD8U);
    const auto scope = hero_.weapon_scope(weapon);
    if (scope.subject != record.subject || scope.generation != record.generation || scope.weapon != record.wrapper - 0xD8U) {
        return;
    }
    record.header = header(weapon, trace::RecordKind::BindingMutation);
    record.header.source = trace::SourceKind::Controlled;
    record.after = binding_state(weapon);
    if (record.source) {
        record.source_after_pointer = field<std::uint32_t>(pointer(record.source), 0);
        record.source_after_type = field<std::uint8_t>(pointer(record.source), 4);
    }
    write(record);
}

// Assigns an existing cause ID to this native invocation and captures its input operands.
void Handler::begin_producer(void *object, trace::NativeProducer producer, std::uintptr_t caller, std::uint32_t rva,
                             std::array<std::uint32_t, 4> arguments, ProducerInvocation &invocation) noexcept {
    const ErrorGuard guard;
    invocation.previous = producer_;
    invocation.previous_cause = scopes_.control_cause;
    if (!trace_.enabled()) {
        return;
    }
    auto &record = invocation.record;
    record.producer = producer;
    record.caller = caller;
    record.site = image_base_ + rva;
    record.parent = scopes_.control_cause;
    std::memcpy(record.arguments, arguments.data(), sizeof(record.arguments));
    if (producer == trace::NativeProducer::Toggle) {
        record.selected_index = field<std::int32_t>(object, 0x12C);
        record.weapon = field<std::uint32_t>(object, 0x10C + static_cast<std::ptrdiff_t>(record.selected_index) * 4);
    } else if (producer == trace::NativeProducer::GroundReturn) {
        const auto selected = field<std::uint8_t>(object, 0x742);
        record.selected_index = static_cast<std::int8_t>(static_cast<std::uint8_t>(selected << 4)) >> 4;
        if (record.selected_index >= 0) {
            record.weapon = field<std::uint32_t>(object, 0x720 + record.selected_index * 4);
        }
        std::memcpy(record.position, static_cast<std::byte *>(object) + 0x120, sizeof(record.position));
        std::memcpy(record.velocity, static_cast<std::byte *>(object) + 0x4DC, sizeof(record.velocity));
    } else if (producer == trace::NativeProducer::FireAudio) {
        record.weapon = reinterpret_cast<std::uintptr_t>(object);
    } else {
        descriptor(record, static_cast<std::byte *>(object) + 0xC8);
        if (arguments[0]) {
            std::memcpy(record.position, pointer(arguments[0]), sizeof(record.position));
        }
    }
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = record.weapon ? hero_.weapon_scope(pointer(record.weapon)) : hero_.current_scope();
        if (producer == trace::NativeProducer::FireAudio && !scope.subject) {
            return;
        }
        record.subject = scope.subject;
        record.generation = scope.generation;
        record.operation_scope = scope.operation_scope;
        record.read_scope = scope.read_scope;
        record.host_turn = scope.host_turn;
        record.prediction_turn = scope.prediction_turn;
        record.update_turn = scope.update_turn;
        record.acknowledged_turn = scope.acknowledged_turn;
        record.phase = scope.phase;
    } else if (producer == trace::NativeProducer::FireAudio) {
        return;
    }
    producer_state(record, object);
    record.prior_state = record.current_state;
    record.prior_clock = record.current_clock;
    record.cause = next_control_cause_.fetch_add(1, std::memory_order_relaxed);
    record.header = header(object, trace::RecordKind::NativeProducer);
    record.stage = 1;
    attach_ui(record.ui_event, record.ui_callback);
    invocation.observed = true;
    producer_ = &invocation;
    scopes_.control_cause = record.cause;
    write(record);
}

// Captures native results before restoring the enclosing producer and cause scope.
void Handler::end_producer(void *object, std::uint32_t result, const ProducerInvocation &invocation) noexcept {
    const ErrorGuard guard;
    if (invocation.observed && trace_.enabled()) {
        auto record = invocation.record;
        producer_state(record, object);
        record.header = header(object, trace::RecordKind::NativeProducer);
        record.stage = 2;
        record.result = result;
        write(record);
    }
    producer_ = invocation.previous;
    scopes_.control_cause = invocation.previous_cause;
}

// Records an executed gate or sound branch under its actual producer invocation.
void Handler::producer_instruction(const fc::CpuContext &cpu, trace::NativeProducer producer, std::uint8_t stage, std::uint16_t branch,
                                   std::uint32_t rva) noexcept {
    const ErrorGuard guard;
    if (!producer_ || !producer_->observed || producer_->record.producer != producer || !trace_.enabled()) {
        return;
    }
    auto &retained = producer_->record;
    auto *object = pointer(retained.header.object);
    const auto actual_object = producer == trace::NativeProducer::GroundReturn ? cpu.edi - 0x240U : cpu.esi;
    if (actual_object != retained.header.object) {
        return;
    }
    producer_state(retained, object);
    if (stage == 4) {
        retained.commit_observed = true;
        retained.commit_result = static_cast<std::uint8_t>(cpu.eax);
    }
    auto record = retained;
    record.header = header(object, trace::RecordKind::NativeProducer);
    record.site = image_base_ + rva;
    record.stage = stage;
    record.branch = branch;
    if (stage == 3) {
        // Copy the operand actually selected at the call, rather than predicting a branch from previous state.
        record.property_object = producer == trace::NativeProducer::Toggle ? cpu.ecx : cpu.ecx - branch;
        descriptor(record, pointer(producer == trace::NativeProducer::Toggle ? cpu.ecx + branch : cpu.ecx));
        if (producer == trace::NativeProducer::GroundReturn) {
            record.weapon = cpu.esi;
        } else {
            record.selected_index = field<std::int32_t>(object, 0x12C);
            record.weapon = field<std::uint32_t>(object, 0x10C + static_cast<std::ptrdiff_t>(record.selected_index) * 4);
        }
        if (hero_ready_.load(std::memory_order_acquire)) {
            const auto identity = hero_.weapon_scope(pointer(record.weapon));
            record.subject = identity.subject;
            record.generation = identity.generation;
        }
    }
    write(record);
}

} // namespace fc_plugins::sound_diagnostics
