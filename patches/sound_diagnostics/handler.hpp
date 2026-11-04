#pragma once

#include "layout.hpp"
#include "schema.hpp"
#include "footsteps.hpp"
#include "api.hpp"
#include "../hero_diagnostics/api.hpp"

#include <fc/plugin.hpp>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>

namespace fc_plugins::sound_diagnostics {

// Values are stored in trace metadata.
enum class CaptureMode : std::uint8_t {
    Standard = 0,
    Full = 2
};

struct Settings {
    CaptureMode capture{CaptureMode::Standard};
};

// Observes native sound synchronously and retains copied identities for lifecycle correlation.
class Handler {
  public:
    using Settings = sound_diagnostics::Settings;

    // Retains immutable capture settings before the native plan is constructed.
    Handler(const Settings &settings, fc::Role role = fc::Role::Client) noexcept : settings_(settings), role_(role) {}

    // Declares passive mixer, renderer and lifecycle observations for the selected retail image.
    void plan(fc::Plan &plan);
    // Creates the optional trace channel and publishes the foot-height observation callback.
    fc::Result<> prepare(fc::PrepareContext &context);
    // Flushes game-thread recurring polls at control checkpoints.
    void update(const fc::UpdateContext &) noexcept;
    // Reports records accepted by ETW, records dropped by the channel and any sound collection gaps.
    void write_status(fc::StatusContext &context) const noexcept;

  private:
    Settings settings_;
    fc::Role role_;

    struct RendererInvocation {
        RendererInvocation *previous{};
        trace::RendererUpdateRecord record{};
    };

    struct AmbientInvocation {
        trace::AmbientBindingRecord record{};
        bool observed{};
    };

    struct BindingInvocation {
        BindingInvocation *previous{};
        trace::BindingMutationRecord record{};
        bool observed{};
    };

    // Native function observation owns this stack state; the pointer restores enclosing producer identity.
    struct ProducerInvocation {
        ProducerInvocation *previous{};
        trace::ProducerRecord record{};
        std::uint64_t previous_cause{};
        bool observed{};
    };

    struct FootSoundInvocation {
        FootSoundInvocation *previous{};
        trace::FootstepRecord record{};
        std::uint64_t previous_cause{};
        bool observed{};
    };

    // Declares qualified height gates and sound selection, retaining silent decisions only in bounded memory.
    void plan_footsteps(fc::Plan &plan, const Layout &layout);
    // Allows the continuity owner to skip optional copies while tracing or Hero subject tracking is unavailable.
    std::uint8_t foot_maintenance_enabled() noexcept;
    // Copies the maintenance owner's result and updates retained writer and arm provenance.
    void foot_maintenance(void *soldier, const FootMaintenance *outcome) noexcept;
    // Finds only already registered selected Hero weapons, clearing retained evidence on identity reuse.
    FootHistory *foot_history(void *soldier) noexcept;
    // Copies actual height comparisons and the live animator clip operands.
    void foot_instruction(const fc::CpuContext &cpu, std::uint8_t operation, std::uint32_t foot) noexcept;
    static thread_local FootInvocation *foot_;
    static thread_local FootSoundInvocation *foot_sound_;
    static thread_local std::array<FootHistory, 64> foot_histories_;

    struct VoiceBinding {
        std::atomic<std::uintptr_t> object{};
        std::atomic<std::uint64_t> generation{};
        std::atomic<std::uint64_t> request{};
        std::atomic<std::uintptr_t> physical{};
    };

    struct PacketGap {
        trace::PacketGapRecord record{};
        bool pending{};
    };

    struct UiEventScope {
        trace::UiEventRecord record{};
        bool emitted{};
    };

    struct UiCallbackScope {
        trace::UiCallbackRecord record{};
        bool emitted{};
        std::uint32_t completion{};
        std::uint32_t frame{};
    };

