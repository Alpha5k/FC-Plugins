#include "handler.hpp"
#include "api.hpp"
#include "aggregation.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>

namespace fc_plugins::sound_diagnostics {
namespace {

// Native HRESULTs remain in CPU context; this guard preserves OS last error across every diagnostic callback.
struct ErrorGuard {
    DWORD error{GetLastError()};

    // Restore the entering thread's error after all diagnostic work in this scope.
    ~ErrorGuard() {
        SetLastError(error);
    }
};

// Copies a field from the live object/stack supplied by the qualified native boundary.
template <class T> T field(const void *object, std::ptrdiff_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::byte *>(object) + offset, sizeof(value));
    return value;
}

// Converts a saved native address without dereferencing it.
void *pointer(std::uint32_t address) noexcept {
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(address));
}

// Retail getters at Steam 0x5383A0/GOG 0x539110 return [wrapper] directly; Bind copies +0 and the type at +4.
// UpdateEffects supplies wrapper=weapon+0xD8 and controller=weapon+0xCC; Stop clears the controller's +2 flags.
trace::AmbientBindingState ambient_binding(const void *weapon) noexcept {
    return {.properties = field<std::uint32_t>(weapon, 0xD8),
            .controller_index = field<std::uint16_t>(weapon, 0xCC),
            .descriptor_type = field<std::uint8_t>(weapon, 0xDC),
            .controller_flags = field<std::uint8_t>(weapon, 0xCE),
            .state_flags = field<std::uint8_t>(weapon, 0x179),
            .sound_masks = field<std::uint8_t>(weapon, 0x17D)};
}

// Returns the native service observation time in QPC ticks.
std::uint64_t ticks() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

// Copies a native NUL-terminated name and reports truncation without allocating or calling Lua.
template <std::size_t Size> bool copy_name(char (&destination)[Size], const char *source) noexcept {
    if (!source) {
        return false;
    }
    for (std::size_t index = 0; index < Size; ++index) {
        if (index + 1 == Size) {
            destination[index] = 0;
            return source[index] != 0;
        }
        destination[index] = source[index];
        if (!source[index]) {
            return false;
        }
    }
    return false;
}

enum Boundary : std::uint8_t {
    Position,
    Consumed,
    CursorReset,
    Requested,
    Lock,
    Unlock,
    Exhausted,
    ZeroFill
};

} // namespace

