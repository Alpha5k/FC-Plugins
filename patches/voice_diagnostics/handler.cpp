#include "handler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

namespace fc_plugins::voice_diagnostics {

thread_local Handler::RelayInvocation *Handler::current_relay_{};
thread_local Handler::QueueInvocation *Handler::current_queue_{};
thread_local Handler::IntakeInvocation *Handler::current_intake_{};
thread_local std::uint16_t Handler::pending_sequence_{0xFFFF};
thread_local std::uint32_t Handler::close_depth_{};

void Handler::plan(fc::Plan &plan) {
    if (role_ == fc::Role::Client) {
        plan_hud(plan);
    }
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Native intake dequeues from local loopback queues before checking the network carrier.
    plan.observe(layout->receive_packet, fc::after([this](void *packet) noexcept {
                     if (!trace_.enabled() || packet == nullptr) {
                         return;
                     }

                     trace::WireIntakeRecord record{
                         .header = header(trace::RecordKind::WireIntake),
                         .packet = pointer_id(packet),
                     };
                     write(record);
                 }));

    plan.observe_call(layout->read_header_call, fc::after([this](void *, void *packet, bool accepted) noexcept {
                          if (!trace_.enabled()) {
                              return;
                          }

                          trace::HeaderReadRecord record{
                              .header = header(trace::RecordKind::HeaderRead),
                              .packet = pointer_id(packet),
                              .accepted = static_cast<std::uint8_t>(accepted),
                          };
                          write(record);
                      }));

    // Native Update has admitted the header and retains its parsed sequence in this frame until local delivery returns.
    plan.observe_instruction(layout->admitted_identity, fc::before([](const fc::CpuContext &cpu) noexcept {
                                 pending_sequence_ = fc::read_field<std::uint16_t>(reinterpret_cast<void *>(cpu.ebp - 0xC), 0) & 0x3FF;
                             }));

    // This call-site result covers all VoiceChat recovery deliveries and reports remaining recipients, including zero on consumption.
    plan.observe_call<IntakeInvocation>(
        layout->receive_audio_call,
        fc::before([this](std::uint8_t, AudioPacket *, std::uint8_t, std::uint16_t, IntakeInvocation &invocation) noexcept {
            invocation.previous = current_intake_;
            invocation.sequence = pending_sequence_;
            invocation.id = next_intake_.fetch_add(1, std::memory_order_relaxed);
            current_intake_ = &invocation;
            pending_sequence_ = 0xFFFF;
        }),
        fc::after([this](std::uint8_t sender, AudioPacket *packet, std::uint8_t relay, std::uint16_t missing_packets,
                         std::uint8_t remaining_recipients, const IntakeInvocation &invocation) noexcept {
            current_intake_ = invocation.previous;
            if (!trace_.enabled()) {
                return;
            }

            const auto payload = inspect_payload(packet);
            trace::AudioIntakeRecord record{
                .header = header(trace::RecordKind::AudioIntake),
                .packet = pointer_id(packet),
                .payload_bytes = payload.byte_size,
                .missing_packets = missing_packets,
                .sender = sender,
                .relay_requested = relay,
                .remaining_recipients = remaining_recipients,
                .payload_kind = payload.kind,
                .recovery = payload.recovery,
                .discontinuity = payload.discontinuity,
                .frame_version = payload.version,
                .frame_flags = payload.flags,
                .intake = invocation.id,
                .voice_sequence = invocation.sequence,
            };
            write(record);
        }));

    // A paired invocation gives nested observations one relay ID; TLS also preserves an enclosing relay during recursion.
    plan.observe<RelayInvocation>(
        layout->relay_audio,
        fc::before([this](AudioPacket *packet, std::uint8_t sender, std::uint16_t voice_sequence, RelayInvocation &invocation) noexcept {
            begin_relay(packet, sender, voice_sequence, invocation);
        }),
        fc::after([this](AudioPacket *, std::uint8_t, std::uint16_t, const RelayInvocation &invocation) noexcept {
            end_relay(invocation);
        }));

    // Every relay queries its method. The node-count, recipient and build sites below cover method 4 (server relay).
    plan.observe_call(layout->relay_method_type_call, fc::after([this](void *, std::int32_t method) noexcept {
                          observe_relay_method(method);
                      }));

    plan.observe_call(layout->relay_node_count_call, fc::after([this](void *, std::uint8_t count) noexcept {
                          observe_relay_node_count(count);
                      }));

    // The call-site after observer sees the VoiceChat modifier's result, including its native recipient lookup.
    plan.observe_call(layout->relay_recipient_call, fc::after([this](void *, std::uint8_t destination, bool selected) noexcept {
                          observe_recipient(destination, selected);
                      }));

    plan.observe_call(layout->send_relay_call,
                      fc::after([this](AudioPacket *packet, std::uint8_t destination, float *, bool built) noexcept {
                          observe_relay_build(packet, destination, built);
                      }));

    plan.observe(layout->send_voice_packet,
                 fc::after([this](void *packet, std::uint8_t destination, std::uint32_t header_bytes, bool accepted) noexcept {
                     observe_wire_send(packet, destination, header_bytes, accepted);
                 }));

    plan.observe(layout->decoder_receive_packet,
                 fc::after([this](void *decoder, std::uint32_t stream, AudioPacket *packet, bool accepted) noexcept {
                     observe_decoder_queue(decoder, stream, packet, accepted);
                 }));

    plan.observe<QueueInvocation>(
        layout->decode_stream,
        fc::before([this](void *node, std::uint8_t sender, AudioPacket *packet, QueueInvocation &invocation) noexcept {
            begin_queue(node, sender, packet, invocation);
        }),
        fc::after([this](void *, std::uint8_t, AudioPacket *, bool accepted, const QueueInvocation &invocation) noexcept {
            end_queue(accepted, invocation);
        }));

    // Save numeric owners while live, then end their trace lifetime after native Close has finished its flushes.
    plan.observe<trace::NodeClosedRecord>(
        layout->node_close, fc::before([this](void *node, std::uint8_t, std::uint8_t, trace::NodeClosedRecord &record) noexcept {
            write_hud(trace::StreamQueueRecord{}, 128);
            ++close_depth_;
            record.node = pointer_id(node);
            record.decoder = fc::read_field<std::uintptr_t>(node, 0x14);
            record.queues = fc::read_field<std::uintptr_t>(node, 0x2C);
        }),
        fc::after([this](void *, std::uint8_t, std::uint8_t, const trace::NodeClosedRecord &record) noexcept {
            observe_node_close(record);
            --close_depth_;
        }));

    if (settings_.capture == CaptureMode::Full) {
        plan.observe_instruction(layout->queue_retired, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                     observe_queue_retired(cpu);
                                 }));
    }
    // This distinct overflow path clears the occupied pending run without passing each descriptor through FlushStreams.
    plan.observe<PendingFlushInvocation>(
        layout->flush_packet, fc::before([this](void *node, void *queue, AudioPacket *, PendingFlushInvocation &invocation) noexcept {
            write_hud(trace::StreamQueueRecord{}, 128);
            invocation.queue = queue;
            auto &record = invocation.record;
            record.node = pointer_id(node);
            record.queue_storage = fc::read_field<std::uintptr_t>(queue, 0);
            record.first_slot = fc::read_field<std::uint16_t>(queue, 0x14);
            record.capacity = fc::read_field<std::uint16_t>(node, 0x38);
            const auto queues = fc::read_field<std::uintptr_t>(node, 0x2C);
            const auto address = reinterpret_cast<std::uintptr_t>(queue);
            if (address >= queues && (address - queues) % 24U == 0 && (address - queues) / 24U < 64) {
                record.sender = static_cast<std::uint8_t>((address - queues) / 24U);
            }
        }),
        fc::after([this](void *, void *, AudioPacket *, bool marker_accepted, const PendingFlushInvocation &invocation) noexcept {
            auto record = invocation.record;
            record.header = header(trace::RecordKind::PendingQueueFlush);
            record.next_slot = fc::read_field<std::uint16_t>(invocation.queue, 0x14);
            record.marker_accepted = static_cast<std::uint8_t>(marker_accepted);
            write(record);
        }));
    plan.observe(layout->update_latency, fc::after([this](void *node, std::uint8_t stream, std::uint32_t target_packets) noexcept {
                     observe_jitter_target(node, stream, target_packets);
                 }));
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    if (role_ == fc::Role::Client) {
        if (auto result = prepare_hud(); !result) {
            return result;
        }
    }
    auto prepared_trace =
        context.trace({.name = "VoiceDiagnostics", .format_version = trace::kSchemaVersion, .buffer_bytes = trace::kChannelCapacity * 80});
    if (!prepared_trace) {
        return std::unexpected(prepared_trace.error());
    }

    trace_ = *prepared_trace;
    return context.publish(api_contract(), VoiceDiagnosticsApi{.capture = fc::bind<&Handler::capture>(*this),
                                                               .state = fc::bind<&Handler::observe_feature_state>(*this),
                                                               .codec = fc::bind<&Handler::observe_codec_outcome>(*this),
                                                               .recipient = fc::bind<&Handler::observe_recipient_policy>(*this),
                                                               .enqueue = fc::bind<&Handler::observe_selected_enqueue>(*this),
                                                               .ui = fc::bind<&Handler::observe_ui>(*this),
                                                               .view = fc::bind<&Handler::observe_view>(*this),
                                                               .activity = fc::bind<&Handler::observe_activity>(*this),
                                                               .admission = fc::bind<&Handler::observe_admission>(*this),
                                                               .sequence = fc::bind<&Handler::observe_sequence>(*this),
                                                               .owner = fc::bind<&Handler::observe_owner>(*this)});
}