    // Native Play may call user callbacks; bounded TLS scopes disclose any unrecorded nesting.
    struct NativeScopes {
        std::array<trace::RequestRecord, 16> requests{};
        std::array<bool, 16> emitted{}, effects{};
        std::array<trace::VoiceRecord, 16> voices{};
        std::array<std::uint32_t, 16> stream_start_frames{}; // Successful due requests remain open until their native frame completes.
        std::uint32_t request_depth{};
        std::uint32_t voice_depth{};
        std::uint32_t stop_depth{};
        std::array<UiEventScope, 16> ui_events{};
        std::array<UiCallbackScope, 16> ui_callbacks{};
        std::uint32_t ui_event_depth{};
        std::uint32_t ui_callback_depth{};
        std::uint64_t ui_dispatch{};
        std::uint64_t ui_dispatch_caller{};
        std::uint64_t control_cause{};
    };

    // Installs the qualified descriptor writers and native producer decisions using existing cause IDs.
    void plan_causes(fc::Plan &plan, const Layout &layout);
    // Rejects unrelated wrappers before reading their native descriptor or property source.
    void begin_binding(void *wrapper, void *source, trace::BindingOperation operation, std::uintptr_t caller, std::uint32_t rva,
                       BindingInvocation &invocation) noexcept;
    // Copies both completed endpoints, including an assignment's native deferred source resolution.
    void end_binding(void *wrapper, const BindingInvocation &invocation) noexcept;
    // Retains actual producer inputs and opens a nested existing ControlCause identity.
    void begin_producer(void *object, trace::NativeProducer producer, std::uintptr_t caller, std::uint32_t rva,
                        std::array<std::uint32_t, 4> arguments, ProducerInvocation &invocation) noexcept;
    // Emits result state and restores the enclosing producer/cause even after nested native calls.
    void end_producer(void *object, std::uint32_t result, const ProducerInvocation &invocation) noexcept;
    // Records actual commit/collision results or the chosen sound wrapper immediately at the native decision.
    void producer_instruction(const fc::CpuContext &cpu, trace::NativeProducer producer, std::uint8_t stage, std::uint16_t branch,
                              std::uint32_t rva) noexcept;
    static thread_local ProducerInvocation *producer_;
    static thread_local BindingInvocation *binding_;

