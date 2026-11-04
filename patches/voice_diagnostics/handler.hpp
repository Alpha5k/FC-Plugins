#pragma once

#include "api.hpp"
#include "hud_layout.hpp"
#include "hud_schema.hpp"
#include "layout.hpp"
#include "schema.hpp"

#include <Windows.h>

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <atomic>
#include <chrono>
#include <array>
#include <cstdint>

namespace fc_plugins::voice_diagnostics {

// Values are stored in trace metadata.
enum class CaptureMode : std::uint8_t {
    Standard = 0,
    Full = 2
};

struct Settings {
    CaptureMode capture{CaptureMode::Standard};
};

// Records native audio, FC voice state and the game decisions that control speaking indicators.
class Handler {
  public:
    using Settings = voice_diagnostics::Settings;

    // Retains immutable capture settings before the native plan is constructed.
    Handler(const Settings &settings, fc::Role role = fc::Role::Client) noexcept : settings_(settings), role_(role) {}

    // Declares passive observations of native audio, HUD updates and rendering.
    void plan(fc::Plan &plan);

    // Creates the optional trace channel and publishes the passive recording contract.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Emits periodic coverage and loss counters even when the game never reaches a HUD callback.
    void update(const fc::UpdateContext &) noexcept;

    // Reports records accepted by ETW and records dropped by the channel.
    void write_status(fc::StatusContext &context) const noexcept;

  private:
    Settings settings_;
    fc::Role role_;

    // Reports the current immutable capture level to the optional producer before it copies observations.
    std::uint8_t capture() noexcept;

    struct StatusInvocation {
        StatusInvocation *previous{};
        trace::HudStatusRecord record{};
    };

    struct HudInvocation {
        HudInvocation *previous{};
        trace::HudScanRecord record{};
    };

    struct DrawInvocation {
        DrawInvocation *previous{};
        void *group{};
    };

    // Declares passive HUD and render probes using the selected retail layout.
    void plan_hud(fc::Plan &plan);

    // Resolves fixed native globals before callbacks can run.
    fc::Result<> prepare_hud();

    // Copies one row while its native object is owned by the current game callback.
    void observe_row(void *group, std::uint8_t row, std::uint8_t phase, const float *matrix = nullptr,
                     const std::uint8_t *color = nullptr) noexcept;

    // Records one actual UI result and links it to an enclosing HUD update when present.
    void observe_ui(const UiObservation *observation) noexcept;

    // Records the producer's full accepted state and confirms that the optional API is connected.
    void observe_view(const ViewObservation *observation) noexcept;

    // Copies producer decisions and links activity to the current HUD frame when present.
    void observe_activity(const ActivityObservation *observation) noexcept;

    // Copies the network candidate's authentication and ownership verdict.
    void observe_admission(const AdmissionObservation *observation) noexcept;

    // Copies the native sequence operands and admission verdict.
    void observe_sequence(const SequenceObservation *observation) noexcept;

    // Copies one receive-owner reset or local queue outcome.
    void observe_owner(const OwnerObservation *observation) noexcept;

    // Identifies only direct text children of the currently drawing voice group.
    static int drawing_row(const void *text) noexcept;

    template <class T> T hud_global(HudGlobal index) const noexcept {
        return fc::read_field<T>(hud_data_[static_cast<std::size_t>(index)], 0);
    }