thread_local Handler::RendererInvocation *Handler::renderer_{};
thread_local std::array<Handler::RequestRun, 64> Handler::request_runs_{};
thread_local std::array<Handler::RendererRun, 128> Handler::renderer_runs_{};
thread_local std::array<Handler::RouteRun, 128> Handler::route_runs_{};
thread_local Handler::RendererInvocation Handler::renderer_storage_{};
thread_local trace::OwnerRecord Handler::owner_{};
thread_local trace::OwnerProgressRecord Handler::owner_progress_{};
thread_local Handler::NativeScopes Handler::scopes_{};
thread_local void *Handler::device_play_{};
thread_local std::uintptr_t Handler::device_play_caller_{};
thread_local bool Handler::flush_locked_{};
thread_local std::array<Handler::PacketGap, 128> Handler::packet_gaps_{};
thread_local std::uint32_t Handler::pending_packet_gaps_{};

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    plan_causes(plan, *layout);
    image_anchor_rva_ = layout->ui_dispatch.rva;
    image_anchor_ = plan.access<const std::byte>(image_anchor_rva_);
    plan.observe<AmbientInvocation>(layout->melee_read,
                                    fc::before([this, rva = layout->melee_read.rva](const fc::NativeContext &native, void *weapon, void *,
                                                                                    AmbientInvocation &invocation) noexcept {
                                        begin_ambient_binding(weapon, native.return_address(), rva, trace::AmbientBoundary::Read,
                                                              invocation);
                                    }),
                                    fc::after([this](void *weapon, void *, const AmbientInvocation &invocation) noexcept {
                                        end_ambient_binding(weapon, invocation);
                                    }));
    plan.observe<AmbientInvocation>(layout->melee_controls,
                                    fc::before([this, rva = layout->melee_controls.rva](const fc::NativeContext &native, void *weapon,
                                                                                        AmbientInvocation &invocation) noexcept {
                                        begin_ambient_binding(weapon, native.return_address(), rva, trace::AmbientBoundary::Controls,
                                                              invocation);
                                    }),
                                    fc::after([this](void *weapon, bool, const AmbientInvocation &invocation) noexcept {
                                        end_ambient_binding(weapon, invocation);
                                    }));
    plan.observe<std::uint64_t>(layout->sound_reset, fc::before([this](const fc::NativeContext &native, std::uint64_t &previous) noexcept {
                                    previous = scopes_.control_cause;
                                    begin_control_cause(trace::RecordKind::SoundReset, native.return_address());
                                }),
                                fc::after([](const std::uint64_t &previous) noexcept {
                                    scopes_.control_cause = previous;
                                }));
    plan.observe_call<std::uint64_t>(layout->stop_all,
                                     fc::before([this](const fc::NativeContext &native, std::uint64_t &previous) noexcept {
                                         previous = scopes_.control_cause;
                                         begin_control_cause(trace::RecordKind::StopAll, native.return_address());
                                     }),
                                     fc::after([](const std::uint64_t &previous) noexcept {
                                         scopes_.control_cause = previous;
                                     }));
    for (std::size_t index = 0; index < layout->health_decisions.size(); ++index) {
        plan.observe_instruction(layout->health_decisions[index],
                                 fc::before([this, index, rva = layout->health_decisions[index].rva](const fc::CpuContext &cpu) noexcept {
                                     health_decision(cpu, index == 0, rva);
                                 }));
    }
    for (const auto &site : layout->ui_queued) {
        plan.observe_instruction(site, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                     queue_ui_event(cpu);
                                 }));
    }
    plan.observe<std::array<std::uint64_t, 2>>(
        layout->ui_dispatch, fc::before([this](const fc::NativeContext &native, void *, std::array<std::uint64_t, 2> &previous) noexcept {
            previous = {scopes_.ui_dispatch, scopes_.ui_dispatch_caller};
            scopes_.ui_dispatch_caller = native.return_address();
            scopes_.ui_dispatch = next_ui_dispatch_.fetch_add(1, std::memory_order_relaxed);
        }),
        fc::after([](void *, const std::array<std::uint64_t, 2> &previous) noexcept {
            scopes_.ui_dispatch = previous[0];
            scopes_.ui_dispatch_caller = previous[1];
        }));
    plan.observe_instruction(layout->ui_event, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 begin_ui_event(cpu);
                             }));
    plan.observe_instruction(layout->ui_event_end, fc::before([this](const fc::CpuContext &) noexcept {
                                 end_ui_event();
                             }));
    for (std::size_t index = 0; index < layout->ui_callbacks.size(); ++index) {
        plan.observe_instruction(layout->ui_callbacks[index],
                                 fc::before([this, index, rva = layout->ui_callbacks[index].rva](const fc::CpuContext &cpu) noexcept {
                                     begin_ui_callback(cpu, static_cast<std::uint32_t>(index), rva);
                                 }));
        plan.observe_instruction(layout->ui_callback_ends[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     end_ui_callback(cpu, static_cast<std::uint32_t>(index));
                                 }));
    }
    plan.observe(layout->packet, fc::after([this](void *resampler, std::uint32_t, void *result) noexcept {
                     packet(resampler, result);
                 }));
    plan.observe_instruction(layout->source_end, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 source_end(cpu);
                             }));
    for (std::size_t index = 0; index < layout->voice_ends.size(); ++index) {
        plan.observe_instruction(
            layout->voice_ends[index], fc::before([this, index, rva = layout->voice_ends[index].rva](const fc::CpuContext &cpu) noexcept {
                voice_decision(cpu, index == 0 ? trace::VoiceDecision::VirtualSampleEnd : trace::VoiceDecision::DownstreamStopped, rva);
            }));
    }

    for (std::size_t index = 0; index < layout->saber_decisions.size(); ++index) {
        plan.observe_instruction(layout->saber_decisions[index],
                                 fc::before([this, index, rva = layout->saber_decisions[index].rva](const fc::CpuContext &cpu) noexcept {
                                     saber_decision(cpu, static_cast<trace::SaberDecision>(index + 1), rva);
                                 }));
    }
    plan.observe_instruction(layout->physical_route, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 const ErrorGuard guard;
                                 if (!trace_.enabled()) {
                                     return;
                                 }
                                 auto *physical = pointer(cpu.edi);
                                 auto *node = static_cast<std::byte *>(physical) + 0xE8;
                                 auto *output = pointer(field<std::uint32_t>(node, 0xF8));
                                 trace::PhysicalRouteRecord record{.header = header(physical, trace::RecordKind::PhysicalRoute),
                                                                   .stream_node = reinterpret_cast<std::uintptr_t>(node),
                                                                   .renderer = reinterpret_cast<std::uintptr_t>(output),
                                                                   .mode = cpu.eax};
                                 if (output) {
                                     record.device = field<std::uint32_t>(output, 0xF8);
                                     record.input_node = field<std::uint32_t>(output, 0x20);
                                     record.channel = field<std::uint32_t>(output, 0x24);
                                 }
                                 write(record);
                             }));
    for (const auto &site : {layout->source_route_software, layout->source_route_hardware}) {
        plan.observe_instruction(site, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                     source_route(cpu);
                                 }));
    }
    plan.observe_instruction(layout->mixer_created, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 stream(pointer(cpu.esi), trace::RecordKind::MixerCreated, false);
                             }));
    plan.observe_instruction(layout->mixer_closing, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 stream(pointer(cpu.esi), trace::RecordKind::MixerClosing, false);
                             }));
    plan.observe_instruction(layout->renderer_created, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 stream(pointer(cpu.edi), trace::RecordKind::RendererCreated, true);
                             }));
    plan.observe_instruction(layout->renderer_closing, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 stream(pointer(cpu.edi), trace::RecordKind::RendererClosing, true);
                             }));
    plan.observe(layout->renderer_format, fc::after([this](void *object, std::uint32_t, std::uint32_t, std::uint32_t, bool, float, bool,
                                                           void *, std::uint32_t, bool, bool accepted) noexcept {
                     stream(object, trace::RecordKind::RendererFormat, true, accepted);
                 }));
    plan.observe(layout->renderer_flush, fc::before([](void *) noexcept {
                     flush_locked_ = false;
                 }),
                 fc::after([this](void *object) noexcept {
                     const ErrorGuard guard;
                     if (renderer_ && renderer_->record.header.object == reinterpret_cast<std::uintptr_t>(object)) {
                         ++renderer_->record.resets;
                     }
                     stream(object, trace::RecordKind::RendererReset, true);
                 }));

    plan.observe(layout->renderer_update, fc::before([this](void *object) noexcept {
                     begin_renderer(object, renderer_storage_);
                 }),
                 fc::after([this](void *) noexcept {
                     end_renderer(renderer_storage_);
                 }));
    plan.observe(layout->renderer_write,
                 fc::after([this](void *object, void *, std::uint32_t, std::int32_t, std::uint32_t written) noexcept {
                     const ErrorGuard guard;
                     if (renderer_ && renderer_->record.header.object == reinterpret_cast<std::uintptr_t>(object)) {
                         renderer_->record.written_bytes += written;
                     }
                 }));

    const std::array sites{layout->position_result, layout->consumed,      layout->cursor_reset,    layout->requested,
                           layout->lock_result,     layout->unlock_result, layout->queue_exhausted, layout->zero_fill};
    for (std::uint8_t index = 0; index < sites.size(); ++index) {
        plan.observe_instruction(sites[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     renderer_instruction(cpu, index);
                                 }));
    }

    // Instruction operands are four bytes but need not have uint32 alignment.
    plan.check(layout->sleep_operand.rva, 4, layout->sleep_operand.evidence);
    plan.check(layout->flush_lock_operand.rva, 4, layout->flush_lock_operand.evidence);
    const std::array owner_sites{layout->owner_wait_before, layout->owner_wait_after, layout->owner_sleep_before,
                                 layout->owner_sleep_after};
    for (std::uint8_t index = 0; index < owner_sites.size(); ++index) {
        plan.observe_instruction(owner_sites[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     owner_instruction(cpu, index);
                                 }));
    }

    plan.observe(
        layout->game_play,
        fc::before([this](const fc::NativeContext &native, void *object, void *position, void *source, bool first, bool second) noexcept {
            begin_request(object, pointer(field<std::uint32_t>(object, 0)), position, trace::SourceKind::Game, nullptr, nullptr, source,
                          native.return_address(),
                          static_cast<std::uint8_t>(static_cast<std::uint8_t>(first) | (static_cast<std::uint8_t>(second) << 1)));
        }),
        fc::after([this](void *, void *, void *, bool, bool) noexcept {
            end_request();
        }));
    plan.observe(
        layout->sound_play,
        fc::before([this](const fc::NativeContext &native, void *source, void *descriptor, void *callback, void *data, bool flag) noexcept {
            auto *position = source ? pointer(field<std::uint32_t>(source, 0x14)) : nullptr;
            begin_request(descriptor, descriptor, position, trace::SourceKind::Ordinary, callback, data, source, native.return_address(),
                          flag);
        }),
        fc::after([this](void *, void *, void *, void *, bool, void *) noexcept {
            end_request();
        }));
    plan.observe(layout->setup_voice, fc::after([this](void *, void *voice, void *, void *) noexcept {
                     bind_voice(voice);
                 }));
    plan.observe(layout->voice_start, fc::before([this](const fc::NativeContext &native, void *voice, bool restart) noexcept {
                     begin_voice(voice, trace::RecordKind::VoiceStart, restart, native.return_address());
                 }),
                 fc::after([this](void *voice, bool) noexcept {
                     end_voice(voice, trace::RecordKind::VoiceStart);
                 }));
    plan.observe(layout->voice_stop, fc::before([this](const fc::NativeContext &native, void *voice) noexcept {
                     begin_voice(voice, trace::RecordKind::VoiceStop, false, native.return_address());
                 }),
                 fc::after([this](void *voice) noexcept {
                     end_voice(voice, trace::RecordKind::VoiceStop);
                 }));
    plan.observe(layout->voice_inactive, fc::before([this](const fc::NativeContext &native, void *, void *voice) noexcept {
                     const ErrorGuard guard;
                     if (trace_.enabled()) {
                         route_epoch_.fetch_add(1, std::memory_order_release);
                         flush_runs();
                         auto record = voice_record(voice, trace::RecordKind::VoiceInactive);
                         record.caller = native.return_address();
                         write(record);
                     }
                 }));
    const std::array gates{
        layout->game_permission,      layout->ordinary_property,        layout->ordinary_sample,      layout->ordinary_gain,
        layout->ordinary_voice,       layout->game_permission_resolved, layout->stream_slot_gate,     layout->stream_bus_gate,
        layout->stream_bus_gain_gate, layout->stream_sample_gate,       layout->stream_pitch_gate,    layout->stream_property_gate,
        layout->stream_gain_gate,     layout->stream_queue_gate,        layout->stream_priority_gate, layout->layered_queue_gate};
    for (std::uint8_t index = 0; index < gates.size(); ++index) {
        plan.observe_instruction(gates[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     request_gate(cpu, index);
                                 }));
    }
    plan.observe_instruction(layout->ordinary_admission, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 const ErrorGuard guard;
                                 if (!trace_.enabled()) {
                                     return;
                                 }
                                 trace::AdmissionRecord record{.header = header(pointer(cpu.ebx), trace::RecordKind::VoiceAdmission),
                                                               .sample = field<std::uint32_t>(pointer(cpu.ebp), -0x20),
                                                               .voice = cpu.ebx,
                                                               .free_voice = field<std::uint8_t>(pointer(cpu.ebp), -2)};
                                 record.header.source = trace::SourceKind::Ordinary;
                                 if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
                                     record.request = scopes_.requests[scopes_.request_depth - 1].request;
                                 }
                                 write(record);
                             }));

    plan.observe(layout->controller_play,
                 fc::before([this](const fc::NativeContext &native, void *controller, void *sound, void *position, void *source,
                                   bool flag) noexcept {
                     begin_request(controller, sound, position, trace::SourceKind::Controlled, nullptr, nullptr, source,
                                   native.return_address(), flag);
                     state(controller, trace::RecordKind::Controller, 1);
                 }),
                 fc::after([this](void *controller, void *, void *, void *, bool) noexcept {
                     state(controller, trace::RecordKind::Controller, 7);
                     end_request();
                 }));
    plan.observe(layout->controller_stop, fc::before([this](const fc::NativeContext &native, void *controller, bool stop) noexcept {
                     begin_request(controller, nullptr, nullptr, trace::SourceKind::Controlled, nullptr, nullptr, nullptr,
                                   native.return_address());
                     state(controller, trace::RecordKind::Controller, 2, stop);
                 }),
                 fc::after([this](void *controller, bool stop) noexcept {
                     state(controller, trace::RecordKind::Controller, 8, stop);
                     end_request();
                 }));
    plan.observe(layout->layered_play, fc::before([this](const fc::NativeContext &native, void *source, void *descriptor) noexcept {
                     begin_request(descriptor, descriptor, source ? pointer(field<std::uint32_t>(source, 0x14)) : nullptr,
                                   trace::SourceKind::Layered, nullptr, nullptr, source, native.return_address());
                 }),
                 fc::after([this](void *, void *) noexcept {
                     end_request();
                 }));
    plan.observe(layout->layered_bind, fc::after([this](void *task, void *, void *layer, float) noexcept {
                     bind_layer(task, layer);
                 }));
    plan.observe_instruction(layout->layered_due, fc::before([this, rva = layout->layered_due.rva](const fc::CpuContext &cpu) noexcept {
                                 auto *task = pointer(cpu.edi);
                                 auto *layer = pointer(field<std::uint32_t>(task, 0x38));
                                 begin_request(task, pointer(field<std::uint32_t>(layer, 0)), pointer(field<std::uint32_t>(task, 0x24)),
                                               trace::SourceKind::Layered);
                                 if (scopes_.request_depth <= scopes_.requests.size() && trace_.enabled()) {
                                     auto &record = scopes_.requests[scopes_.request_depth - 1];
                                     record.site = image_base_ + rva;
                                 }
                             }));
    plan.observe_instruction(layout->layered_due_end, fc::before([this](const fc::CpuContext &) noexcept {
                                 end_request();
                             }));
    plan.observe(layout->stream_play, fc::before([this](const fc::NativeContext &native, void *descriptor, void *source, bool) noexcept {
                     begin_request(descriptor, descriptor, source ? pointer(field<std::uint32_t>(source, 0x14)) : nullptr,
                                   trace::SourceKind::Stream, nullptr, nullptr, source, native.return_address());
                 }),
                 fc::after([this](void *, void *, bool) noexcept {
                     end_request();
                 }));
    plan.observe(layout->stream_setup, fc::before([this](const fc::NativeContext &native, void *descriptor, void *object, void *) noexcept {
                     begin_request(object, descriptor, nullptr, trace::SourceKind::Stream, nullptr, nullptr, nullptr,
                                   native.return_address());
                 }),
                 fc::after([this](void *descriptor, void *object, void *, bool accepted) noexcept {
                     bind_stream(descriptor, object, accepted);
                     end_request();
                 }));
    plan.observe(layout->stream_connect, fc::after([this](void *object, void *) noexcept {
                     stream_routes(object);
                 }));
    plan.observe(layout->stream_start, fc::before([this](const fc::NativeContext &native, void *object, bool restart) noexcept {
                     state(object, trace::RecordKind::StreamStart, 9, restart, native.return_address());
                 }),
                 fc::after([this](void *object, bool restart) noexcept {
                     state(object, trace::RecordKind::StreamStart, 1, restart);
                 }));
    plan.observe_instruction(layout->stream_close, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 state(pointer(cpu.ecx), trace::RecordKind::StreamStop, 6, false,
                                       field<std::uint32_t>(pointer(cpu.esp), 0));
                             }));
    plan.observe_instruction(layout->stream_queued, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 const ErrorGuard guard;
                                 if (!trace_.enabled()) {
                                     return;
                                 }
                                 auto *task = pointer(cpu.esi);
                                 auto *descriptor = pointer(field<std::uint32_t>(task, 0));
                                 trace::BindingRecord record{.header = header(task, trace::RecordKind::StreamQueued),
                                                             .generation = next_generation_.fetch_add(1, std::memory_order_relaxed),
                                                             .descriptor = reinterpret_cast<std::uintptr_t>(descriptor),
                                                             .sample = field<std::uint32_t>(task, 0x24),
                                                             .definition_id = field<std::uint32_t>(descriptor, 0x10),
                                                             .accepted = true};
                                 record.header.source = trace::SourceKind::Stream;
                                 if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
                                     record.request = scopes_.requests[scopes_.request_depth - 1].request;
                                 }
                                 write(record);
                             }));
    plan.observe_instruction(layout->stream_due, fc::before([this, rva = layout->stream_due.rva](const fc::CpuContext &cpu) noexcept {
                                 auto *task = pointer(cpu.esi);
                                 begin_request(task, pointer(field<std::uint32_t>(task, 0)), static_cast<std::byte *>(task) + 4,
                                               trace::SourceKind::Stream);
                                 if (scopes_.request_depth <= scopes_.requests.size() && trace_.enabled()) {
                                     auto &record = scopes_.requests[scopes_.request_depth - 1];
                                     record.site = image_base_ + rva;
                                 }
                             }));
    plan.observe_instruction(layout->stream_due_end, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 const ErrorGuard guard;
                                 if (static_cast<std::uint8_t>(cpu.eax) && scopes_.request_depth &&
                                     scopes_.request_depth <= scopes_.requests.size()) {
                                     scopes_.stream_start_frames[scopes_.request_depth - 1] = cpu.ebp;
                                 } else {
                                     end_request();
                                 }
                             }));
    plan.observe_instruction(layout->stream_deferred_start, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 const ErrorGuard guard;
                                 if (!scopes_.request_depth || scopes_.request_depth > scopes_.requests.size() ||
                                     scopes_.stream_start_frames[scopes_.request_depth - 1] != cpu.ebp) {
                                     return;
                                 }
                                 // Both the inline start and its already-active shortcut finish here; skipped slots have no scope.
                                 scopes_.stream_start_frames[scopes_.request_depth - 1] = 0;
                                 state(pointer(field<std::uint32_t>(pointer(cpu.ebp), -0xC)), trace::RecordKind::StreamStart, 1, true);
                                 end_request();
                             }));
    plan.observe(layout->voice_assignment, fc::before([this](const fc::NativeContext &native, void *voice, void *) noexcept {
                     begin_voice(voice, trace::RecordKind::VoiceAssignment, false, native.return_address());
                 }),
                 fc::after([this](void *voice, void *) noexcept {
                     end_voice(voice, trace::RecordKind::VoiceAssignment);
                 }));
    const std::array stream_states{layout->stream_requested_stop, layout->stream_requested_pause, layout->stream_requested_play,
                                   layout->stream_downstream_stop};
    for (std::uint8_t index = 0; index < stream_states.size(); ++index) {
        plan.observe_instruction(stream_states[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     state(pointer(cpu.ebx), trace::RecordKind::StreamState, index + 2);
                                 }));
    }
    plan.observe(layout->device_play,
                 fc::before([](const fc::NativeContext &native, void *wrapper, bool, std::int32_t, bool, bool) noexcept {
                     device_play_ = wrapper;
                     device_play_caller_ = native.return_address();
                 }),
                 fc::after([](void *, bool, std::int32_t, bool, bool, bool) noexcept {
                     device_play_ = nullptr;
                 }));
    plan.observe(layout->device_stop, fc::after([this](void *wrapper, std::int32_t result) noexcept {
                     native_api(wrapper, result, 2);
                 }));
    const std::array api_sites{layout->device_play_result,   layout->device_position_result, layout->flush_position_result,
                               layout->stop_position_result, layout->flush_lock_result,      layout->flush_unlock_result};
    for (std::uint8_t index = 0; index < api_sites.size(); ++index) {
        plan.observe_instruction(api_sites[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     api_instruction(cpu, index);
                                 }));
    }
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    const auto anchor = image_anchor_.view();
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    image_base_ = reinterpret_cast<std::uintptr_t>(anchor->data()) - image_anchor_rva_;
    auto channel = context.trace({.name = "SoundDiagnostics", .format_version = trace::kSchemaVersion, .buffer_bytes = 512 * 1024});
    if (!channel) {
        return std::unexpected(channel.error());
    }
    trace_ = *channel;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    counter_frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    if (auto result = context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                           [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                               hero_ = api;
                                               hero_ready_.store(true, std::memory_order_release);
                                           });
        !result) {
        return result;
    }
    return context.publish(api_contract(),
                           SoundDiagnosticsApi{.foot_maintenance_enabled = fc::bind<&Handler::foot_maintenance_enabled>(*this),
                                               .foot_maintenance = fc::bind<&Handler::foot_maintenance>(*this)});
}