std::uint8_t Handler::capture() noexcept {
    owner_observed_.store(true, std::memory_order_relaxed);
    return trace_.enabled() ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(settings_.capture) + 1) : std::uint8_t{0};
}

void Handler::begin_relay(AudioPacket *packet, std::uint8_t sender, std::uint16_t voice_sequence, RelayInvocation &invocation) noexcept {
    // Always link the invocation so the paired after callback can restore TLS when recording is disabled.
    invocation.previous = current_relay_;
    invocation.sender = sender;
    current_relay_ = &invocation;
    if (!trace_.enabled()) {
        return;
    }

    invocation.id = next_relay_.fetch_add(1, std::memory_order_relaxed);
    invocation.recipients.relay = invocation.id;
    invocation.recipients.sender = sender;
    invocation.recipients.voice_sequence = voice_sequence & 0x3FF;

    const auto payload = inspect_payload(packet);
    auto &record = invocation.recipients;
    record.packet = pointer_id(packet);
    record.payload_bytes = payload.byte_size;
    record.payload_kind = payload.kind;
    record.recovery = payload.recovery;
    record.discontinuity = payload.discontinuity;
    record.frame_version = payload.version;
    record.frame_flags = payload.flags;
}

void Handler::end_relay(const RelayInvocation &invocation) noexcept {
    if (invocation.id != 0) {
        auto record = invocation.recipients;
        record.header = header(trace::RecordKind::RelayRecipients);
        write(record);
    }
    current_relay_ = invocation.previous;
}