    // Emits changed native HUD decisions and the exact count/range of identical repeats.
    template <class Record> void write_hud(Record record, std::size_t index = 0) noexcept {
        struct Cached {
            Record record{};
            std::uint32_t generation{};
            bool valid{};
        };

        static thread_local std::array<Cached, 128> cached{};
        if (!trace_.enabled()) {
            cached = {};
            return;
        }
        const auto generation = hud_generation_.load(std::memory_order_relaxed);
        if (index == cached.size()) {
            for (auto &entry : cached) {
                if (entry.valid && entry.record.observations) {
                    entry.record.header = header(entry.record.header.kind);
                    write(entry.record);
                }
                entry = {};
            }
            return;
        }
        const auto now = GetTickCount64();
        auto &entry = cached[index % cached.size()];
        auto key = record;
        auto previous = entry.record;
        key.header.sequence = previous.header.sequence = 0;
        key.first_ms = previous.first_ms = key.last_ms = previous.last_ms = 0;
        key.observations = previous.observations = 0;
        key.last_clock = previous.last_clock = 0;
        if constexpr (requires { key.frame; }) {
            key.frame = previous.frame = 0;
        }
        if constexpr (requires { key.hud_frame; }) {
            key.hud_frame = previous.hud_frame = 0;
        }
        if constexpr (requires { key.delta; }) {
            key.delta = previous.delta = 0;
        }
        if constexpr (requires { key.now; }) {
            key.now = previous.now = 0;
        }
        if constexpr (requires { key.value.now; }) {
            key.value.now = previous.value.now = 0;
        }
        if (entry.valid && entry.generation == generation && key == previous) {
            if (!entry.record.observations) {
                entry.record = record;
                entry.record.first_ms = now;
                entry.record.observations = 0;
            }
            ++entry.record.observations;
            entry.record.last_ms = now;
            if constexpr (requires { record.now; }) {
                entry.record.last_clock = record.now;
            }
            if constexpr (requires { record.value.now; }) {
                entry.record.last_clock = record.value.now;
            }
            suppressed_hud_.fetch_add(1, std::memory_order_relaxed);
            if (now - entry.record.first_ms < 1000) {
                return;
            }
            entry.record.header = header(record.header.kind);
            write(entry.record);
            entry.record.observations = 0;
            return;
        }
        if (entry.valid && entry.record.observations) {
            entry.record.header = header(entry.record.header.kind);
            write(entry.record);
        }
        record.first_ms = record.last_ms = now;
        record.observations = 1;
        if constexpr (requires { record.now; }) {
            record.last_clock = record.now;
        }
        if constexpr (requires { record.value.now; }) {
            record.last_clock = record.value.now;
        }
        record.header = header(record.header.kind);
        write(record);
        entry = {.record = record, .generation = generation, .valid = true};
        entry.record.observations = 0;
    }

    std::atomic<std::uint32_t> hud_generation_{1};
    std::atomic<std::uint64_t> suppressed_hud_{};
    std::atomic<bool> owner_observed_{};

    static thread_local StatusInvocation *current_status_;
    static thread_local HudInvocation *current_hud_;
    static thread_local DrawInvocation *current_draw_;
    std::array<fc::Memory<const std::byte>, static_cast<std::size_t>(HudGlobal::Count)> hud_memory_;
    std::array<const std::byte *, static_cast<std::size_t>(HudGlobal::Count)> hud_data_{};
    std::atomic<std::uint32_t> next_hud_frame_{1};
    std::atomic<std::uint32_t> status_updates_{}, hud_updates_{}, binds_{}, renders_{}, text_draws_{}, submissions_{}, glyph_queries_{},
        views_{}, constructors_{};
    std::atomic<std::uint64_t> write_failures_{};
    std::chrono::steady_clock::time_point next_health_{};

    struct PayloadInfo {
        std::uint32_t byte_size{};
        trace::PayloadKind kind{trace::PayloadKind::Unavailable};
        trace::RecoveryMode recovery{trace::RecoveryMode::None};
        std::uint8_t discontinuity{}; // Raw descriptor flag byte; native discontinuity is bit zero.
        std::uint8_t version{};
        std::uint8_t flags{};
    };

    // Runtime-owned paired observer state links nested observations and restores an enclosing relay afterward.
    struct RelayInvocation {
        RelayInvocation *previous{};
        std::uint32_t id{};
        std::int32_t method{-1};
        std::uint8_t sender{};
        std::uint8_t node_count{};
        trace::RelayRecipientsRecord recipients{};
    };

    // Saves descriptor scalars before DecodeStream; nested decoder callbacks update only these saved values.
    struct QueueInvocation {
        QueueInvocation *previous{};
        trace::StreamQueueRecord record{};
    };

    // Restores the enclosing admitted frame when native local delivery reenters.
    struct IntakeInvocation {
        IntakeInvocation *previous{};
        std::uint32_t id{};
        std::uint16_t sequence{0xFFFF};
    };

    // FlushPacket retains its sender table while running; only the paired after observer borrows it again.
    struct PendingFlushInvocation {
        void *queue{};
        trace::PendingQueueFlushRecord record{};
    };