    // Observes the native owner semaphore and Sleep boundaries without instrumenting Win32 globally.
    void owner_instruction(const fc::CpuContext &context, std::uint8_t boundary) noexcept;
    // Opens/closes a request and records its native route; a null fire-and-forget result is never a failure.
    void begin_request(void *object, void *descriptor, void *position, trace::SourceKind source, void *callback = nullptr,
                       void *data = nullptr, void *source_object = nullptr, std::uintptr_t caller = 0,
                       std::uint8_t native_flags = 0) noexcept;
    void end_request() noexcept;
    // Materializes pending ancestors before an actual nested effect references their existing IDs.
    void materialize_requests() noexcept;
    // Closes recurring refusal and route summaries on this native thread without reading retired objects.
    void flush_runs() noexcept;
    // Emits owner-thread no-work intervals at existing service/lifecycle checkpoints.
    void flush_renderers() noexcept;
    // Records the actual rejection branch before its jump; zero never means a null-return failure.
    void request_gate(const fc::CpuContext &context, std::uint8_t boundary) noexcept;
    // Captures native voice state and correlates each binding with its ordinary request.
    trace::VoiceRecord voice_record(void *voice, trace::RecordKind kind) noexcept;
    void bind_voice(void *voice) noexcept;
    void begin_voice(void *voice, trace::RecordKind kind, bool restart = false, std::uintptr_t caller = 0) noexcept;
    void end_voice(void *voice, trace::RecordKind kind) noexcept;
    // Records fixed native control points and asynchronous binding identities for offline generation joins.
    void state(void *object, trace::RecordKind kind, std::uint8_t reason, bool argument = false, std::uint64_t caller = 0) noexcept;
    void bind_layer(void *task, void *layer) noexcept;
    void bind_stream(void *descriptor, void *object, bool accepted) noexcept;
    void stream_routes(void *object) noexcept;
    // Captures existing device outcomes with the current request/renderer scope.
    void native_api(void *wrapper, std::int32_t result, std::uint8_t operation, std::uint64_t caller = 0) noexcept;
    void api_instruction(const fc::CpuContext &context, std::uint8_t operation) noexcept;
    // Joins a physical voice to the actual mixer and input slot passed to native SetGain.
    void source_route(const fc::CpuContext &context) noexcept;
    // Retains event/callback identities for sound requests, resets and queued screen transitions.
    void begin_ui_event(const fc::CpuContext &context) noexcept;
    void end_ui_event() noexcept;
    void begin_ui_callback(const fc::CpuContext &context, std::uint32_t index, std::uint32_t rva) noexcept;
    void end_ui_callback(const fc::CpuContext &context, std::uint32_t index) noexcept;
    // Copies the current UI identities and emits their descriptions before a record refers to them.
    void attach_ui(std::uint64_t &event, std::uint64_t &callback) noexcept;
    // Records each native screen transition's producer before asynchronous dispatch can lose its caller.
    void queue_ui_event(const fc::CpuContext &context) noexcept;
    // Opens a native reset/stop cause for nested request and voice records; the function observer restores its parent.
    void begin_control_cause(trace::RecordKind kind, std::uintptr_t caller) noexcept;
    // Copies the health warning operands only when the native controller actually starts or stops.
    void health_decision(const fc::CpuContext &context, bool play, std::uint32_t rva) noexcept;
    // Tracks physical reuse without erasing immutable packet records emitted for the previous voice.
    void assign_physical(const trace::VoiceRecord &record) noexcept;
    // Tracks ordinary empty-source gaps and records the returned packet that recovers a gap.
    void packet(void *resampler, void *packet) noexcept;
    // Records actual source packets and native end flags before PacketComplete updates their headers.
    void source_end(const fc::CpuContext &context) noexcept;
    // Retains exact null-return counts without submitting a record for each empty poll.
    void packet_gap(void *resampler, const trace::PacketRecord &owner, bool available) noexcept;
    // Closes owner-thread gaps after atomic binding publication proves retirement or replacement.
    void finish_packet_gaps() noexcept;
    // Emits a completed gap using retained values, without dereferencing a retired source.
    void end_packet_gap(PacketGap &gap, std::uint8_t stage) noexcept;
    void voice_decision(const fc::CpuContext &context, trace::VoiceDecision decision, std::uint32_t rva) noexcept;
    // Copies the actual saber mask/property decision and its restoration scope before the controller call.
    void saber_decision(const fc::CpuContext &context, trace::SaberDecision decision, std::uint32_t rva) noexcept;
    // Retains borrowed saber bindings across one live Read/control call, emitting only actual binding changes.
    void begin_ambient_binding(void *weapon, std::uintptr_t caller, std::uint32_t rva, trace::AmbientBoundary boundary,
                               AmbientInvocation &invocation) noexcept;
    void end_ambient_binding(void *weapon, const AmbientInvocation &invocation) noexcept;
    // Submits records and counts queue failures without profiling the recorder.
    void write_bytes(std::span<const std::byte> bytes) noexcept;
    // Publishes submission failures and scope/table coverage limits.
    void coverage() noexcept;

    // Copies initialized stream metadata at a qualified synchronous lifetime or format boundary.
    void stream(void *object, trace::RecordKind kind, bool renderer, bool accepted = false) noexcept;
    // Copies one native update's metadata, timing, nested writes and device outcomes into a trace event.
    void begin_renderer(void *renderer, RendererInvocation &invocation) noexcept;
    void end_renderer(const RendererInvocation &invocation) noexcept;
    // Records a named update instruction's registers/locals without changing the original result.
    void renderer_instruction(const fc::CpuContext &context, std::uint8_t boundary) noexcept;