void Handler::observe_relay_method(std::int32_t method) noexcept {
    if (!trace_.enabled() || current_relay_ == nullptr) {
        return;
    }

    current_relay_->method = method;
    current_relay_->recipients.method = method;
}

void Handler::observe_relay_node_count(std::uint8_t count) noexcept {
    if (!trace_.enabled() || current_relay_ == nullptr) {
        return;
    }

    current_relay_->node_count = count;
    current_relay_->recipients.node_count = count;
}

void Handler::observe_recipient(std::uint8_t destination, bool selected) noexcept {
    if (!trace_.enabled() || current_relay_ == nullptr || destination >= 64) {
        return;
    }

    const auto bit = std::uint64_t{1} << destination;
    current_relay_->recipients.tested |= bit;
    if (selected) {
        current_relay_->recipients.selected |= bit;
    }
}

void Handler::observe_recipient_policy(const RecipientObservation *observation) noexcept {
    if (!trace_.enabled() || current_relay_ == nullptr || observation == nullptr || observation->destination >= 64) {
        return;
    }
    auto &record = current_relay_->recipients;
    const auto bit = std::uint64_t{1} << observation->destination;
    if (observation->native_selected != 0) {
        record.native_selected |= bit;
    }
    if (observation->policy_evaluated == 0) {
        return;
    }
    record.policy_evaluated |= bit;
    if (observation->exclusions == 0) {
        record.fc_permitted |= bit;
    }
    for (std::size_t index = 0; index < std::size(record.exclusions); ++index) {
        if ((observation->exclusions & (1U << index)) != 0) {
            record.exclusions[index] |= bit;
        }
    }
}