    // Installs this invocation in TLS and records its borrowed payload before the native relay runs.
    void begin_relay(AudioPacket *packet, std::uint8_t sender, std::uint16_t voice_sequence, RelayInvocation &invocation) noexcept;

    // Restores the enclosing relay invocation after the native call returns, even when tracing is disabled.
    void end_relay(const RelayInvocation &invocation) noexcept;

    // Records the native transport method inside the current relay.
    void observe_relay_method(std::int32_t method) noexcept;

    // Records the native transport recipient range inside the current relay.
    void observe_relay_node_count(std::uint8_t count) noexcept;

    // Records the recipient decision after VoiceChat's routing modifier has run.
    void observe_recipient(std::uint8_t destination, bool selected) noexcept;

    // Merges the producer's already-evaluated native/FC policy into the current relay summary.
    void observe_recipient_policy(const RecipientObservation *observation) noexcept;

    // Records one actual selected-node enqueue with the current admitted-wire identity when available.
    void observe_selected_enqueue(const EnqueueObservation *observation) noexcept;

    // Saves native retained-slot identity before decoder intake; temporary flush descriptors retain an unknown slot.
    void begin_queue(void *node, std::uint8_t sender, AudioPacket *packet, QueueInvocation &invocation) noexcept;

    // Emits one queue attempt with its nested decoder outcomes and restores the enclosing invocation.
    void end_queue(bool accepted, const QueueInvocation &invocation) noexcept;

    // Ends node queue identity after native teardown using saved numeric addresses and invalidates jitter change caches.
    void observe_node_close(trace::NodeClosedRecord record) noexcept;

    // Records the exact retiring descriptor and native scalar policy immediately before native clears the slot.
    void observe_queue_retired(const fc::CpuContext &cpu) noexcept;

    // Records whether the native relay obtained a carrier packet and attempted sending to one destination.
    void observe_relay_build(AudioPacket *packet, std::uint8_t destination, bool built) noexcept;

    // Records native local/network submission and its enclosing relay ID; acceptance precedes remote receipt.
    void observe_wire_send(void *packet, std::uint8_t destination, std::uint32_t header_bytes, bool accepted) noexcept;

    // Records native decoder intake or flush acceptance and the borrowed prefix; codec outcomes and audible playback are separate.
    void observe_decoder_queue(void *decoder, std::uint32_t stream, AudioPacket *packet, bool accepted) noexcept;

    // Records target changes and repeats the initial target after any observed native node close.
    void observe_jitter_target(void *node, std::uint8_t stream, std::uint32_t target_packets) noexcept;

    // Copies the producer control decision into a bounded trace record.
    void observe_feature_state(const StateObservation *observation) noexcept;

    // Copies one completed or failed producer codec outcome.
    void observe_codec_outcome(const CodecObservation *observation) noexcept;

    // Classifies the synchronous descriptor and at most eight payload bytes; prefix recognition does not validate a frame.
    static PayloadInfo inspect_payload(const AudioPacket *packet) noexcept;

    // Converts the full native pointer value to a trace ID without extending the object's lifetime.
    static std::uint64_t pointer_id(const void *pointer) noexcept;

    // Assigns a wrapping process-local sequence to an attempted record, including attempts later dropped by FC.
    trace::RecordHeader header(trace::RecordKind kind) noexcept;

    // Submits one complete event while preserving last error; FC may drop it on queue contention or insufficient space.
    template <class Record> void write(Record &record) noexcept {
        if (!trace_.enabled()) {
            return;
        }

        const auto error = GetLastError();
        if (!trace_.write(record)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        SetLastError(error);
    }

    static thread_local RelayInvocation *current_relay_;
    static thread_local QueueInvocation *current_queue_;
    static thread_local IntakeInvocation *current_intake_;
    static thread_local std::uint16_t pending_sequence_;
    static thread_local std::uint32_t close_depth_;

    fc::TraceChannel trace_;
    std::atomic<std::uint32_t> next_sequence_{1};
    std::atomic<std::uint32_t> next_relay_{1};
    std::atomic<std::uint32_t> next_intake_{1};
    std::atomic<std::uint32_t> jitter_generation_{1};
};

} // namespace fc_plugins::voice_diagnostics