void Handler::owner_instruction(const fc::CpuContext &cpu, std::uint8_t boundary) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    auto *object = pointer(cpu.esi);
    const auto now = ticks();
    if (boundary == 0) {
        owner_ = {.header = {.kind = trace::RecordKind::OwnerIteration, .object = reinterpret_cast<std::uintptr_t>(object)},
                  .iteration = next_iteration_.fetch_add(1, std::memory_order_relaxed),
                  .wait_start = now,
                  .frequency = counter_frequency_,
                  .waited = true};
        owner_.active = field<std::uint8_t>(object, 0x20);
        owner_.pause_requested = field<std::uint8_t>(object, 0x21);
    } else if (boundary == 1) {
        owner_.wait_end = now;
        // The resumed owner sees completed game-thread bindings before any new source can report its end.
        finish_packet_gaps();
    } else if (boundary == 2) {
        // A paused owner skips Wait entirely; the preceding completed iteration must not supply its timings.
        if (owner_.sleep_end || !owner_.iteration) {
            owner_ = {.header = {.kind = trace::RecordKind::OwnerIteration, .object = reinterpret_cast<std::uintptr_t>(object)},
                      .iteration = next_iteration_.fetch_add(1, std::memory_order_relaxed),
                      .frequency = counter_frequency_};
            owner_.active = field<std::uint8_t>(object, 0x20);
            owner_.pause_requested = field<std::uint8_t>(object, 0x21);
        }
        owner_.requested_sleep_ms = field<std::uint32_t>(pointer(cpu.esp), 0);
        owner_.nodes_visited = owner_.waited ? cpu.ebx : 0;
        owner_.sleep_start = now;
    } else {
        owner_.sleep_end = now;
        finish_packet_gaps();
        if (settings_.capture == CaptureMode::Full) {
            owner_.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(owner_);
        } else {
            auto &progress = owner_progress_;
            const auto had_callback = static_cast<std::uint8_t>(owner_.renderer_callbacks != 0);
            static thread_local bool state_seen{};
            const bool changed = !state_seen || progress.header.object != owner_.header.object || progress.active != owner_.active ||
                                 progress.pause_requested != owner_.pause_requested || progress.waited != owner_.waited ||
                                 progress.had_callback != had_callback || progress.requested_sleep_ms != owner_.requested_sleep_ms;
            if (changed && progress.iterations) {
                progress.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
                write(progress);
                progress.iterations = 0;
            }
            state_seen = true;
            if (!progress.iterations) {
                progress = {.header = {.kind = trace::RecordKind::OwnerProgress, .object = owner_.header.object},
                            .first_iteration = owner_.iteration,
                            .first_ticks = owner_.waited ? owner_.wait_start : owner_.sleep_start,
                            .frequency = counter_frequency_,
                            .requested_sleep_ms = owner_.requested_sleep_ms,
                            .active = owner_.active,
                            .pause_requested = owner_.pause_requested,
                            .waited = owner_.waited,
                            .had_callback = static_cast<std::uint8_t>(had_callback)};
            }
            ++progress.iterations;
            progress.last_iteration = owner_.iteration;
            progress.last_ticks = now;
            progress.renderer_callbacks += owner_.renderer_callbacks;
            progress.idle_callbacks += owner_.idle_callbacks;
            progress.nodes_visited += owner_.nodes_visited;
            if (!progress.first_callback) {
                progress.first_callback = owner_.first_callback;
            }
            if (owner_.last_callback) {
                progress.last_callback = owner_.last_callback;
            }
            const auto wait = owner_.waited && owner_.wait_end >= owner_.wait_start ? owner_.wait_end - owner_.wait_start : 0;
            if (wait > progress.max_wait) {
                progress.max_wait = wait;
                progress.wait_start = owner_.wait_start;
                progress.wait_end = owner_.wait_end;
            }
            const auto sleep = owner_.sleep_end - owner_.sleep_start;
            if (sleep > progress.max_sleep) {
                progress.max_sleep = sleep;
                progress.sleep_start = owner_.sleep_start;
                progress.sleep_end = owner_.sleep_end;
            }
            if (changed || now - progress.first_ticks >= counter_frequency_ || cpu.ebx == 0) {
                progress.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
                write(progress);
                progress.iterations = 0;
            }
        }
        // Publish channel loss and observation coverage once per second.
        static thread_local std::uint64_t previous_summary{};
        if (!previous_summary || now - previous_summary >= counter_frequency_ || cpu.ebx == 0) {
            flush_runs();
            flush_renderers();
            coverage();
            previous_summary = now;
        }
    }
}

void Handler::begin_request(void *object, void *descriptor, void *position, trace::SourceKind source, void *callback, void *data,
                            void *source_object, std::uintptr_t caller, std::uint8_t native_flags) noexcept {
    const ErrorGuard guard;
    const auto index = scopes_.request_depth++;
    if (index >= scopes_.requests.size()) {
        scope_overflows_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto &record = scopes_.requests[index];
    scopes_.stream_start_frames[index] = 0;
    scopes_.emitted[index] = scopes_.effects[index] = false;
    record = {.header = {.kind = trace::RecordKind::Request, .object = reinterpret_cast<std::uintptr_t>(object)},
              .request = next_request_.fetch_add(1, std::memory_order_relaxed),
              .parent = index ? scopes_.requests[index - 1].request : 0,
              .caller = caller,
              .descriptor = reinterpret_cast<std::uintptr_t>(descriptor),
              .position_pointer = reinterpret_cast<std::uintptr_t>(position),
              .callback = reinterpret_cast<std::uintptr_t>(callback),
              .callback_data = reinterpret_cast<std::uintptr_t>(data),
              .source_object = reinterpret_cast<std::uintptr_t>(source_object)};
    record.native_flags = native_flags;
    record.entry_ticks = trace_.enabled() ? ticks() : 0;
    record.control_cause = scopes_.control_cause;
    record.header.source = source;
    attach_ui(record.ui_event, record.ui_callback);
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = hero_.current_scope();
        record.subject = scope.subject;
        record.generation = scope.generation;
        record.operation_scope = scope.operation_scope;
        record.read_scope = scope.read_scope;
        record.weapon = scope.weapon;
        record.host_turn = scope.host_turn;
        record.prediction_turn = scope.prediction_turn;
        record.update_turn = scope.update_turn;
        record.acknowledged_turn = scope.acknowledged_turn;
        record.phase = scope.phase;
    }
    if (source == trace::SourceKind::Game || (source == trace::SourceKind::Controlled && descriptor)) {
        auto *sound = source == trace::SourceKind::Game ? object : descriptor;
        record.descriptor_type = field<std::uint8_t>(sound, 4);
        record.descriptor = field<std::uint32_t>(sound, 0);
    } else {
        record.descriptor_type = source == trace::SourceKind::Ordinary  ? 1
                                 : source == trace::SourceKind::Layered ? 2
                                 : source == trace::SourceKind::Stream  ? 3
                                                                        : 0;
    }
    if (record.descriptor && record.descriptor_type) {
        record.definition_id = record.descriptor_type == 4 ? static_cast<std::uint32_t>(record.descriptor)
                                                           : field<std::uint32_t>(pointer(static_cast<std::uint32_t>(record.descriptor)),
                                                                                  record.descriptor_type == 3 ? 0x10 : 4);
    }
    if (position) {
        std::memcpy(record.position, position, sizeof(record.position));
        record.position_valid = true;
    }
}