    // Submits one self-contained event; callers preserve OS error around diagnostic work.
    template <class Record> void write(const Record &record) noexcept {
        const auto kind = record.header.kind;
        if (kind != trace::RecordKind::Request && kind != trace::RecordKind::RequestResult && kind != trace::RecordKind::RequestRun &&
            kind != trace::RecordKind::Coverage && kind != trace::RecordKind::OwnerIteration && kind != trace::RecordKind::OwnerProgress &&
            kind != trace::RecordKind::RendererUpdate && kind != trace::RecordKind::SourceRoute &&
            kind != trace::RecordKind::SourceRouteRun) {
            materialize_requests();
        }
        write_bytes(std::as_bytes(std::span{&record, 1}));
    }

    // Assigns an attempt sequence and copies the borrowed native address as an identity.
    trace::Header header(void *object, trace::RecordKind kind) noexcept;

    struct RequestRun {
        trace::RequestRecord first{};
        trace::RequestRunRecord summary{};
    };

    struct RendererRun {
        trace::RendererUpdateRecord record{};
        std::uint64_t epoch{}, object{}, previous_start{};
    };

    struct RouteRun {
        trace::BindingRecord first{};
        trace::SourceRouteRunRecord summary{};
        std::uint64_t epoch{};
    };

    static thread_local std::array<RequestRun, 64> request_runs_;
    static thread_local std::array<RendererRun, 128> renderer_runs_;
    static thread_local std::array<RouteRun, 128> route_runs_;
    // Cross-thread lifecycle boundaries invalidate retained pointer tuples without borrowing objects.
    std::atomic<std::uint64_t> route_epoch_{1};
    std::atomic<std::uint64_t> suppressed_requests_{}, suppressed_routes_{}, suppressed_renderers_{};
    static thread_local RendererInvocation *renderer_;
    static thread_local RendererInvocation renderer_storage_;
    static thread_local trace::OwnerRecord owner_;
    static thread_local trace::OwnerProgressRecord owner_progress_;
    static thread_local NativeScopes scopes_;
    static thread_local void *device_play_;
    static thread_local std::uintptr_t device_play_caller_;
    static thread_local bool flush_locked_;
    static thread_local std::array<PacketGap, 128> packet_gaps_; // Qualified physical pool has 64 main/LFE pairs.
    static thread_local std::uint32_t pending_packet_gaps_;
    std::array<VoiceBinding, 64> bindings_{}; // Qualified ordinary pool has 64 entries of 0xC8 bytes.
    std::atomic<std::uint64_t> next_request_{1};
    std::atomic<std::uint64_t> next_generation_{1};
    std::atomic<std::uint64_t> next_iteration_{1};
    std::atomic<std::uint64_t> next_ui_dispatch_{1};
    std::atomic<std::uint64_t> next_ui_event_{1};
    std::atomic<std::uint64_t> next_ui_callback_{1};
    std::atomic<std::uint64_t> next_packet_{1};
    std::atomic<std::uint64_t> next_control_cause_{1};
    std::atomic<std::uint64_t> attempted_{};
    std::atomic<std::uint64_t> bytes_{};
    std::atomic<std::uint64_t> rejected_{};
    std::atomic<std::uint64_t> scope_overflows_{};
    std::atomic<std::uint64_t> voice_overflows_{};
    std::atomic<std::uint64_t> empty_packet_returns_{};
    hero_diagnostics::HeroDiagnosticsApi hero_{};
    std::atomic<bool> hero_ready_{};
    fc::TraceChannel trace_;
    std::atomic<std::uint32_t> next_sequence_{1};
    std::uint64_t counter_frequency_{};
    // Resolve a qualified code anchor once during Prepare; instruction EIP can point into the hook trampoline.
    fc::Memory<const std::byte> image_anchor_;
    std::uint32_t image_anchor_rva_{};
    std::uintptr_t image_base_{};
};

} // namespace fc_plugins::sound_diagnostics