void Handler::observe_selected_enqueue(const EnqueueObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }
    trace::SelectedEnqueueRecord record{.header = header(trace::RecordKind::SelectedEnqueue),
                                        .node = observation->node,
                                        .packet = observation->packet,
                                        .intake = current_intake_ != nullptr ? current_intake_->id : 0,
                                        .payload_bytes = observation->payload_bytes,
                                        .voice_sequence = current_intake_ != nullptr ? current_intake_->sequence : std::uint16_t{0xFFFF},
                                        .sender = observation->sender,
                                        .recovery = observation->recovery,
                                        .discontinuity = observation->discontinuity,
                                        .accepted = observation->accepted};
    write(record);
}

void Handler::begin_queue(void *node, std::uint8_t sender, AudioPacket *packet, QueueInvocation &invocation) noexcept {
    invocation.previous = current_queue_;
    current_queue_ = &invocation;
    if (!trace_.enabled() || node == nullptr || packet == nullptr) {
        return;
    }
    const auto payload = inspect_payload(packet);
    auto &record = invocation.record;
    record.node = pointer_id(node);
    record.packet = pointer_id(packet);
    record.sender = sender;
    record.payload_bytes = payload.byte_size;
    record.discontinuity = payload.discontinuity;
    record.recovery = static_cast<std::uint8_t>(payload.recovery);
    // Native sender queues have 24-byte entries and 16-byte descriptors. A temporary empty flush is outside this array.
    const auto queues = fc::read_field<std::uintptr_t>(node, 0x2C);
    const auto count = fc::read_field<std::uint16_t>(node, 0x38);
    if (queues != 0) {
        const auto storage = fc::read_field<std::uintptr_t>(reinterpret_cast<void *>(queues + sender * 24U), 0);
        const auto address = reinterpret_cast<std::uintptr_t>(packet);
        if (storage != 0 && address >= storage && (address - storage) % sizeof(AudioPacket) == 0 &&
            (address - storage) / sizeof(AudioPacket) < count) {
            record.queue_storage = storage;
            record.slot = static_cast<std::uint16_t>((address - storage) / sizeof(AudioPacket));
        }
    }
}

void Handler::end_queue(bool accepted, const QueueInvocation &invocation) noexcept {
    if (invocation.record.node != 0) {
        auto record = invocation.record;
        record.header = {.kind = trace::RecordKind::StreamQueue};
        record.accepted = static_cast<std::uint8_t>(accepted);
        // Only temporary empty cleanup without a retained slot or decoder attempt can repeat.
        if (close_depth_ && !record.payload_bytes && !record.decoder_attempts && record.stream == 0xFFFFFFFF && record.slot == 0xFFFF) {
            write_hud(record, record.sender);
        } else {
            write_hud(trace::StreamQueueRecord{}, 128);
            record.header = header(trace::RecordKind::StreamQueue);
            write(record);
        }
    }
    current_queue_ = invocation.previous;
}

void Handler::observe_node_close(trace::NodeClosedRecord record) noexcept {
    jitter_generation_.fetch_add(1, std::memory_order_relaxed);
    write_hud(trace::StreamQueueRecord{}, 128);
    hud_generation_.fetch_add(1, std::memory_order_relaxed);
    if (!trace_.enabled()) {
        return;
    }
    record.header = header(trace::RecordKind::NodeClosed);
    write(record);
}

void Handler::observe_queue_retired(const fc::CpuContext &cpu) noexcept {
    if (!trace_.enabled()) {
        return;
    }
    // The decoder may already have consumed audio. Read only live descriptor-table scalars and numeric addresses here.
    auto *queue = reinterpret_cast<void *>(cpu.esi);
    auto *frame = reinterpret_cast<void *>(cpu.ebp);
    trace::QueueRetiredRecord record{.header = header(trace::RecordKind::QueueRetired),
                                     .node = cpu.edx,
                                     .queue_storage = fc::read_field<std::uintptr_t>(queue, 0),
                                     .packet = cpu.edi,
                                     .slot = fc::read_field<std::uint16_t>(queue, 0x14),
                                     .sender = fc::read_field<std::uint8_t>(frame, 8),
                                     .receive_allowed = static_cast<std::uint8_t>(cpu.ebx),
                                     .force_flush = fc::read_field<std::uint8_t>(frame, 12)};
    write(record);
}