void Handler::materialize_requests() noexcept {
    if (scopes_.request_depth) {
        flush_runs();
    }
    for (std::size_t index = 0; index < std::min<std::size_t>(scopes_.request_depth, scopes_.requests.size()); ++index) {
        scopes_.effects[index] = true;
        if (!scopes_.emitted[index]) {
            scopes_.emitted[index] = true;
            scopes_.requests[index].header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write_bytes(std::as_bytes(std::span{&scopes_.requests[index], 1}));
        }
    }
}

void Handler::flush_runs() noexcept {
    for (auto &run : request_runs_) {
        if (run.summary.count > 1) {
            run.summary.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(run.summary);
        }
        run = {};
    }
    for (auto &run : route_runs_) {
        if (run.summary.count > 1) {
            run.summary.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(run.summary);
        }
        run = {};
    }
}

void Handler::update(const fc::UpdateContext &) noexcept {
    const ErrorGuard guard;
    static thread_local std::uint64_t checkpoint{};
    if (!trace_.enabled()) {
        return;
    }
    const auto now = ticks();
    if (!checkpoint || now - checkpoint >= counter_frequency_) {
        flush_runs();
        checkpoint = now;
    }
}

void Handler::end_request() noexcept {
    const ErrorGuard guard;
    if (!scopes_.request_depth) {
        return;
    }
    const auto index = scopes_.request_depth - 1;
    if (index >= scopes_.requests.size() || !trace_.enabled()) {
        --scopes_.request_depth;
        return;
    }
    auto request = scopes_.requests[index];
    request.end_ticks = ticks();
    request.completed = 1;
    if (request.header.source == trace::SourceKind::Game) {
        auto *object = pointer(static_cast<std::uint32_t>(request.header.object));
        request.descriptor = field<std::uint32_t>(object, 0);
        request.descriptor_type = field<std::uint8_t>(object, 4);
        request.definition_id =
            request.descriptor && request.descriptor_type
                ? (request.descriptor_type == 4 ? static_cast<std::uint32_t>(request.descriptor)
                                                : field<std::uint32_t>(pointer(static_cast<std::uint32_t>(request.descriptor)),
                                                                       request.descriptor_type == 3 ? 0x10 : 4))
                : 0;
    }
    const bool eligible =
        index == 0 && !scopes_.effects[index] && !scopes_.emitted[index] &&
        (request.rejection || (request.header.source == trace::SourceKind::Controlled && request.controller_reason == 2)) &&
        !request.starts && !request.callback && !request.callback_data && request.controller_before == request.controller_after &&
        request.controller_flags_before == request.controller_flags_after;
    if (eligible) {
        RequestRun *slot{};
        for (auto &run : request_runs_) {
            if (trace::same_refusal(run.first, request)) {
                run.summary.last_request = request.request;
                run.summary.last_ticks = request.end_ticks;
                ++run.summary.count;
                run.summary.last_operation_scope = request.operation_scope;
                run.summary.last_read_scope = request.read_scope;
                run.summary.last_host_turn = request.host_turn;
                run.summary.last_prediction_turn = request.prediction_turn;
                run.summary.last_update_turn = request.update_turn;
                run.summary.last_acknowledged_turn = request.acknowledged_turn;
                std::memcpy(run.summary.last_position, request.position, sizeof(request.position));
                run.summary.last_position_valid = request.position_valid;
                suppressed_requests_.fetch_add(1, std::memory_order_relaxed);
                --scopes_.request_depth;
                return;
            }
            if (run.summary.count && run.first.header.object == request.header.object && run.first.caller == request.caller &&
                run.first.header.source == request.header.source) {
                if (run.summary.count > 1) {
                    run.summary.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
                    write(run.summary);
                }
                run = {};
            }
            if (!run.summary.count && !slot) {
                slot = &run;
            }
        }
        if (!slot) {
            flush_runs();
            slot = &request_runs_[0];
        }
        *slot = {
            .first = request,
            .summary = {.header = {.kind = trace::RecordKind::RequestRun, .source = request.header.source, .object = request.header.object},
                        .first_request = request.request,
                        .last_request = request.request,
                        .first_ticks = request.entry_ticks,
                        .last_ticks = request.end_ticks,
                        .count = 1}};
        slot->summary.header.reserved =
            !request.subject && !request.weapon && !request.control_cause && !request.ui_event && !request.ui_callback;
        slot->summary.last_operation_scope = request.operation_scope;
        slot->summary.last_read_scope = request.read_scope;
        slot->summary.last_host_turn = request.host_turn;
        slot->summary.last_prediction_turn = request.prediction_turn;
        slot->summary.last_update_turn = request.update_turn;
        slot->summary.last_acknowledged_turn = request.acknowledged_turn;
        std::memcpy(slot->summary.last_position, request.position, sizeof(request.position));
        slot->summary.last_position_valid = request.position_valid;
    } else {
        flush_runs();
    }
    if (scopes_.emitted[index]) {
        write(trace::RequestResultRecord{
            .header = header(pointer(static_cast<std::uint32_t>(request.header.object)), trace::RecordKind::RequestResult),
            .request = request.request,
            .descriptor = request.descriptor,
            .starts = request.starts,
            .rejection = request.rejection,
            .definition_id = request.definition_id,
            .descriptor_type = request.descriptor_type,
            .controller_before = request.controller_before,
            .controller_after = request.controller_after,
            .controller_flags_before = request.controller_flags_before,
            .controller_flags_after = request.controller_flags_after,
            .controller_reason = request.controller_reason,
            .stop_argument = request.stop_argument});
    } else {
        // Complete nested outcomes retain their parent's immutable identity before referencing it.
        --scopes_.request_depth;
        if (index) {
            materialize_requests();
        }
        request.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        write(request);
        return;
    }
    --scopes_.request_depth;
}

void Handler::request_gate(const fc::CpuContext &cpu, std::uint8_t boundary) noexcept {
    const ErrorGuard guard;
    if (!scopes_.request_depth || scopes_.request_depth > scopes_.requests.size()) {
        return;
    }
    auto &record = scopes_.requests[scopes_.request_depth - 1];
    if (boundary < 2 && !(cpu.eax & 0xFF)) {
        record.rejection = boundary + 1;
    } else if (boundary == 2 && !cpu.eax) {
        record.rejection = 3;
    } else if (boundary == 3 && field<std::uint8_t>(pointer(cpu.ebp), 0x18)) {
        record.rejection = 4;
    } else if (boundary == 4 && !cpu.eax && !cpu.ebx) {
        record.rejection = 5;
    } else if (boundary == 5 && !(cpu.eax & 0xFF)) {
        record.rejection = 1;
    } else if (boundary == 6 && !cpu.esi) {
        record.rejection = 7;
    } else if (boundary == 7 && !cpu.eax) {
        record.rejection = 8;
    } else if (boundary == 8 && !(cpu.eflags & 1)) {
        record.rejection = 9;
    } else if (boundary == 9 && !cpu.edi) {
        record.rejection = 3;
    } else if (boundary == 10 && !(cpu.eflags & 1)) {
        record.rejection = 10;
    } else if (boundary == 11 && !(cpu.eax & 0xFF)) {
        record.rejection = 2;
    } else if (boundary == 12 && !(cpu.eflags & 1)) {
        record.rejection = 4;
    } else if (boundary == 13 && !cpu.esi) {
        record.rejection = 11;
    } else if (boundary == 14 && (cpu.eflags & 1)) {
        record.rejection = 12;
    } else if (boundary == 15 && !cpu.edi) {
        record.rejection = 13;
    }
}

trace::VoiceRecord Handler::voice_record(void *voice, trace::RecordKind kind) noexcept {
    trace::VoiceRecord record{.header = header(voice, kind)};
    record.control_cause = scopes_.control_cause;
    for (const auto &binding : bindings_) {
        if (binding.object.load(std::memory_order_acquire) == reinterpret_cast<std::uintptr_t>(voice)) {
            const auto generation = binding.generation.load(std::memory_order_acquire);
            record.request = binding.request.load(std::memory_order_relaxed);
            if (generation && generation == binding.generation.load(std::memory_order_acquire)) {
                record.generation = generation;
            } else {
                record.request = 0;
                voice_overflows_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
    }
    record.sample = field<std::uint32_t>(voice, 0x7C);
    if (record.sample) {
        record.sample_frequency = field<std::uint32_t>(pointer(static_cast<std::uint32_t>(record.sample)), 8);
        record.sample_length = field<std::uint32_t>(pointer(static_cast<std::uint32_t>(record.sample)), 0x18);
    }
    record.physical = field<std::uint32_t>(voice, 0xA0);
    record.physical_after = record.physical;
    record.header.source = trace::SourceKind::Ordinary;
    record.callback = field<std::uint32_t>(voice, 0xC0);
    record.callback_data = field<std::uint32_t>(voice, 0xC4);
    record.position_before = field<std::uint32_t>(voice, 0x68);
    record.position_after = record.position_before;
    record.inserted_before = field<std::uint32_t>(voice, 0x9C);
    record.inserted_after = record.inserted_before;
    record.transport_before = field<std::uint16_t>(voice, 0x6C);
    record.transport_after = record.transport_before;
    record.inside_stop = scopes_.stop_depth != 0;
    return record;
}

void Handler::bind_voice(void *voice) noexcept {
    const ErrorGuard guard;
    const auto generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
    const auto request =
        scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size() ? scopes_.requests[scopes_.request_depth - 1].request : 0;
    bool mapped = false;
    for (auto &binding : bindings_) {
        auto expected = std::uintptr_t{};
        if (binding.object.load(std::memory_order_acquire) == reinterpret_cast<std::uintptr_t>(voice) ||
            binding.object.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(voice), std::memory_order_acq_rel)) {
            binding.generation.store(0, std::memory_order_release);
            binding.request.store(request, std::memory_order_relaxed);
            binding.physical.store(0, std::memory_order_relaxed);
            binding.generation.store(generation, std::memory_order_release);
            mapped = true;
            break;
        }
    }
    if (!mapped) {
        voice_overflows_.fetch_add(1, std::memory_order_relaxed);
    }
    if (trace_.enabled()) {
        auto record = voice_record(voice, trace::RecordKind::VoiceBind);
        record.generation = generation;
        record.request = request;
        write(record);
    }
}

void Handler::begin_voice(void *voice, trace::RecordKind kind, bool restart, std::uintptr_t caller) noexcept {
    const ErrorGuard guard;
    const auto index = scopes_.voice_depth++;
    if (index < scopes_.voices.size()) {
        scopes_.voices[index] = voice_record(voice, kind);
        scopes_.voices[index].restart = restart;
        scopes_.voices[index].caller = caller;
    } else {
        scope_overflows_.fetch_add(1, std::memory_order_relaxed);
    }
    if (kind == trace::RecordKind::VoiceStop) {
        ++scopes_.stop_depth;
    }
    if (kind == trace::RecordKind::VoiceStart) {
        for (std::size_t request = 0; request < std::min<std::size_t>(scopes_.request_depth, scopes_.requests.size()); ++request) {
            ++scopes_.requests[request].starts;
        }
    }
}

void Handler::end_voice(void *voice, trace::RecordKind kind) noexcept {
    const ErrorGuard guard;
    if (!scopes_.voice_depth) {
        return;
    }
    const auto index = --scopes_.voice_depth;
    if (index >= scopes_.voices.size()) {
        if (kind == trace::RecordKind::VoiceStop) {
            --scopes_.stop_depth;
        }
        return;
    }
    auto record = scopes_.voices[index];
    record.physical_after = field<std::uint32_t>(voice, 0xA0);
    record.position_after = field<std::uint32_t>(voice, 0x68);
    record.inserted_after = field<std::uint32_t>(voice, 0x9C);
    record.transport_after = field<std::uint16_t>(voice, 0x6C);
    if (kind == trace::RecordKind::VoiceAssignment) {
        assign_physical(record);
    }
    if (record.header.kind == trace::RecordKind::VoiceStop) {
        --scopes_.stop_depth;
    }
    if (trace_.enabled()) {
        write(record);
    }
}

void Handler::begin_ui_event(const fc::CpuContext &cpu) noexcept {
    const ErrorGuard guard;
    const auto index = scopes_.ui_event_depth++;
    if (index >= scopes_.ui_events.size()) {
        scope_overflows_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto &scope = scopes_.ui_events[index];
    scope = {};
    auto *event = pointer(cpu.esi);
    auto *manager = pointer(cpu.edi);
    auto &record = scope.record;
    record = {.header = header(manager, trace::RecordKind::UiEvent),
              .event = next_ui_event_.fetch_add(1, std::memory_order_relaxed),
              .dispatch = scopes_.ui_dispatch,
              .caller = scopes_.ui_dispatch_caller,
              .type = field<std::uint32_t>(event, 0),
              .value = field<std::uint32_t>(event, 4),
              .controller = field<std::int32_t>(event, 8)};
    record.native_event = cpu.esi;
    // Native manager names are fixed 64-byte arrays; lifecycle events own a name at +0xC.
    std::memcpy(record.screen, static_cast<const std::byte *>(manager) + 0x34, sizeof(record.screen));
    std::memcpy(record.popup, static_cast<const std::byte *>(manager) + 0x74, sizeof(record.popup));
    std::memcpy(record.error, static_cast<const std::byte *>(manager) + 0xB4, sizeof(record.error));
    if (record.type == 2 || record.type == 3) {
        std::memcpy(record.name, static_cast<const std::byte *>(event) + 0xC, sizeof(record.name));
    }
    if (trace_.enabled() && record.type != 5) {
        write(record);
        scope.emitted = true;
    }
}

void Handler::end_ui_event() noexcept {
    if (scopes_.ui_event_depth) {
        --scopes_.ui_event_depth;
    }
}

void Handler::begin_ui_callback(const fc::CpuContext &cpu, std::uint32_t callback_index, std::uint32_t rva) noexcept {
    const ErrorGuard guard;
    const auto index = scopes_.ui_callback_depth++;
    if (index >= scopes_.ui_callbacks.size()) {
        scope_overflows_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto &scope = scopes_.ui_callbacks[index];
    scope = {};
    auto &record = scope.record;
    record = {.header = header(pointer(cpu.ecx), trace::RecordKind::UiCallback),
              .callback = next_ui_callback_.fetch_add(1, std::memory_order_relaxed),
              .site = image_base_ + rva};
    if (scopes_.ui_event_depth && scopes_.ui_event_depth <= scopes_.ui_events.size()) {
        record.event = scopes_.ui_events[scopes_.ui_event_depth - 1].record.event;
    }
    // A callback can change the manager's table name before requesting sound; retain its entry identity.
    const auto scope_truncated = copy_name(record.scope, static_cast<const char *>(pointer(cpu.ecx)));
    const auto function_truncated = copy_name(record.function, static_cast<const char *>(pointer(cpu.edx)));
    record.truncated = static_cast<std::uint8_t>(scope_truncated || function_truncated);
    scope.completion = callback_index;
    scope.frame = cpu.ebp;
}

void Handler::end_ui_callback(const fc::CpuContext &cpu, std::uint32_t callback_index) noexcept {
    const ErrorGuard guard;
    if (!scopes_.ui_callback_depth) {
        return;
    }
    // Popup continuation sites also run when no popup was called; preserve any enclosing callback scope.
    if (scopes_.ui_callback_depth <= scopes_.ui_callbacks.size()) {
        const auto &scope = scopes_.ui_callbacks[scopes_.ui_callback_depth - 1];
        // A nested helper may skip its popup and reach this continuation while an outer popup remains active.
        if (scope.completion != callback_index || scope.frame != cpu.ebp) {
            return;
        }
    }
    const auto index = --scopes_.ui_callback_depth;
    if (index < scopes_.ui_callbacks.size() && scopes_.ui_callbacks[index].emitted && trace_.enabled()) {
        auto record = scopes_.ui_callbacks[index].record;
        record.header = header(pointer(static_cast<std::uint32_t>(record.header.object)), trace::RecordKind::UiCallback);
        record.completed = true;
        record.result = static_cast<std::uint8_t>(cpu.eax);
        write(record);
    }
}

void Handler::attach_ui(std::uint64_t &event, std::uint64_t &callback) noexcept {
    if (scopes_.ui_event_depth && scopes_.ui_event_depth <= scopes_.ui_events.size()) {
        auto &scope = scopes_.ui_events[scopes_.ui_event_depth - 1];
        event = scope.record.event;
        if (!scope.emitted && trace_.enabled()) {
            write(scope.record);
            scope.emitted = true;
        }
    }
    if (scopes_.ui_callback_depth && scopes_.ui_callback_depth <= scopes_.ui_callbacks.size()) {
        auto &scope = scopes_.ui_callbacks[scopes_.ui_callback_depth - 1];
        callback = scope.record.callback;
        if (!scope.emitted && trace_.enabled()) {
            write(scope.record);
            scope.emitted = true;
        }
    }
}

void Handler::queue_ui_event(const fc::CpuContext &cpu) noexcept {
    if (!trace_.enabled() || !cpu.edi) {
        return;
    }
    auto *event = pointer(cpu.edi);
    trace::UiQueuedRecord record{.header = header(event, trace::RecordKind::UiQueued),
                                 .manager = cpu.ebx,
                                 .caller = field<std::uint32_t>(pointer(cpu.ebp), 4),
                                 .type = field<std::uint32_t>(event, 0),
                                 .value = field<std::uint32_t>(event, 4),
                                 .controller = field<std::int32_t>(event, 8)};
    std::memcpy(record.name, static_cast<const std::byte *>(event) + 0xC, sizeof(record.name));
    // A producer callback may only queue an event; retain its name even when the sound starts later.
    attach_ui(record.parent_event, record.parent_callback);
    write(record);
}

void Handler::begin_control_cause(trace::RecordKind kind, std::uintptr_t caller) noexcept {
    route_epoch_.fetch_add(1, std::memory_order_release);
    flush_runs();
    if (!trace_.enabled()) {
        return;
    }

    trace::ControlCauseRecord record{.header = header(nullptr, kind),
                                     .cause = next_control_cause_.fetch_add(1, std::memory_order_relaxed),
                                     .parent = scopes_.control_cause,
                                     .caller = caller};
    attach_ui(record.ui_event, record.ui_callback);
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = hero_.current_scope();
        record.subject = scope.subject;
        record.operation_scope = scope.operation_scope;
        record.read_scope = scope.read_scope;
        record.host_turn = scope.host_turn;
        record.prediction_turn = scope.prediction_turn;
        record.update_turn = scope.update_turn;
        record.phase = scope.phase;
    }
    scopes_.control_cause = record.cause;
    write(record);
}

void Handler::health_decision(const fc::CpuContext &cpu, bool play, std::uint32_t rva) noexcept {
    if (!trace_.enabled()) {
        return;
    }

    auto *base = pointer(cpu.edi);
    auto *properties = pointer(field<std::uint32_t>(base, 0x200));
    auto *controller = pointer(cpu.ecx);
    trace::HealthDecisionRecord record{.header = header(pointer(cpu.edi - 0x240U), trace::RecordKind::HealthDecision),
                                       .site = image_base_ + rva,
                                       .caller = field<std::uint32_t>(pointer(cpu.ebp), 4),
                                       .controller = cpu.ecx,
                                       .properties = reinterpret_cast<std::uintptr_t>(properties),
                                       .health = field<float>(base, -0xFC),
                                       .maximum_health = field<float>(base, -0xF8),
                                       .threshold = properties ? field<float>(properties, 0x108C) : 0.0F,
                                       .soldier_flags = field<std::uint32_t>(base, -0x44),
                                       .controller_index = field<std::uint16_t>(controller, 0),
                                       .controller_flags = field<std::uint8_t>(controller, 2),
                                       .play = play};
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = hero_.current_scope();
        record.operation_scope = scope.operation_scope;
        record.read_scope = scope.read_scope;
    }
    write(record);
}

void Handler::assign_physical(const trace::VoiceRecord &record) noexcept {
    route_epoch_.fetch_add(1, std::memory_order_release);
    flush_runs();
    // A physical slot can move to a new virtual voice while the old virtual object is still allocated.
    for (auto &binding : bindings_) {
        if (binding.object.load(std::memory_order_acquire) == record.header.object) {
            binding.physical.store(static_cast<std::uintptr_t>(record.physical_after), std::memory_order_release);
        } else if (record.physical_after && binding.physical.load(std::memory_order_acquire) == record.physical_after) {
            binding.physical.store(0, std::memory_order_release);
        }
    }
}

void Handler::packet(void *resampler, void *native_packet) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled() || (native_packet && !pending_packet_gaps_)) {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(resampler);
    // A returned packet is recorded only when it recovers an observed empty-source gap.
    const bool retain_packet = native_packet && std::ranges::any_of(packet_gaps_, [address](const PacketGap &gap) noexcept {
                                   return gap.pending && gap.record.header.object == address;
                               });
    for (const auto &binding : bindings_) {
        const auto physical = binding.physical.load(std::memory_order_acquire);
        // Voice owns main/LFE SourceResampler at +0x3D4/+0x490; each embeds StreamResampler at +0x64.
        if (!physical || (address != physical + 0x438 && address != physical + 0x4F4)) {
            continue;
        }
        const auto generation = binding.generation.load(std::memory_order_acquire);
        trace::PacketRecord record{.generation = generation,
                                   .request = binding.request.load(std::memory_order_relaxed),
                                   .physical = physical,
                                   .sample = field<std::uint32_t>(pointer(static_cast<std::uint32_t>(physical)), 0x7C),
                                   .packet = reinterpret_cast<std::uintptr_t>(native_packet)};
        if (native_packet && retain_packet) {
            record.header = header(resampler, trace::RecordKind::PacketProduced);
            record.production = next_packet_.fetch_add(1, std::memory_order_relaxed);
            record.position = field<std::uint32_t>(native_packet, 0);
            record.bytes = field<std::uint32_t>(native_packet, 8);
            record.flags = field<std::uint16_t>(native_packet, 0xE);
            record.references = field<std::uint16_t>(native_packet, 0xC);
        }
        if (!generation || generation != binding.generation.load(std::memory_order_acquire) ||
            physical != binding.physical.load(std::memory_order_acquire)) {
            voice_overflows_.fetch_add(1, std::memory_order_relaxed);
            record.generation = 0;
            record.request = 0;
        }
        packet_gap(resampler, record, native_packet != nullptr);
        if (native_packet && retain_packet) {
            write(record);
        }
        return;
    }
}