void Handler::observe_relay_build(AudioPacket *packet, std::uint8_t destination, bool built) noexcept {
    if (!trace_.enabled() || current_relay_ == nullptr) {
        return;
    }

    const auto payload = inspect_payload(packet);
    trace::RelayBuildRecord record{
        .header = header(trace::RecordKind::RelayBuild),
        .relay = current_relay_->id,
        .payload_bytes = payload.byte_size,
        .destination = destination,
        .built = static_cast<std::uint8_t>(built),
        .payload_kind = payload.kind,
        .recovery = payload.recovery,
    };
    write(record);
}

void Handler::observe_wire_send(void *packet, std::uint8_t destination, std::uint32_t header_bytes, bool accepted) noexcept {
    if (!trace_.enabled()) {
        return;
    }

    trace::WireSendRecord record{
        .header = header(trace::RecordKind::WireSend),
        .relay = current_relay_ != nullptr ? current_relay_->id : 0,
        .packet = pointer_id(packet),
        .header_bytes = header_bytes,
        .destination = destination,
        .accepted = static_cast<std::uint8_t>(accepted),
    };
    write(record);
}

void Handler::observe_decoder_queue(void *decoder, std::uint32_t stream, AudioPacket *packet, bool accepted) noexcept {
    if (!trace_.enabled()) {
        return;
    }

    if (current_queue_ != nullptr) {
        auto &record = current_queue_->record;
        record.decoder = pointer_id(decoder);
        record.stream = stream;
        ++record.decoder_attempts;
        record.decoder_accepted += static_cast<std::uint8_t>(accepted);
        return;
    }

    const auto payload = inspect_payload(packet);
    trace::DecoderQueueRecord record{
        .header = header(trace::RecordKind::DecoderQueue),
        .decoder = pointer_id(decoder),
        .packet = pointer_id(packet),
        .stream = stream,
        .payload_bytes = payload.byte_size,
        .accepted = static_cast<std::uint8_t>(accepted),
        .payload_kind = payload.kind,
        .recovery = payload.recovery,
        .discontinuity = payload.discontinuity,
        .frame_version = payload.version,
        .frame_flags = payload.flags,
    };
    write(record);
}

void Handler::observe_jitter_target(void *node, std::uint8_t stream, std::uint32_t target_packets) noexcept {
    if (!trace_.enabled()) {
        return;
    }

    struct LastTarget {
        std::uint64_t node{};
        std::uint32_t packets{};
    };

    // A close invalidates caches on every producer thread, including reopen at the same native address.
    static thread_local std::array<LastTarget, 64> last_targets{};
    static thread_local std::uint32_t last_generation{};
    const auto generation = jitter_generation_.load(std::memory_order_relaxed);
    if (generation != last_generation) {
        last_targets = {};
        last_generation = generation;
    }

    const auto node_id = pointer_id(node);
    if (stream < last_targets.size()) {
        auto &previous = last_targets[stream];
        if (previous.node == node_id && previous.packets == target_packets) {
            return;
        }

        previous = {.node = node_id, .packets = target_packets};
    }

    trace::JitterTargetRecord record{
        .header = header(trace::RecordKind::JitterTarget),
        .node = node_id,
        .target_packets = target_packets,
        .stream = stream,
    };
    write(record);
}

void Handler::observe_feature_state(const StateObservation *observation) noexcept {
    // Accepted control/connection events can replace a player owner without changing its visible flags.
    if (observation && observation->result != StateResult::Rejected) {
        hud_generation_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }

    trace::FeatureStateRecord record{
        .header = header(trace::RecordKind::FeatureState),
        .peer_id = observation->peer_id,
        .revision = observation->revision,
        .peer_slot = observation->peer_slot,
        .player = observation->player,
        .flags = observation->flags,
        .message_kind = observation->message_kind,
        .result = static_cast<std::uint8_t>(observation->result),
        .send_result = observation->send_result,
    };
    write(record);
}