void Handler::end_packet_gap(PacketGap &gap, std::uint8_t stage) noexcept {
    auto record = gap.record;
    record.header = header(pointer(static_cast<std::uint32_t>(record.header.object)), trace::RecordKind::PacketGap);
    record.stage = stage;
    write(record);
    gap.pending = false;
    --pending_packet_gaps_;
}

void Handler::packet_gap(void *resampler, const trace::PacketRecord &owner, bool available) noexcept {
    if (available && !pending_packet_gaps_) {
        return;
    }
    if (!available) {
        empty_packet_returns_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!owner.generation) {
        return;
    }
    PacketGap *slot{};
    const auto address = reinterpret_cast<std::uintptr_t>(resampler);
    for (auto &gap : packet_gaps_) {
        if (gap.record.header.object == address) {
            slot = &gap;
            break;
        }
        if (!slot && !gap.pending) {
            slot = &gap;
        }
    }
    if (!slot) {
        voice_overflows_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto stage = trace::packet_gap_end_stage(slot->record, owner.generation, owner.physical, false);
    if (slot->pending && stage == 4) {
        end_packet_gap(*slot, stage);
    }
    if (available) {
        if (slot->pending && stage == 2) {
            end_packet_gap(*slot, stage);
        }
        return;
    }
    const auto now = ticks();
    const bool beginning = !slot->pending;
    if (beginning) {
        slot->record = {.header = header(resampler, trace::RecordKind::PacketGap),
                        .generation = owner.generation,
                        .request = owner.request,
                        .physical = owner.physical,
                        .sample = owner.sample,
                        .first_ticks = now,
                        .stage = 1};
        slot->pending = true;
        ++pending_packet_gaps_;
    }
    auto &record = slot->record;
    record.last_ticks = now;
    ++record.null_returns;
    // Both ordinary resamplers embed their source at -0x64; values are copied while GetPacket owns the live object.
    auto *source = static_cast<std::byte *>(resampler) - 0x64;
    record.pending_packet = field<std::uint32_t>(resampler, 0x20);
    record.source_position = field<std::uint32_t>(source, 0x18);
    record.source_bytes = field<std::uint32_t>(source, 0x38);
    record.segment_bytes = field<std::uint32_t>(source, 0x28);
    record.source_transport = field<std::uint16_t>(source, 0x1C);
    record.resampler_references = field<std::uint16_t>(resampler, 0x34);
    if (beginning) {
        write(record);
    }
}

void Handler::finish_packet_gaps() noexcept {
    if (!pending_packet_gaps_) {
        return;
    }

    for (auto &gap : packet_gaps_) {
        if (!gap.pending) {
            continue;
        }
        bool retained{};
        bool publishing{};
        bool replaced{};
        for (const auto &binding : bindings_) {
            const auto generation = binding.generation.load(std::memory_order_acquire);
            const auto physical = binding.physical.load(std::memory_order_acquire);
            // Zero marks an unfinished generation publication, so defer closure until the identity is stable.
            if ((!generation && binding.object.load(std::memory_order_acquire)) ||
                generation != binding.generation.load(std::memory_order_acquire)) {
                publishing = true;
                continue;
            }
            if (generation == gap.record.generation && physical == gap.record.physical) {
                retained = true;
                break;
            }
            replaced |= physical == gap.record.physical && generation != gap.record.generation;
        }
        if (!retained && !publishing) {
            end_packet_gap(gap, replaced ? 4 : 3);
        }
    }
}

void Handler::source_end(const fc::CpuContext &cpu) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    auto *stack = pointer(cpu.esp);
    const auto sample_end = field<std::uint8_t>(stack, 0xC);
    const auto transport_end = field<std::uint8_t>(stack, 0x10);
    if (!sample_end && !transport_end) {
        return;
    }

    auto *source = pointer(cpu.esi);
    auto *native_packet = pointer(field<std::uint32_t>(stack, 0));
    trace::SourceEndRecord record{
        .header = {},
        .packet = reinterpret_cast<std::uintptr_t>(native_packet),
        .bytes = field<std::uint32_t>(stack, 4),
        .position = field<std::uint32_t>(stack, 0x14),
        .segment_bytes = field<std::uint32_t>(source, 0x28),
        .next_segment_bytes = field<std::uint32_t>(source, 0x30),
        .desired_transport = field<std::uint32_t>(pointer(cpu.ebp), -0x14),
        .transport = field<std::uint16_t>(source, 0x1C),
        .sample_end = sample_end,
        .transport_end = transport_end,
        // PacketComplete has not yet updated its header; the actual data pointer was already written by SourceUpdate.
        .data = field<std::uint32_t>(native_packet, 4),
        .source_base = field<std::uint32_t>(source, 0x34),
        .segment_start = field<std::uint32_t>(source, 0x24),
        .next_segment_start = field<std::uint32_t>(source, 0x2C),
        .total_bytes = field<std::uint32_t>(source, 0x38),
        .stride = field<std::uint32_t>(source, 0x3C),
        .loop = field<std::uint8_t>(source, 0x48)};
    bool associated = false;
    for (const auto &binding : bindings_) {
        const auto physical = binding.physical.load(std::memory_order_acquire);
        if (!physical || (cpu.esi != physical + 0x3D4 && cpu.esi != physical + 0x490)) {
            continue;
        }
        record.generation = binding.generation.load(std::memory_order_acquire);
        record.request = binding.request.load(std::memory_order_relaxed);
        record.physical = physical;
        record.sample = field<std::uint32_t>(pointer(static_cast<std::uint32_t>(physical)), 0x7C);
        if (!record.generation || record.generation != binding.generation.load(std::memory_order_acquire) ||
            physical != binding.physical.load(std::memory_order_acquire)) {
            record.generation = 0;
            record.request = 0;
            voice_overflows_.fetch_add(1, std::memory_order_relaxed);
        }
        associated = true;
        break;
    }
    // Ignore source ends that have no matching ordinary physical binding.
    if (!associated) {
        return;
    }
    record.header = header(source, trace::RecordKind::SourceEnd);
    write(record);
    for (auto &gap : packet_gaps_) {
        if (gap.pending && gap.record.header.object == cpu.esi + 0x64U) {
            const auto stage = trace::packet_gap_end_stage(gap.record, record.generation, record.physical, true);
            if (stage) {
                end_packet_gap(gap, stage);
            }
            break;
        }
    }
}

void Handler::voice_decision(const fc::CpuContext &cpu, trace::VoiceDecision decision, std::uint32_t rva) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }

    const auto voice = voice_record(pointer(cpu.edi), trace::RecordKind::VoiceDecision);
    trace::VoiceDecisionRecord record{.header = voice.header,
                                      .generation = voice.generation,
                                      .request = voice.request,
                                      .physical = voice.physical,
                                      .sample = voice.sample,
                                      .site = image_base_ + rva,
                                      .position = voice.position_before,
                                      .length = voice.sample_length,
                                      .transport = voice.transport_before,
                                      .decision = decision};
    if (voice.physical) {
        record.physical_transport = field<std::uint16_t>(pointer(static_cast<std::uint32_t>(voice.physical)), 0x6C);
    }
    write(record);
}

void Handler::saber_decision(const fc::CpuContext &cpu, trace::SaberDecision decision, std::uint32_t rva) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }

    auto *weapon = pointer(cpu.ebx);
    const auto flags = field<std::uint8_t>(weapon, 0x179);
    const auto masks = field<std::uint8_t>(weapon, 0x17C);
    trace::SaberDecisionRecord record{.header = header(weapon, trace::RecordKind::SaberDecision),
                                      .site = image_base_ + rva,
                                      // UpdateEffects uses XMM1 for delta and has no typed hook; its EBP frame retains the native caller.
                                      .caller = field<std::uint32_t>(pointer(cpu.ebp), 4),
                                      .controller = cpu.ecx,
                                      .posture = field<std::uint32_t>(weapon, 0xAC),
                                      .turn_on_time = field<float>(weapon, 0x1A0),
                                      .state_flags = flags,
                                      .action_masks = masks,
                                      .sound_masks = field<std::uint8_t>(weapon, 0x17D),
                                      .desired_mask = static_cast<std::uint8_t>((flags & 2U) ? masks >> 4U : 0U),
                                      .ambient_flags = field<std::uint8_t>(weapon, 0xCE),
                                      .on_off_flags = field<std::uint8_t>(weapon, 0x1B2),
                                      .decision = decision,
                                      .ambient_wrapper = cpu.ebx + 0xD8U,
                                      .ambient_properties = field<std::uint32_t>(weapon, 0xD8),
                                      .ambient_index = field<std::uint16_t>(weapon, 0xCC),
                                      .ambient_type = field<std::uint8_t>(weapon, 0xDC)};
    if (decision == trace::SaberDecision::AmbientPropertiesChanged) {
        record.old_properties = cpu.edx;
        record.new_properties = cpu.eax;
        record.desired_wrapper = cpu.edi;
        record.desired_type = field<std::uint8_t>(pointer(cpu.edi), 4);
    }
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = hero_.current_scope();
        record.operation_scope = scope.operation_scope;
        record.read_scope = scope.read_scope;
        const auto identity = hero_.weapon_scope(weapon);
        record.subject = identity.subject;
        record.generation = identity.generation;
    }
    record.property_object = field<std::uint32_t>(weapon, 0x64);
    record.control_cause = scopes_.control_cause;
    write(record);
}

void Handler::begin_ambient_binding(void *weapon, std::uintptr_t caller, std::uint32_t rva, trace::AmbientBoundary boundary,
                                    AmbientInvocation &invocation) noexcept {
    const ErrorGuard guard;
    invocation.observed = trace_.enabled();
    if (!invocation.observed) {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(weapon);
    invocation.record = {.header = {.kind = trace::RecordKind::AmbientBinding, .source = trace::SourceKind::Controlled, .object = address},
                         .site = image_base_ + rva,
                         .caller = caller,
                         .controller = address + 0xCCU,
                         .wrapper = address + 0xD8U,
                         .before = ambient_binding(weapon),
                         .boundary = boundary};
    if (hero_ready_.load(std::memory_order_acquire)) {
        const auto scope = hero_.weapon_scope(weapon);
        // An enclosing operation may concern another weapon; never borrow its identity for this native call.
        if (scope.weapon == address) {
            auto &record = invocation.record;
            record.subject = scope.subject;
            record.generation = scope.generation;
            record.operation_scope = scope.operation_scope;
            record.read_scope = scope.read_scope;
            record.host_turn = scope.host_turn;
            record.prediction_turn = scope.prediction_turn;
            record.update_turn = scope.update_turn;
            record.acknowledged_turn = scope.acknowledged_turn;
            record.phase = scope.phase;
        }
    }
}

void Handler::end_ambient_binding(void *weapon, const AmbientInvocation &invocation) noexcept {
    const ErrorGuard guard;
    if (!invocation.observed || !trace_.enabled()) {
        return;
    }

    auto record = invocation.record;
    record.after = ambient_binding(weapon);
    if (trace::ambient_binding_changed(record.before, record.after)) {
        // Allocate accepted ordering only when the paired observation is emitted, preserving sequence continuity.
        record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        write(record);
    }
}

void Handler::coverage() noexcept {
    const ErrorGuard guard;
    const auto health = trace_.health();
    write(trace::CoverageRecord{.header = header(nullptr, trace::RecordKind::Coverage),
                                .attempted = attempted_.load(std::memory_order_relaxed),
                                .bytes = bytes_.load(std::memory_order_relaxed),
                                .rejected = rejected_.load(std::memory_order_relaxed),
                                .scope_overflows = scope_overflows_.load(std::memory_order_relaxed),
                                .voice_overflows = voice_overflows_.load(std::memory_order_relaxed),
                                .accepted = health.accepted,
                                .written = health.written,
                                .dropped = health.dropped,
                                .output_failed = health.output_failed,
                                .file_limit_reached = health.file_limit_reached,
                                .hero_available = hero_ready_.load(std::memory_order_acquire),
                                .capture = static_cast<std::uint8_t>(settings_.capture),
                                .role = static_cast<std::uint8_t>(role_),
                                .suppressed_requests = suppressed_requests_.load(std::memory_order_relaxed),
                                .suppressed_routes = suppressed_routes_.load(std::memory_order_relaxed),
                                .suppressed_renderers = suppressed_renderers_.load(std::memory_order_relaxed),
                                .empty_packet_returns = empty_packet_returns_.load(std::memory_order_relaxed)});
}

void Handler::write_status(fc::StatusContext &context) const noexcept {
    const auto health = trace_.health();
    context.line("Records: {}; dropped: {}", health.written, health.dropped);

    if (const auto count = scope_overflows_.load(std::memory_order_relaxed); count != 0) {
        context.line("Scope overflows: {}", count);
    }
    if (const auto count = voice_overflows_.load(std::memory_order_relaxed); count != 0) {
        context.line("Voice binding gaps: {}", count);
    }
}

void Handler::write_bytes(std::span<const std::byte> bytes) noexcept {
    attempted_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
    if (!trace_.write(bytes)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Handler::flush_renderers() noexcept {
    for (auto &run : renderer_runs_) {
        if (run.record.header.object && run.record.callbacks) {
            run.record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(run.record);
            run.record.callbacks = 0;
        }
    }
}

void Handler::source_route(const fc::CpuContext &cpu) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    auto *physical = pointer(cpu.esi);
    // Retail stereo SetGain forwards to its embedded StreamMixer at +0xC, which owns the input packets.
    trace::BindingRecord record{.header = {.kind = trace::RecordKind::SourceRoute, .object = cpu.esi},
                                .sample = field<std::uint32_t>(physical, 0x7C),
                                .input_node = cpu.ecx + 0xCU,
                                .renderer = field<std::uint32_t>(physical, 0x1E0),
                                .channel = field<std::uint32_t>(pointer(cpu.esp), 0),
                                .accepted = true};
    // Retain an observed assignment identity; zero remains unknown when tracing began after assignment.
    for (const auto &binding : bindings_) {
        const auto generation = binding.generation.load(std::memory_order_acquire);
        if (generation && binding.physical.load(std::memory_order_acquire) == cpu.esi &&
            binding.generation.load(std::memory_order_acquire) == generation) {
            record.generation = generation;
            break;
        }
    }
    const auto now = ticks();
    const auto epoch = route_epoch_.load(std::memory_order_acquire);
    auto &run = route_runs_[(cpu.esi >> 4) % route_runs_.size()];
    if (run.summary.count && run.epoch == epoch && run.first.header.object == record.header.object &&
        run.first.generation == record.generation && run.first.sample == record.sample && run.first.input_node == record.input_node &&
        run.first.renderer == record.renderer && run.first.channel == record.channel) {
        ++run.summary.count;
        run.summary.last_ticks = now;
        suppressed_routes_.fetch_add(1, std::memory_order_relaxed);
        if (now - run.summary.first_ticks < counter_frequency_) {
            return;
        }
        run.summary.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        write(run.summary);
        run = {};
        return;
    }
    if (run.summary.count > 1) {
        run.summary.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        write(run.summary);
    }
    // A first/changed route is a nested native effect, even if its request starts no new voice.
    if (scopes_.request_depth) {
        materialize_requests();
    }
    record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    write(record);
    run = {.first = record,
           .summary = {.header = {.kind = trace::RecordKind::SourceRouteRun, .object = record.header.object},
                       .generation = record.generation,
                       .first_ticks = now,
                       .last_ticks = now,
                       .count = 1},
           .epoch = epoch};
}

void Handler::state(void *object, trace::RecordKind kind, std::uint8_t reason, bool argument, std::uint64_t caller) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    trace::StateRecord record{.header = {.kind = kind, .object = reinterpret_cast<std::uintptr_t>(object)},
                              .caller = caller,
                              .reason = reason,
                              .argument = argument};
    const bool controller = kind == trace::RecordKind::Controller;
    record.header.source = controller ? trace::SourceKind::Controlled : trace::SourceKind::Stream;
    record.position = controller ? field<std::uint16_t>(object, 0) : field<std::uint32_t>(object, 0x68);
    record.transport = controller ? field<std::uint8_t>(object, 2) : field<std::uint16_t>(object, 0x6C);
    if (!controller) {
        record.sample = field<std::uint32_t>(object, 0xF4);
    }
    if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
        const auto &request = scopes_.requests[scopes_.request_depth - 1];
        record.request = request.request;
        if (!record.caller) {
            record.caller = request.caller;
        }
    }
    if (controller && scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
        auto &request = scopes_.requests[scopes_.request_depth - 1];
        if (reason == 1 || reason == 2) {
            request.controller_before = static_cast<std::uint16_t>(record.position);
            request.controller_flags_before = static_cast<std::uint8_t>(record.transport);
            request.controller_reason = reason;
            request.stop_argument = argument;
        } else {
            request.controller_after = static_cast<std::uint16_t>(record.position);
            request.controller_flags_after = static_cast<std::uint8_t>(record.transport);
        }
        return;
    }
    record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    write(record);
}

void Handler::bind_layer(void *task, void *layer) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    auto *descriptor = pointer(field<std::uint32_t>(layer, 0));
    trace::BindingRecord record{.header = header(task, trace::RecordKind::LayeredBinding),
                                .generation = next_generation_.fetch_add(1, std::memory_order_relaxed),
                                .descriptor = reinterpret_cast<std::uintptr_t>(descriptor),
                                .definition_id = descriptor ? field<std::uint32_t>(descriptor, 4) : 0,
                                .delay = field<float>(task, 8),
                                .accepted = true};
    record.header.source = trace::SourceKind::Layered;
    if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
        record.request = scopes_.requests[scopes_.request_depth - 1].request;
    }
    write(record);
}

void Handler::bind_stream(void *descriptor, void *object, bool accepted) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    trace::BindingRecord record{.header = header(object, trace::RecordKind::StreamBinding),
                                .generation = accepted ? next_generation_.fetch_add(1, std::memory_order_relaxed) : 0,
                                .descriptor = reinterpret_cast<std::uintptr_t>(descriptor),
                                .sample = field<std::uint32_t>(object, 0xF4),
                                .input_node = reinterpret_cast<std::uintptr_t>(object) + 0xD81C0,
                                .definition_id = field<std::uint32_t>(descriptor, 0x10),
                                .accepted = accepted};
    record.header.source = trace::SourceKind::Stream;
    if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
        auto &request = scopes_.requests[scopes_.request_depth - 1];
        record.request = request.request;
        if (!accepted) {
            request.rejection = 6;
        }
    }
    write(record);
}

void Handler::stream_routes(void *object) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    // Connect uses the first opened stream's channel count and its fixed 0x2EC StreamRenderer array.
    auto *node = pointer(field<std::uint32_t>(object, 0xFC));
    auto *opened = pointer(field<std::uint32_t>(node, 4));
    const auto channels = field<std::uint32_t>(opened, 0x1C);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        auto *source = static_cast<std::byte *>(object) + 0x3608F4 + channel * 0x2EC;
        trace::BindingRecord record{.header = header(object, trace::RecordKind::StreamRoute),
                                    .sample = field<std::uint32_t>(object, 0xF4),
                                    .input_node = reinterpret_cast<std::uintptr_t>(source),
                                    .renderer = field<std::uint32_t>(source, 0xF8),
                                    .channel = channel,
                                    .accepted = true};
        record.header.source = trace::SourceKind::Stream;
        write(record);
    }
}

void Handler::native_api(void *wrapper, std::int32_t result, std::uint8_t operation, std::uint64_t caller) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    trace::ApiRecord record{.header = header(wrapper, trace::RecordKind::NativeApi),
                            .device = field<std::uint32_t>(wrapper, 0),
                            .caller = caller,
                            .result = result,
                            .operation = operation};
    if (renderer_) {
        record.renderer = renderer_->record.header.object;
    }
    if (scopes_.request_depth && scopes_.request_depth <= scopes_.requests.size()) {
        record.request = scopes_.requests[scopes_.request_depth - 1].request;
    }
    write(record);
}