void Handler::observe_codec_outcome(const CodecObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }

    trace::CodecOutcomeRecord record{
        .header = {.kind = trace::RecordKind::CodecOutcome},
        .native_state = observation->native_state,
        .direction = static_cast<std::uint8_t>(observation->direction),
        .result = static_cast<std::uint8_t>(observation->result),
    };
    if (settings_.capture == CaptureMode::Standard && observation->result == CodecResult::Complete) {
        record.header = {.kind = trace::RecordKind::CodecOutcome};
        write_hud(record,
                  static_cast<std::size_t>(((observation->native_state >> 4) ^ static_cast<std::uint8_t>(observation->direction)) % 128));
    } else {
        write_hud(trace::CodecOutcomeRecord{}, 128);
        record.header = header(trace::RecordKind::CodecOutcome);
        write(record);
    }
}

void Handler::observe_activity(const ActivityObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }
    trace::HudActivityRecord record{.header = {.kind = trace::RecordKind::HudActivity},
                                    .hud_frame = current_hud_ ? current_hud_->record.frame : 0,
                                    .value = *observation};
    write_hud(record, observation->player);
}

void Handler::observe_admission(const AdmissionObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }
    trace::NetworkAdmissionRecord record{.header = {.kind = trace::RecordKind::NetworkAdmission}, .value = *observation};
    if (observation->reason == AdmissionReason::NoPacket) {
        write_hud(record);
    } else {
        write_hud(trace::NetworkAdmissionRecord{}, 128);
        record.header = header(trace::RecordKind::NetworkAdmission);
        write(record);
    }
}

void Handler::observe_sequence(const SequenceObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }
    trace::SequenceAdmissionRecord record{.header = header(trace::RecordKind::SequenceAdmission), .value = *observation};
    write(record);
}

void Handler::observe_owner(const OwnerObservation *observation) noexcept {
    if (!trace_.enabled() || observation == nullptr) {
        return;
    }
    trace::ReceiveOwnerRecord record{.header = header(trace::RecordKind::ReceiveOwner), .value = *observation};
    write(record);
}

Handler::PayloadInfo Handler::inspect_payload(const AudioPacket *packet) noexcept {
    if (packet == nullptr) {
        return {};
    }

    // Copy the raw flag byte, including unspecified bits; native consumers test only bit zero for discontinuity.
    PayloadInfo result{
        .byte_size = packet->size,
        .kind = packet->size == 0 ? trace::PayloadKind::Empty : trace::PayloadKind::Unavailable,
        .discontinuity = packet->discontinuity,
    };
    if (packet->size == 0) {
        return result;
    }

    if (packet->data == nullptr) {
        return result;
    }

    result.kind = trace::PayloadKind::Native;
    if (packet->size < 8) {
        return result;
    }

    // Sample the prefix only. VoiceChat checks version, declared payload length, padding and codec validity elsewhere.
    std::array<std::byte, 8> prefix{};
    std::memcpy(prefix.data(), packet->data, prefix.size());
    constexpr std::array kFusionCutterMagic{
        std::byte{0xF9},
        std::byte{0x7D},
        std::byte{'F'},
        std::byte{'C'},
    };
    if (!std::equal(kFusionCutterMagic.begin(), kFusionCutterMagic.end(), prefix.begin())) {
        return result;
    }

    result.kind = trace::PayloadKind::FusionCutter;
    result.version = std::to_integer<std::uint8_t>(prefix[4]);
    result.flags = std::to_integer<std::uint8_t>(prefix[5]);

    // Recovery flags describe the prefix marker, including internal FEC/PLC forms seen downstream of wire validation.
    switch (result.flags) {
    case 0:
        result.recovery = trace::RecoveryMode::None;
        break;
    case 1:
        result.recovery = trace::RecoveryMode::ForwardErrorCorrection;
        break;
    case 2:
        result.recovery = trace::RecoveryMode::Concealment;
        break;
    default:
        result.recovery = trace::RecoveryMode::Invalid;
        break;
    }
    return result;
}

std::uint64_t Handler::pointer_id(const void *pointer) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

trace::RecordHeader Handler::header(trace::RecordKind kind) noexcept {
    return {
        .sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed),
        .kind = kind,
    };
}

} // namespace fc_plugins::voice_diagnostics