void Handler::api_instruction(const fc::CpuContext &cpu, std::uint8_t operation) noexcept {
    const ErrorGuard guard;
    if (operation == 0) {
        native_api(device_play_, std::bit_cast<std::int32_t>(cpu.eax), 1, device_play_caller_);
    } else if (operation == 1) {
        // The shared continuation also follows the branch that skipped SetCurrentPosition.
        if (field<std::uint8_t>(pointer(cpu.ebp), 0x14)) {
            native_api(device_play_, std::bit_cast<std::int32_t>(cpu.eax), 3, device_play_caller_);
        }
    } else if (operation == 2 || operation == 3) {
        native_api(pointer(operation == 2 ? cpu.ebx + 0xF8 : cpu.esi), std::bit_cast<std::int32_t>(cpu.eax), 3);
    } else if (operation == 4) {
        flush_locked_ = std::bit_cast<std::int32_t>(cpu.eax) >= 0;
        native_api(pointer(cpu.ebx + 0xF8), std::bit_cast<std::int32_t>(cpu.eax), 4);
    } else if (flush_locked_) {
        native_api(pointer(cpu.ebx + 0xF8), std::bit_cast<std::int32_t>(cpu.eax), 5);
        flush_locked_ = false;
    }
}

trace::Header Handler::header(void *object, trace::RecordKind kind) noexcept {
    return {.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed),
            .kind = kind,
            .object = reinterpret_cast<std::uintptr_t>(object)};
}

void Handler::stream(void *object, trace::RecordKind kind, bool renderer, bool accepted) noexcept {
    const ErrorGuard guard;
    if (!trace_.enabled()) {
        return;
    }
    trace::StreamRecord record{.header = header(object, kind)};
    if (renderer) {
        route_epoch_.fetch_add(1, std::memory_order_release);
        flush_runs();
        flush_renderers();
        record.frequency = field<std::uint32_t>(object, 0x13C);
        record.channels = field<std::uint32_t>(object, 0x140);
        record.format = field<std::uint32_t>(object, 0x144);
        record.capacity_bytes = field<std::uint32_t>(object, 0x150);
        record.gain = field<std::int32_t>(object, 0x198);
        record.hardware_gain = (field<std::uint8_t>(object, 0x18C) & 0x10) != 0;
        record.accepted = accepted;
    } else {
        record.channels = field<std::uint8_t>(object, 0x8B);
        record.capacity_bytes = field<std::uint32_t>(object, 0x80);
        record.gain = field<std::int32_t>(object, 0x70);
    }
    write(record);
}

void Handler::begin_renderer(void *object, RendererInvocation &invocation) noexcept {
    const ErrorGuard guard;
    invocation.previous = renderer_;
    renderer_ = &invocation;
    invocation.record.start_ticks = 0;
    if (!trace_.enabled()) {
        return;
    }
    invocation.record = {.header = {.kind = trace::RecordKind::RendererUpdate, .object = reinterpret_cast<std::uintptr_t>(object)},
                         .start_ticks = ticks(),
                         .counter_frequency = counter_frequency_};
    auto &record = invocation.record;
    record.capacity_bytes = field<std::uint32_t>(object, 0x150);
    record.next_lock_before = field<std::uint32_t>(object, 0x15C);
    record.buffered_before = field<std::uint32_t>(object, 0x160);
    record.left_to_play_before = field<std::uint32_t>(object, 0x164);
    record.frequency = field<std::uint32_t>(object, 0x13C);
    record.channels = field<std::uint32_t>(object, 0x140);
    record.format = field<std::uint32_t>(object, 0x144);
    record.gain_current[0] = field<std::int32_t>(object, 0x198);
    record.gain_current[1] = field<std::int32_t>(object, 0x19C);
    record.hardware_gain = (field<std::uint8_t>(object, 0x18C) & 0x10) != 0;
    record.owner_iteration = owner_.iteration;
    record.device = field<std::uint32_t>(object, 0xF8);
    record.position_before = field<std::uint32_t>(object, 0x14);
    record.transport_before = field<std::uint16_t>(object, 0x18);
    record.input_node = field<std::uint32_t>(object, 0x20);
    record.input_channel = field<std::uint32_t>(object, 0x24);
    record.gain_target[0] = field<std::int32_t>(object, 0x190);
    record.gain_target[1] = field<std::int32_t>(object, 0x194);
    if (!owner_.first_callback) {
        owner_.first_callback = record.start_ticks;
    }
    ++owner_.renderer_callbacks;
}

void Handler::end_renderer(const RendererInvocation &invocation) noexcept {
    const ErrorGuard guard;
    renderer_ = invocation.previous;
    if (invocation.record.start_ticks) {
        auto record = invocation.record;
        record.duration_ticks = ticks() - record.start_ticks;
        auto *object = pointer(static_cast<std::uint32_t>(record.header.object));
        record.next_lock_after = field<std::uint32_t>(object, 0x15C);
        record.buffered_after = field<std::uint32_t>(object, 0x160);
        record.left_to_play_after = field<std::uint32_t>(object, 0x164);
        record.position_after = field<std::uint32_t>(object, 0x14);
        record.transport_after = field<std::uint16_t>(object, 0x18);
        owner_.last_callback = record.start_ticks + record.duration_ticks;
        const auto epoch = route_epoch_.load(std::memory_order_acquire);
        auto &run = renderer_runs_[(record.header.object >> 4) % renderer_runs_.size()];
        if (run.object != record.header.object || run.epoch != epoch) {
            if (run.record.header.object && run.record.callbacks) {
                run.record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
                write(run.record);
            }
            run = {.epoch = epoch, .object = record.header.object};
        }
        const auto previous_start = run.previous_start;
        run.previous_start = record.start_ticks;
        record.max_service_gap = previous_start ? record.start_ticks - previous_start : 0;
        record.gap_start_ticks = previous_start;
        record.gap_end_ticks = record.start_ticks;
        record.gap_position_ticks = record.position_ticks;
        record.gap_request_ticks = record.request_ticks;
        record.gap_lock_ticks = record.lock_ticks;
        record.gap_unlock_ticks = record.unlock_ticks;
        if (run.record.header.object && run.epoch == epoch && trace::same_idle_renderer(run.record, record)) {
            if (!run.record.callbacks) {
                run.record = record;
                run.record.callbacks = 0;
                run.record.max_duration = record.duration_ticks;
                run.record.duration_start_ticks = record.start_ticks;
                run.record.duration_position_ticks = record.position_ticks;
                run.record.duration_request_ticks = record.request_ticks;
                run.record.duration_lock_ticks = record.lock_ticks;
                run.record.duration_unlock_ticks = record.unlock_ticks;
            }
            const auto gap = record.max_service_gap;
            if (gap > run.record.max_service_gap) {
                run.record.max_service_gap = gap;
                run.record.gap_start_ticks = previous_start;
                run.record.gap_end_ticks = record.start_ticks;
                run.record.gap_position_ticks = record.position_ticks;
                run.record.gap_request_ticks = record.request_ticks;
                run.record.gap_lock_ticks = record.lock_ticks;
                run.record.gap_unlock_ticks = record.unlock_ticks;
            }
            if (record.duration_ticks > run.record.max_duration) {
                run.record.max_duration = record.duration_ticks;
                run.record.duration_start_ticks = record.start_ticks;
                run.record.duration_position_ticks = record.position_ticks;
                run.record.duration_request_ticks = record.request_ticks;
                run.record.duration_lock_ticks = record.lock_ticks;
                run.record.duration_unlock_ticks = record.unlock_ticks;
            }
            ++run.record.callbacks;
            run.record.last_start_ticks = record.start_ticks;
            run.record.last_end_ticks = record.start_ticks + record.duration_ticks;
            run.record.last_owner_iteration = record.owner_iteration;
            ++owner_.idle_callbacks;
            suppressed_renderers_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (run.record.header.object && run.record.callbacks) {
            run.record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(run.record);
        }
        run.record = {};
        if (trace::idle_renderer(record)) {
            record.last_start_ticks = record.start_ticks;
            record.last_end_ticks = record.start_ticks + record.duration_ticks;
            record.last_owner_iteration = record.owner_iteration;
            record.max_duration = record.duration_ticks;
            record.duration_start_ticks = record.start_ticks;
            record.duration_position_ticks = record.position_ticks;
            record.duration_request_ticks = record.request_ticks;
            record.duration_lock_ticks = record.lock_ticks;
            record.duration_unlock_ticks = record.unlock_ticks;
            record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(record);
            run.record = record;
            run.record.callbacks = 0;
        } else {
            record.header.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
            write(record);
        }
    }
}

void Handler::renderer_instruction(const fc::CpuContext &cpu, std::uint8_t boundary) noexcept {
    const ErrorGuard guard;
    if (!renderer_ || renderer_->record.header.object != cpu.esi) {
        return;
    }
    auto &record = renderer_->record;
    switch (boundary) {
    case Position:
        record.position_observed = true;
        record.position_result = std::bit_cast<std::int32_t>(cpu.eax);
        record.position_ticks = settings_.capture == CaptureMode::Full ? ticks() : 0;
        if (record.position_result >= 0) {
            record.play_cursor = field<std::uint32_t>(pointer(cpu.ebp), -0x10);
            record.write_cursor = field<std::uint32_t>(pointer(cpu.ebp), -0x18);
            record.raw_play_cursor = record.play_cursor;
            record.raw_write_cursor = record.write_cursor;
            record.raw_cursors_observed = true;
        }
        break;
    case Consumed:
        record.consumed_bytes = cpu.eax;
        record.play_cursor = cpu.ecx;
        record.write_cursor = cpu.edx;
        record.cursors_observed = true;
        break;
    case CursorReset:
        ++record.cursor_resets;
        break;
    case Requested:
        record.requested_bytes = cpu.edx;
        record.request_ticks = settings_.capture == CaptureMode::Full ? ticks() : 0;
        break;
    case Lock:
        record.lock_observed = true;
        record.lock_ticks = settings_.capture == CaptureMode::Full ? ticks() : 0;
        record.lock_result = std::bit_cast<std::int32_t>(cpu.eax);
        break;
    case Unlock:
        // The zero-request branch reaches this site without calling Unlock; failed Lock exits earlier.
        if (record.lock_observed && record.lock_result >= 0) {
            record.unlock_observed = true;
            record.unlock_ticks = settings_.capture == CaptureMode::Full ? ticks() : 0;
            record.unlock_result = std::bit_cast<std::int32_t>(cpu.eax);
        }
        break;
    case Exhausted:
        ++record.exhausted_writes;
        break;
    case ZeroFill:
        record.zero_fill_bytes += field<std::uint32_t>(pointer(cpu.ebp), 0x0C);
        break;
    }
}

} // namespace fc_plugins::sound_diagnostics
