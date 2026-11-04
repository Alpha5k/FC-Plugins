#include "handler.hpp"
#include "microphone_test.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>

namespace fc_plugins::voice_chat {
namespace {

// The decoder owns an auxiliary renderer at 0xA8; its flags byte is 0x18C bytes into that object.
constexpr std::size_t kAuxiliaryRendererOffset = 0xA8;
constexpr std::size_t kRendererFlagsOffset = 0x18C;

// The encoder owns its automatic gain controller at 0x4B4; its maximum gain field is 0x24 bytes into that object.
constexpr std::size_t kAutomaticGainControlOffset = 0x4B4;
constexpr std::size_t kMaximumGainOffset = 0x24;

// Both retail close paths detach their pipelines before releasing these embedded/nested GSM states.
constexpr std::size_t kEncoderGsmOffset = 0x60 + 0x2C;
constexpr std::size_t kDecoderStreamsOffset = 0x14;
constexpr std::size_t kDecoderCountOffset = 0x274;
constexpr std::size_t kDecoderStreamStride = 0x2D4;
constexpr std::size_t kStreamGsmOffset = 0x2C;

constexpr std::uint8_t kHardwareGainFlag = 0x10; // Selects the renderer's DirectSound gain path.
constexpr std::uint32_t kQ15Unity = 0x7FFF;      // Native unity convention, one below 2^15.
constexpr std::uint32_t kMaximumCaptureGain = 2 * kQ15Unity;

// Sets the microphone AGC ceiling from the normalized setting: slider 0/5/10 permits about 0/1/2 gain.
// Native clipping/gating still chooses the target, and current gain converges per sample even after selecting zero.
void apply_capture_gain(fc::CpuContext &context) noexcept {
    if (context.ecx == 0) {
        return;
    }

    // Encoder::SetGain receives the profile's 0-10 value divided by ten in the low float of XMM1.
    const auto normalized = fc::read_field<float>(&context.xmm[1].low);
    const auto gain = normalized * 2.0F;

    // Positive values truncate to Q15; zero, negative and NaN stay zero, and values at least two reach the ceiling.
    std::uint32_t fixed_gain{};
    if (gain >= 2.0F) {
        fixed_gain = kMaximumCaptureGain;
    } else if (gain > 0.0F) {
        fixed_gain = static_cast<std::uint32_t>(gain * static_cast<float>(kQ15Unity));
    }

    auto *encoder = reinterpret_cast<void *>(context.ecx);
    fc::write_field(encoder, kAutomaticGainControlOffset + kMaximumGainOffset, fixed_gain);
}

// Selects DirectSound gain because the software gain path halves PCM at unity gain.
// The saved playback slider already reaches the native mixer; this removes the renderer's extra attenuation.
void enable_playback_hardware_gain(fc::CpuContext &context) noexcept {
    if (context.esi == 0) {
        return;
    }

    auto *decoder = reinterpret_cast<void *>(context.esi);
    const auto renderer_flags = fc::read_field<std::uint8_t>(decoder, kAuxiliaryRendererOffset + kRendererFlagsOffset);

    // Preserve other renderer flags; the displaced instructions update a separate decoder flags byte at +0x281.
    fc::write_field(decoder, kAuxiliaryRendererOffset + kRendererFlagsOffset,
                    static_cast<std::uint8_t>(renderer_flags | kHardwareGainFlag));
}

constexpr float kPacketMilliseconds = 20.0F;
constexpr float kSamplesPerMillisecond = 48.0F;
constexpr std::uint32_t kSampleAlignment = kFrameSamples;
constexpr std::uint32_t kCaptureRingScale = 10;
constexpr std::uint32_t kMinimumJitterPackets = 2;
constexpr std::uint32_t kMaximumJitterPackets = 4;
constexpr std::uint64_t kNativeVoiceThreshold = 30'000;

// Limits the ring-size modifier to a voice capture open on this thread.
thread_local bool gOpeningVoiceCapture{};

// Carries the native sender through RelayAudioPacket's nested recipient calls.
thread_local std::uint8_t gRelaySource{0xFF};
// Captured sends use the node's player ID; the shared native read header belongs to incoming audio.
thread_local std::uint8_t gCaptureSource{0xFF};
// Retains the admitted network identity while native intake builds a relay; local reads leave it zero.
thread_local std::uint64_t gNetworkIdentity{};

// Shares recovery only with the per-node copy reached by this thread's single routing call.
struct ReceiveRecovery {
    AudioPacket *packet{};
    std::uint16_t missing_packets{};
};

thread_local ReceiveRecovery gReceiveRecovery{};

// Clamps the packet count in ECX before the displaced instruction stores CX in the sender's jitter state.
void limit_jitter_target(fc::CpuContext &context) noexcept {
    context.ecx = std::clamp(context.ecx, kMinimumJitterPackets, kMaximumJitterPackets);
}

} // namespace

void Handler::plan(fc::Plan &plan, bool include_client) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Player launches, including player hosts, add volume and HUD corrections.
    if (include_client) {
        // Q15 has 15 fractional bits; the retail shift uses 16.
        constexpr auto corrected_capture_gain = fc::byte_array<0x0F, 0xAC, 0xD9, 0x0F>();
        plan.write(layout->scale_capture_gain, corrected_capture_gain);

        // The shared mixer's final Q15 scaling also shifts by 16, halving playback below 100%.
        constexpr auto corrected_playback_gain = fc::byte_array<0xC1, 0xFA, 0x0F>();
        for (const auto &location : layout->scale_playback_gain) {
            plan.write(location, corrected_playback_gain);
        }

        // Instruction modifiers run before the displaced code; SetGain's original source gain store still runs.
        plan.hook_instruction(layout->source_gain_store, apply_capture_gain);
        plan.hook_instruction(layout->playback_renderer_ready, enable_playback_hardware_gain);
    }

    constexpr auto sample_rate = fc::byte_array<0x68, 0x80, 0xBB, 0x00, 0x00>();
    constexpr auto vad_sample_rate = fc::byte_array<0xC7, 0x46, 0x30, 0x80, 0xBB, 0x00, 0x00>();
    constexpr auto vad_release = fc::byte_array<0xC7, 0x46, 0x34, 0x00, 0x77, 0x01, 0x00>();
    constexpr auto server_relay_default = fc::byte_array<0xB9, 0x04, 0x00, 0x00, 0x00>();

    // Voice open selects server relay (method 4) and skips the command-line override.
    // Game options and NetGame resets can select a different method later.
    plan.write(layout->default_transport_method, server_relay_default);
    plan.nop(layout->transport_override, 3);

    local_echo_memory_ = plan.access(layout->local_echo_enabled);
    plan.check(layout->local_echo_store, layout->local_echo_store.evidence.bytes.size());
    // The MOV embeds an absolute address that Windows relocates; validate its target at the loaded image base.
    plan.check(plan.at(layout->local_echo_store.rva + 2, layout->local_echo_store.name), sizeof(std::uint32_t),
               fc::points_to(plan.at(layout->local_echo_enabled.rva, layout->local_echo_enabled.name)));
    plan.observe(layout->local_echo_enable, fc::after([this](bool) noexcept {
                     microphone_test_changed();
                 }));
    plan.hook_instruction(layout->send_method_dispatch, [this](fc::CpuContext &context) noexcept {
        select_send_method(context);
    });
    plan.hook_instruction(layout->send_network_dispatch, [this](fc::CpuContext &context) noexcept {
        select_send_delivery(context);
    });

    relay_audio_original_ =
        plan.hook(layout->relay_audio, [this](AudioPacket *packet, std::uint8_t sender, std::uint16_t sequence) noexcept {
            relay_audio(relay_audio_original_, packet, sender, sequence);
        });

    relay_recipient_original_ = plan.hook_call(layout->relay_recipient_call, [this](void *header, std::uint8_t destination) noexcept {
        return select_recipient(relay_recipient_original_, header, destination);
    });

    // Skip the ten sequence bits and discontinuity flag before counting recipients.
    // The displaced INC already skips the format bit; GetReceiveNode uses the same bitmap start.
    plan.hook_instruction(layout->recipient_count_start, [](fc::CpuContext &context) noexcept {
        context.esi += 11;
    });

    // Gate peer traffic by each local node's native policy and accepted FC state while keeping local echo.
    send_possible_original_ =
        plan.hook(layout->send_possible, [this](void *transport, std::uint8_t source, std::uint8_t destination) noexcept {
            return send_possible(send_possible_original_, transport, source, destination);
        });

    receive_possible_original_ =
        plan.hook(layout->receive_possible, [this](void *transport, std::uint8_t receiver, std::uint8_t source) noexcept {
            return receive_possible(receive_possible_original_, transport, receiver, source);
        });

    if (include_client) {
        // Use gamefont_large, the score HUD's font. Bind also sizes each text box from the selected font.
        plan.write(layout->hud_font, fc::byte_array<0x68, 0xDE, 0x64, 0xE5, 0xB3>());
        plan.hook_instruction(layout->hud_row, [](fc::CpuContext &context) noexcept {
            const auto height = fc::read_field<std::uint8_t>(context.edi, 0x0E);
            const auto row = 4 - context.ebx;
            // SetPosition stores Y at text+0x64. Space rows above the first name by the font height plus four pixels.
            fc::write_field(context.esi, 0x64, -static_cast<float>(row * (height + 4)));
        });

        // The three gameplay HUD queries use actual local sending or qualified remote reception.
        plan.hook_call(layout->hud_receive_possible, [this](void *transport, std::uint8_t receiver, std::uint8_t source) noexcept {
            return hud_permission(transport, receiver, source);
        });
        plan.hook_call(layout->hud_activity, [this](void *transport, std::uint8_t player) noexcept {
            return hud_activity(transport, player);
        });
        plan.hook_call(layout->hud_last_activity, [this](std::uint8_t player) noexcept {
            return hud_last_activity(player);
        });
    }

    is_local_ = plan.function(layout->is_local);
    send_connected_ = plan.hook(layout->send_connected, [this](void *transport, std::uint8_t player) noexcept {
        return player_state(send_connected_, transport, player, SendConnected);
    });
    receive_connected_ = plan.hook(layout->receive_connected, [this](void *transport, std::uint8_t player) noexcept {
        return player_state(receive_connected_, transport, player, ReceiveConnected);
    });
    get_sending_original_ = plan.hook(layout->get_sending, [this](void *transport, std::uint8_t player) noexcept {
        return player_state(get_sending_original_, transport, player, static_cast<std::uint8_t>(Speaking | SendConnected));
    });

    // Every valid local node reaches SetSending after its native capture/send update, even when silent.
    plan.observe(layout->set_sending, fc::after([this](std::uint8_t player, bool speaking) noexcept {
                     set_sending(player, speaking);
                 }));

    // Decode synchronizes native connection bits after SetSending; publish readiness when that work is complete.
    plan.observe(layout->native_update, fc::before([this](float) noexcept {
                     synchronize_receive_owners();
                 }));
    plan.observe(layout->native_update, fc::after([this](float) noexcept {
                     refresh_local_routing();
                 }));

    // Capture, codec, and playback must agree on the 48 kHz mono PCM stream.
    plan.write(layout->encoder_sample_rate, sample_rate);
    plan.write(layout->decoder_sample_rate, sample_rate);
    plan.write(layout->vad_sample_rate, vad_sample_rate);

    // Bypass the 8 kHz frequency filter, which rejects valid 48 kHz speech.
    // Keep the energy decision and a 96,000-byte release budget for 48 kHz mono PCM16.
    plan.write(layout->vad_release, vad_release);
    plan.write(layout->vad_frequency_filter, fc::byte_array<0xEB, 0x2A, 0x90>());
    plan.hook_instruction(layout->vad_power_result, [this](fc::CpuContext &context) noexcept {
        apply_voice_threshold(context);
    });

    // The native wrappers otherwise clamp the encoder to a 200 ms packet.
    // Their block loops remain useful and now collect six blocks per frame.
    plan.write(layout->encoder_packet_duration, fc::byte_array<0xEB, 0x08>());
    plan.hook_instruction(layout->packet_duration, set_packet_duration);
    plan.hook(layout->calculate_samples, [](float milliseconds) noexcept {
        return calculate_samples(milliseconds);
    });

    // Both hooks are limited to the voice stream's open calls. Capture follows the
    // profile selection, while playback follows the game's DirectSound device.
    capture_original_ =
        plan.hook_call(layout->voice_capture_open, [this](void *source, std::uint8_t device, void *format, std::uint32_t packet_bytes,
                                                          std::uint8_t packet_count, void *packets) noexcept {
            return open_capture(capture_original_, source, device, format, packet_bytes, packet_count, packets);
        });

    playback_original_ =
        plan.hook_call(layout->voice_playback_open, [this](void *renderer, std::uint8_t device, void *format, float buffer_ms) noexcept {
            return open_playback(playback_original_, renderer, device, format, buffer_ms);
        });

    plan.hook_instruction(layout->capture_ring_size, [this](fc::CpuContext &context) noexcept {
        restore_capture_window(context);
    });

    // FC prepares callable handles with each site's ABI; native objects remain borrowed from the game.
    get_current_profile_ = plan.function(layout->get_current_profile);
    game_voice_close_ = plan.hook(layout->game_voice_close, [this]() noexcept {
        static_cast<void>(game_voice_close_.call());
        refresh_local_routing();
    });
    game_voice_is_open_ = plan.function(layout->game_voice_is_open);
    find_voice_node_ = plan.function(layout->find_voice_node);
    get_voice_transport_ = plan.function(layout->get_voice_transport);
    get_lobby_player_index_ = plan.function(layout->get_lobby_player_index);
    send_enable_ = plan.function(layout->send_enable);
    receive_enable_ = plan.function(layout->receive_enable);
    get_num_nodes_ = plan.function(layout->get_num_nodes);
    flush_packet_ = plan.function(layout->flush_packet);
    packet_reset_ = plan.function(layout->packet_reset);
    packet_release_ = plan.function(layout->packet_release);
    packet_audio_ = plan.function(layout->packet_audio);
    read_header_ = plan.function(layout->read_header);
    header_sender_ = plan.function(layout->header_sender);
    receive_metrics_memory_ = plan.access(layout->receive_metrics);
    voice_time_memory_ = plan.access(layout->voice_time);
    voice_nodes_memory_ = plan.access(layout->voice_nodes);
    // These native operands embed addresses relocated by Windows; bind each global to its actual loaded target.
    plan.check(layout->native_update.rva + 0x4D, sizeof(std::uint32_t), fc::points_to(plan.at(layout->receive_metrics.rva)));
    plan.check(layout->native_update.rva + 0x127, sizeof(std::uint32_t), fc::points_to(plan.at(layout->voice_time.rva)));
    plan.check(layout->native_update.rva + 0x1BE, sizeof(std::uint32_t), fc::points_to(plan.at(layout->voice_nodes.rva)));
    // Both device boundaries clear +0x3A and +0x20. Retain the sequence and actual last-send time of the same node.
    node_open_original_ =
        plan.hook(layout->node_open, [this](void *node, std::uint32_t capture, std::uint32_t playback, std::uint32_t flags) noexcept {
            const auto sequence = fc::read_field<std::uint16_t>(node, 0x3A);
            const auto last_send = fc::read_field<float>(node, 0x20);
            const auto result = node_open_original_.call(node, capture, playback, flags).value_or(false);
            fc::write_field(node, 0x3A, sequence);
            fc::write_field(node, 0x20, last_send);
            return result;
        });
    node_close_original_ = plan.hook(layout->node_close, [this](void *node, std::uint8_t send, std::uint8_t receive) noexcept {
        const auto sequence = fc::read_field<std::uint16_t>(node, 0x3A);
        const auto last_send = fc::read_field<float>(node, 0x20);
        static_cast<void>(node_close_original_.call(node, send, receive));
        fc::write_field(node, 0x3A, sequence);
        fc::write_field(node, 0x20, last_send);
    });
    decode_stream_original_ = plan.hook(layout->decode_stream, [this](void *node, std::uint8_t sender, AudioPacket *packet) noexcept {
        return decode_stream(decode_stream_original_, node, sender, packet);
    });
    network_read_original_ = plan.hook_call(layout->network_read, [this](std::uint32_t type, std::uint32_t priority) noexcept {
        return read_network_audio(network_read_original_, type, priority);
    });
    plan.observe(layout->receive_packet, fc::before([]() noexcept {
                     gNetworkIdentity = 0;
                 }));
    network_send_original_ =
        plan.hook_call(layout->network_send, [this](std::uint8_t destination, void *packet, std::uint32_t header_bytes) noexcept {
            return send_network_audio(network_send_original_, destination, packet, header_bytes);
        });
    classify_packet_original_ = plan.hook_call(layout->voice_lookup, [this](std::uint32_t type) noexcept {
        // Joining clients use the existing current-owner/server-origin branch for host voice relays.
        return type == 0x18 && mode_.load(std::memory_order_acquire) == fc::GameMode::Client
                   ? true
                   : classify_packet_original_.call(type).value_or(false);
    });
    sequence_admission_memory_ = plan.access<const std::byte>(plan.at(layout->sequence_admission.rva, layout->sequence_admission.name));
    plan.hook_instruction(layout->sequence_admission, [this](fc::CpuContext &context) noexcept {
        admit_sequence(context);
    });
    game_voice_open_ = plan.hook(layout->game_voice_open, [this](float open_rate, bool open_devices) noexcept {
        return open_voice(game_voice_open_, open_rate, open_devices);
    });

    apply_options_original_ = plan.hook(layout->apply_online_options, [this](void *options) noexcept {
        apply_online_options(apply_options_original_, options);
    });

    reset_options_original_ = plan.hook(layout->reset_online_options, [this](void *options) noexcept {
        reset_online_options(reset_options_original_, options);
    });

    gsm_init_original_ = plan.hook(layout->gsm_init, [this](void *state) noexcept {
        initialize_gsm(gsm_init_original_, state);
    });

    encoder_close_original_ = plan.hook(layout->encoder_close, [this](void *encoder) noexcept {
        const auto state = reinterpret_cast<std::uintptr_t>(encoder) + kEncoderGsmOffset;
        if (encoder_close_original_.call(encoder)) {
            // Native Close has detached input/output and stopped capture, including any final flush.
            codecs_.release(reinterpret_cast<void *>(state));
        }
    });

    decoder_close_original_ = plan.hook(layout->decoder_close, [this](void *decoder) noexcept {
        const auto streams = fc::read_field<std::uintptr_t>(decoder, kDecoderStreamsOffset);
        const auto count = fc::read_field<std::uint32_t>(decoder, kDecoderCountOffset);
        if (decoder_close_original_.call(decoder) && streams != 0) {
            // Native Close disconnects sources/stops render before destruction; saved addresses are never dereferenced here.
            for (std::uint32_t index = 0; index < count; ++index) {
                codecs_.release(reinterpret_cast<void *>(streams + index * kDecoderStreamStride + kStreamGsmOffset));
            }
        }
    });

    plan.hook(layout->gsm_encode, [this](void *state, const std::int16_t *pcm, std::byte *encoded) noexcept {
        encode_gsm(state, pcm, encoded);
    });

    plan.hook(layout->gsm_decode, [this](void *state, const std::byte *encoded, std::int16_t *pcm) noexcept {
        decode_gsm(state, encoded, pcm);
    });

    // Audio stays on the game's voice carrier. Send trims internal frames;
    // receive expands local playback data while relay retains the wire packet.
    send_audio_original_ = plan.hook(layout->send_audio, [this](void *node, AudioPacket *packet) noexcept {
        send_audio(send_audio_original_, node, packet);
    });

    receive_audio_original_ = plan.hook(layout->receive_audio, [this](std::uint8_t sender, AudioPacket *packet, std::uint8_t relay,
                                                                      std::uint16_t missing_packets) noexcept {
        return receive_audio(receive_audio_original_, sender, packet, relay, missing_packets);
    });

    enqueue_audio_original_ =
        plan.hook_call(layout->receive_enqueue_call, [this](void *node, std::uint8_t sender, AudioPacket *packet) noexcept {
            if (packet != gReceiveRecovery.packet || packet == nullptr) {
                return enqueue_audio(node, sender, packet);
            }

            // Native policy already selected this node. Each copy keeps its queue, flush and release ownership.
            return enqueue_recovered_audio(*packet, gReceiveRecovery.missing_packets, [&](AudioPacket &queued) noexcept {
                return enqueue_audio(node, sender, &queued);
            });
        });

    plan.hook_instruction(layout->sequence_gap, [](fc::CpuContext &context) noexcept {
        // SUB/DEC have already formed the admitted gap; MOVZX consumes AX before the native loss cap.
        context.eax &= 0x3FFU;
    });

    plan.hook_instruction(layout->jitter_target, limit_jitter_target);
}

std::uint32_t Handler::calculate_samples(float milliseconds) noexcept {
    if (!(milliseconds > 0.0F)) {
        return 0;
    }

    const auto exact = milliseconds * kSamplesPerMillisecond;
    if (exact >= static_cast<float>(std::numeric_limits<std::uint32_t>::max() - kSampleAlignment)) {
        return 0;
    }
    auto samples = static_cast<std::uint32_t>(exact);
    if (static_cast<float>(samples) < exact) {
        ++samples;
    }

    // The wrappers consume 160-sample blocks, while Opus requires the complete
    // 960-sample frame. Alignment keeps every requested buffer divisible by six.
    return ((samples + kSampleAlignment - 1) / kSampleAlignment) * kSampleAlignment;
}

void Handler::set_packet_duration(fc::CpuContext &context) noexcept {
    // FC runs this modifier before the displaced MOVSS stores XMM0 in sBufferSizeMs.
    fc::write_field(&context.xmm[0].low, 0, kPacketMilliseconds);
}

void Handler::restore_capture_window(fc::CpuContext &context) const noexcept {
    // The displaced ADD EAX,EAX doubles this scaled allocation: 20 packets, or 400 ms at 20 ms each.
    if (gOpeningVoiceCapture) {
        context.eax *= kCaptureRingScale;
    }
}

void Handler::select_send_method(fc::CpuContext &context) noexcept {
    // The displaced SUB dispatches on this effective value; the transport object's stored method remains unchanged.
    context.eax = MicrophoneTestRoute::method(context.eax);
    if (reported_transport_method_.exchange(context.eax, std::memory_order_relaxed) != context.eax) {
        const auto error = GetLastError();
        if (context.eax == 2) {
            logger_.debug("Voice sends to each recipient (method 2)");
        } else if (context.eax == 4) {
            logger_.debug("Voice uses server relay (method 4)");
        } else {
            logger_.debug("Voice transport selected method {}", context.eax);
        }
        SetLastError(error);
    }
}

void Handler::select_send_delivery(fc::CpuContext &context) noexcept {
    const auto network = MicrophoneTestRoute::network_delivery((context.ecx & 0xFFU) != 0, static_cast<std::uint8_t>(context.edi));
    // Only CL is consumed by the displaced TEST; the native branch retains its queue, ownership and readiness checks.
    if (!network) {
        context.ecx &= ~0xFFU;
    }
    // Relay shares SendVoice; it must not alternate this captured-route log on every host packet during a test.
    if (MicrophoneTestRoute::captured() &&
        reported_network_delivery_.exchange(network ? 1 : 0, std::memory_order_relaxed) != (network ? 1 : 0)) {
        const auto error = GetLastError();
        logger_.debug("Voice delivery selected the {} route", network ? "network" : "local");
        SetLastError(error);
    }
}

void Handler::microphone_test_changed() noexcept {
    const auto enabled = local_echo_enabled_ != nullptr && *local_echo_enabled_ != 0;
    if (reported_microphone_test_.exchange(enabled, std::memory_order_relaxed) != enabled) {
        const auto error = GetLastError();
        logger_.info("Microphone test {}", enabled ? "started" : "stopped");
        SetLastError(error);
    }
}

bool Handler::open_capture(fc::NativeFunction<CaptureSourceOpen> original, void *source, std::uint8_t, void *format,
                           std::uint32_t packet_bytes, std::uint8_t packet_count, void *packets) noexcept {
    // Keep the voice-capture marker set during the original open and clear it when this scope ends.
    struct CaptureScope final {
        CaptureScope() noexcept {
            gOpeningVoiceCapture = true;
        }

        ~CaptureScope() {
            gOpeningVoiceCapture = false;
        }
    } scope;

    // The marker scopes the inner ring size hook to this voice stream's open call;
    // other users of StreamCaptureSource retain their native allocation.
    const auto device = selected_capture_.load(std::memory_order_relaxed);
    const auto result = original.call(source, device, format, packet_bytes, packet_count, packets);
    const auto error = GetLastError();
    if (result.value_or(false)) {
        reported_open_failures_.fetch_and(~CaptureOpenFailed, std::memory_order_relaxed);
        logger_.debug("Microphone device {} capture opened", device);
    } else if ((reported_open_failures_.fetch_or(CaptureOpenFailed, std::memory_order_relaxed) & CaptureOpenFailed) == 0) {
        if (!result) {
            logger_.error("Could not call native microphone capture open for device {}: {}", device, result.error().message);
        } else {
            logger_.error("The game could not open microphone device {}", device);
        }
    }
    SetLastError(error);
    return result.value_or(false);
}

bool Handler::open_playback(fc::NativeFunction<AuxRendererOpen> original, void *renderer, std::uint8_t, void *format,
                            float buffer_ms) noexcept {
    // 0xFE makes StreamAuxRenderer reuse the DirectSound device already opened
    // by the game's audio engine instead of Windows' communications device.
    const auto result = original.call(renderer, std::uint8_t{0xFE}, format, buffer_ms);
    const auto error = GetLastError();
    if (result.value_or(false)) {
        reported_open_failures_.fetch_and(~PlaybackOpenFailed, std::memory_order_relaxed);
        logger_.debug("Voice playback on the game audio device opened");
    } else if ((reported_open_failures_.fetch_or(PlaybackOpenFailed, std::memory_order_relaxed) & PlaybackOpenFailed) == 0) {
        if (!result) {
            logger_.error("Could not call native voice playback open: {}", result.error().message);
        } else {
            logger_.error("The game could not open voice playback");
        }
    }
    SetLastError(error);
    return result.value_or(false);
}

void Handler::apply_voice_threshold(fc::CpuContext &context) const noexcept {
    // SignalPower16 returns mean-square PCM16 power in EDX:EAX. Map it to the native comparison's two outcomes
    // so the selected threshold controls the game's voice-activity check.
    const auto measured = (static_cast<std::uint64_t>(context.edx) << 32U) | static_cast<std::uint64_t>(context.eax);
    const auto threshold = vad_threshold_.load(std::memory_order_relaxed);
    context.eax = measured > threshold ? static_cast<std::uint32_t>(kNativeVoiceThreshold + 1) : 0;
    context.edx = 0;
}

void Handler::initialize_gsm(fc::NativeFunction<GsmInit> original, void *state) noexcept {
    // The containing sound object still expects its native codec state to be initialized.
    static_cast<void>(original.call(state));
    codecs_.reset(state);
}

void Handler::encode_gsm(void *state, const std::int16_t *pcm, std::byte *encoded) noexcept {
    // The codec also serves the game's local microphone test, which has no
    // negotiated peer. Network compatibility is enforced by the route hooks.
    observe_codec(state, CodecDirection::Encode, codecs_.encode(state, pcm, encoded));
}

void Handler::decode_gsm(void *state, const std::byte *encoded, std::int16_t *pcm) noexcept {
    observe_codec(state, CodecDirection::Decode, codecs_.decode(state, encoded, pcm));
}

bool Handler::decode_stream(fc::NativeFunction<DecodeStream> original, void *node, std::uint8_t sender, AudioPacket *packet) noexcept {
    const auto queues = fc::read_field<std::uintptr_t>(node, 0x2C);
    if (packet != nullptr && packet->size == 0 && (packet->discontinuity & 1U) != 0 &&
        fc::read_field<std::uint8_t>(reinterpret_cast<void *>(queues + sender * 0x18), 0x16) == 0xFF) {
        return true;
    }
    return original.call(node, sender, packet).value_or(false);
}

void Handler::admit_sequence(fc::CpuContext &context) noexcept {
    // ESI indexes a player's nine-dword metric row; EDI points to the receive metric array.
    const auto *metrics = reinterpret_cast<const void *>(context.edi + context.esi * 4);
    const auto active = (fc::read_field<std::uint8_t>(metrics, 0x20) & 1U) != 0;
    const auto distance = (context.eax - context.ecx) & 0x3FFU;
    // Skip the obsolete compare/backward-history branch entirely. Reject before either timestamp or sequence changes.
    const auto accepted = !active || (distance != 0 && distance < 0x200U);
    // Resume in the game code; the saved EIP points to displaced instructions in a trampoline.
    context.eip = sequence_admission_address_ + (accepted ? 0x34U : 0x139U);
    if (diagnostics_enabled()) {
        const voice_diagnostics::SequenceObservation observation{.metrics = reinterpret_cast<std::uintptr_t>(metrics),
                                                                 .last_activity = fc::read_field<float>(metrics, 8),
                                                                 .incoming = static_cast<std::uint16_t>(context.eax & 0x3FFU),
                                                                 .prior = static_cast<std::uint16_t>(context.ecx & 0x3FFU),
                                                                 .distance = static_cast<std::uint16_t>(distance),
                                                                 .player = static_cast<std::uint8_t>(context.esi / 9U),
                                                                 .active = static_cast<std::uint8_t>(active),
                                                                 .accepted = static_cast<std::uint8_t>(accepted)};
        const auto error = GetLastError();
        diagnostics_.sequence(&observation);
        SetLastError(error);
    }
}

std::uint64_t Handler::audio_identity(std::uint8_t player) const noexcept {
    const auto mode = mode_.load(std::memory_order_acquire);
    return mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated ? server_peers_.identity(player)
                                                                         : remote_players_.identity(player);
}

void Handler::synchronize_receive_owners() noexcept {
    if (receive_metrics_ == nullptr || *receive_metrics_ == 0 || voice_nodes_ == nullptr) {
        return;
    }
    auto *transport = get_voice_transport_.call().value_or(nullptr);
    const auto count = transport != nullptr ? get_num_nodes_.call(transport).value_or(0) : 0;
    const auto host = audio_host_.load(std::memory_order_acquire);
    const auto old_host = receive_host_;
    const auto host_changed = old_host != host;
    receive_host_ = host;
    for (std::uint8_t player = 0; player < count; ++player) {
        const auto identity = audio_identity(player);
        if (!host_changed && receive_owners_[player] == identity) {
            continue;
        }
        const auto old_identity = receive_owners_[player];
        receive_owners_[player] = identity;
        received_speech_[player].time.store(-2.0F, std::memory_order_relaxed);
        auto *metrics = reinterpret_cast<void *>(*receive_metrics_ + player * 0x24);
        const auto old_flags = fc::read_field<std::uint8_t>(metrics, 0x20);
        voice_diagnostics::OwnerObservation observation{.old_host = old_host,
                                                        .new_host = host,
                                                        .old_identity = old_identity,
                                                        .new_identity = identity,
                                                        .metrics = reinterpret_cast<std::uintptr_t>(metrics),
                                                        .old_last = fc::read_field<float>(metrics, 8),
                                                        .player = player,
                                                        .old_active = static_cast<std::uint8_t>(old_flags & 1U)};
        fc::write_field(metrics, 8, 0.0F);
        fc::write_field(metrics, 0x20, static_cast<std::uint8_t>(old_flags & ~1U));
        const auto observe = [&](voice_diagnostics::OwnerOutcome outcome) noexcept {
            if (diagnostics_enabled()) {
                observation.outcome = outcome;
                const auto error = GetLastError();
                diagnostics_.owner(&observation);
                SetLastError(error);
            }
        };
        observe(voice_diagnostics::OwnerOutcome::MetricsReset);

        // Flush each local node's affected queue; already mixed audio and other speakers finish normally.
        auto link = *voice_nodes_;
        while (link != 0) {
            auto *node = fc::read_field<void *>(reinterpret_cast<void *>(link), 0xC);
            observation.node = reinterpret_cast<std::uintptr_t>(node);
            observation.queues = 0;
            observation.flush_accepted = 0;
            if (node == nullptr) {
                observe(voice_diagnostics::OwnerOutcome::MissingNode);
                break;
            }
            if (local_echo_enabled_ != nullptr && *local_echo_enabled_ != 0 &&
                fc::read_field<std::uint8_t>(node, kVoicePlayerOffset) == player) {
                observe(voice_diagnostics::OwnerOutcome::EchoSkipped);
                link = fc::read_field<std::uintptr_t>(reinterpret_cast<void *>(link), 4);
                continue;
            }
            const auto queues = fc::read_field<std::uintptr_t>(node, 0x2C);
            // A closed node has released its receive queues; the next open starts with empty storage.
            if (queues != 0) {
                AudioPacket marker{.discontinuity = 1};
                const auto result = flush_packet_.call(node, reinterpret_cast<void *>(queues + player * 0x18), &marker);
                observation.queues = queues;
                observation.flush_accepted = result.value_or(false);
                observe(result ? voice_diagnostics::OwnerOutcome::Flushed : voice_diagnostics::OwnerOutcome::FlushCallFailed);
            } else {
                observe(voice_diagnostics::OwnerOutcome::ClosedNode);
            }
            link = fc::read_field<std::uintptr_t>(reinterpret_cast<void *>(link), 4);
        }
    }
}

void *Handler::read_network_audio(fc::NativeFunction<GetNetworkPacket> original, std::uint32_t type, std::uint32_t priority) noexcept {
    for (;;) {
        const auto returned = original.call(type, priority);
        auto *packet = returned.value_or(nullptr);
        voice_diagnostics::AdmissionObservation observation{
            .packet = reinterpret_cast<std::uintptr_t>(packet), .type = type, .priority = priority};
        const auto observe = [&](voice_diagnostics::AdmissionReason reason) noexcept {
            if (diagnostics_enabled()) {
                observation.reason = reason;
                observation.accepted = reason == voice_diagnostics::AdmissionReason::Accepted;
                const auto error = GetLastError();
                diagnostics_.admission(&observation);
                SetLastError(error);
            }
        };
        if (packet == nullptr) {
            observe(returned ? voice_diagnostics::AdmissionReason::NoPacket : voice_diagnostics::AdmissionReason::ReadFailed);
            return nullptr;
        }
        const auto source = fc::read_field<std::uint8_t>(packet, 0xF);
        const auto mode = mode_.load(std::memory_order_acquire);
        const auto host = mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated;
        const auto size = fc::read_field<std::uint16_t>(packet, 6);
        observation.source = source;
        observation.mode = static_cast<std::uint8_t>(mode);
        observation.payload_bytes = size;
        // NetPkt stores payload length at +6, payload at +12 and authenticated source at +15.
        // The qualified intake requires 22 bytes for the native voice header and eight-byte trailer.
        auto reason = voice_diagnostics::AdmissionReason::Accepted;
        if (size < 22) {
            reason = voice_diagnostics::AdmissionReason::ShortPacket;
        } else if (host) {
            observation.source_flags = server_peers_.flags(source);
            if (!can_send_voice(observation.source_flags)) {
                reason = voice_diagnostics::AdmissionReason::SendUnavailable;
            }
        } else {
            observation.active = voice_active();
            if (!observation.active) {
                reason = voice_diagnostics::AdmissionReason::Inactive;
            } else if (source != kHostSlot) {
                reason = voice_diagnostics::AdmissionReason::WrongSource;
            }
        }
        if (reason != voice_diagnostics::AdmissionReason::Accepted) {
            observe(reason);
            static_cast<void>(packet_release_.call(packet));
            continue;
        }
        // Native local microphone packets never reach this network-only call site.
        static_cast<void>(packet_reset_.call(packet));
        std::array<std::uint32_t, 4> audio{};
        static_cast<void>(packet_audio_.call(audio.data(), packet));
        const auto parsed = read_header_.call(audio.data(), audio.data()).value_or(false);
        const auto speaker = parsed ? header_sender_.call(audio.data()).value_or(kInvalidPlayer) : kInvalidPlayer;
        const auto identity = fc::read_field<std::uint64_t>(packet, 12 + size - 8);
        observation.parsed = parsed;
        observation.speaker = speaker;
        observation.received_identity = identity;
        // Preserve the admission checks' short-circuit order, including the single owner lookup.
        if (!parsed) {
            reason = voice_diagnostics::AdmissionReason::HeaderRejected;
        } else if (speaker >= kMaximumPlayers) {
            reason = voice_diagnostics::AdmissionReason::InvalidSpeaker;
        } else if (host && source != speaker) {
            reason = voice_diagnostics::AdmissionReason::WrongSpeaker;
        } else if (!host && mode != fc::GameMode::Client) {
            reason = voice_diagnostics::AdmissionReason::WrongMode;
        } else if (identity == 0) {
            reason = voice_diagnostics::AdmissionReason::ZeroIdentity;
        } else {
            observation.expected_identity = audio_identity(speaker);
            observation.expected_evaluated = 1;
            if (identity != observation.expected_identity) {
                reason = voice_diagnostics::AdmissionReason::StaleIdentity;
            }
        }
        if (reason == voice_diagnostics::AdmissionReason::Accepted) {
            synchronize_receive_owners();
            gNetworkIdentity = identity;
            // The native header's aligned audio length excludes the trailer; relay will append it to its new carrier.
            fc::write_field(packet, 6, static_cast<std::uint16_t>(size - 8));
            observe(reason);
            return packet;
        }
        observe(reason);
        static_cast<void>(packet_release_.call(packet));
    }
}

bool Handler::send_network_audio(fc::NativeFunction<SendNetworkPacket> original, std::uint8_t destination, void *packet,
                                 std::uint32_t header_bytes) noexcept {
    const auto speaker = gRelaySource != kInvalidPlayer ? gRelaySource : gCaptureSource;
    const auto owner = audio_identity(speaker);
    const auto identity = gRelaySource != kInvalidPlayer && gNetworkIdentity != 0 ? gNetworkIdentity : owner;
    if (identity == 0 || identity != owner) {
        static_cast<void>(packet_release_.call(packet));
        return false;
    }
    const auto size = fc::read_field<std::uint16_t>(packet, 6);
    // SendVoice allocates the native 1024-byte pool packet; the aligned codec prefix and header leave room for eight bytes.
    fc::write_field(packet, 12 + size, identity);
    fc::write_field(packet, 6, static_cast<std::uint16_t>(size + 8));
    return original.call(destination, packet, header_bytes).value_or(false);
}

float Handler::hud_last_activity(std::uint8_t player, voice_diagnostics::ActivityObservation *observation) noexcept {
    const auto local = local_player_.load(std::memory_order_acquire);
    auto *transport = get_voice_transport_.call().value_or(nullptr);
    // Native row maintenance asks only for a timestamp, so it must apply the same permission as row insertion.
    if (player >= kMaximumPlayers || transport == nullptr || !hud_permission(transport, local, player)) {
        return -2.0F;
    }
    if (player == local) {
        const auto found = find_voice_node_.call();
        auto *node = found.value_or(nullptr);
        if (observation != nullptr) {
            observation->last_node = reinterpret_cast<std::uintptr_t>(node);
            observation->last_lookup = !found ? 3 : node != nullptr ? 1 : 0;
        }
        return node != nullptr ? fc::read_field<float>(node, 0x20) : -2.0F;
    }
    return received_speech_[player].time.load(std::memory_order_relaxed);
}

bool Handler::hud_activity(void *transport, std::uint8_t player) noexcept {
    voice_diagnostics::ActivityObservation observation{.transport = reinterpret_cast<std::uintptr_t>(transport),
                                                       .player = player,
                                                       .clock_available = static_cast<std::uint8_t>(voice_time_ != nullptr)};
    const auto finish = [&](bool result, voice_diagnostics::ActivityReason reason) noexcept {
        if (diagnostics_enabled()) {
            observation.result = result;
            observation.reason = reason;
            const auto error = GetLastError();
            diagnostics_.activity(&observation);
            SetLastError(error);
            auto ui = ui_observation(voice_diagnostics::UiQuery::HudActivity, player);
            ui.context = observation.transport;
            ui.result = result;
            ui.reason = result ? voice_diagnostics::UiReason::Complete : voice_diagnostics::UiReason::Inactive;
            observe_ui(ui);
        }
        return result;
    };
    if (voice_time_ == nullptr) {
        return finish(false, voice_diagnostics::ActivityReason::NoClock);
    }
    const auto count = get_num_nodes_.call(transport);
    observation.count_evaluated = 1;
    observation.count_call_failed = !count;
    observation.node_count = count.value_or(0);
    if (player >= observation.node_count) {
        return finish(false,
                      count ? voice_diagnostics::ActivityReason::OutsideNodeRange : voice_diagnostics::ActivityReason::CountUnavailable);
    }
    const auto last = hud_last_activity(player, &observation);
    observation.last = last;
    observation.local_player = local_player_.load(std::memory_order_acquire);
    observation.local_source = player == observation.local_player;
    bool valid{};
    if (observation.local_source) {
        const auto found = find_voice_node_.call();
        auto *node = found.value_or(nullptr);
        observation.valid_node = reinterpret_cast<std::uintptr_t>(node);
        observation.valid_lookup = !found ? 3 : node != nullptr ? 1 : 0;
        observation.local_sequence = node != nullptr ? fc::read_field<std::uint16_t>(node, 0x3A) : 0;
        valid = node != nullptr && (observation.local_sequence != 0 || last > 0.0F);
    } else {
        valid = last >= 0.0F;
        // Retain transport metrics for diagnostics without treating forwarded packets as locally heard speech.
        if (diagnostics_enabled() && receive_metrics_ != nullptr && *receive_metrics_ != 0) {
            observation.metrics_available = 1;
            observation.active = fc::read_field<std::uint8_t>(reinterpret_cast<void *>(*receive_metrics_ + player * 0x24), 0x20) & 1U;
        }
    }
    observation.valid = valid;
    observation.now = *voice_time_;
    const auto result = valid && observation.now >= last && observation.now - last <= 1.0F;
    const auto reason = !valid                       ? voice_diagnostics::ActivityReason::InvalidActivity
                        : !(observation.now >= last) ? voice_diagnostics::ActivityReason::ClockBeforeActivity
                        : !result                    ? voice_diagnostics::ActivityReason::Expired
                                                     : voice_diagnostics::ActivityReason::Complete;
    return finish(result, reason);
}

bool Handler::hud_permission(void *transport, std::uint8_t receiver, std::uint8_t source) noexcept {
    if (receiver == source && source == local_player_.load(std::memory_order_acquire)) {
        auto observed = ui_observation(voice_diagnostics::UiQuery::HudPermission, source, receiver);
        observed.context = reinterpret_cast<std::uintptr_t>(transport);
        const auto active = voice_active();
        const auto flags = active ? local_flags_.load(std::memory_order_acquire) : observed.local_flags;
        bool result{};
        if (active && can_send_voice(flags)) {
            const auto count = get_num_nodes_.call(transport).value_or(0);
            for (std::uint8_t destination = 0; destination < count; ++destination) {
                if (destination != source && send_possible(send_possible_original_, transport, source, destination)) {
                    result = true;
                    break;
                }
            }
        }
        observed.local_flags = flags;
        observed.player_flags = flags;
        observed.result = result;
        observed.reason = !active   ? voice_diagnostics::UiReason::Inactive
                          : !result ? voice_diagnostics::UiReason::SendUnavailable
                                    : voice_diagnostics::UiReason::Complete;
        observe_ui(observed);
        return result;
    }
    return receive_possible(receive_possible_original_, transport, receiver, source);
}

void Handler::send_audio(fc::NativeFunction<SendAudioPacket> original, void *node, AudioPacket *packet) noexcept {
    // The adapter accepts complete 198-byte frames; zero-size native discontinuity packets are dropped here.
    if (packet == nullptr || packet->data == nullptr || packet->size != kNativeFrameBytes) {
        return;
    }

    const auto frame = std::span<const std::byte, kNativeFrameBytes>{static_cast<const std::byte *>(packet->data), kNativeFrameBytes};
    const auto size = wire_size(frame);
    if (!size) {
        return;
    }

    // The native send consumes this view before returning; the stream still owns its payload and app-data.
    AudioPacket wire{
        .data = packet->data,
        .size = static_cast<std::uint32_t>(*size),
        .app_data = packet->app_data,
        .discontinuity = packet->discontinuity,
    };
    // This native call builds and submits the captured node's carriers synchronously; relay calls have no test scope.
    const auto player = node != nullptr ? fc::read_field<std::uint8_t>(node, kVoicePlayerOffset) : kInvalidPlayer;
    MicrophoneTestRoute test{player, local_echo_enabled_ != nullptr && *local_echo_enabled_ != 0};
    const auto previous_source = gCaptureSource;
    gCaptureSource = player;
    static_cast<void>(original.call(node, &wire));
    gCaptureSource = previous_source;
}

std::uint8_t Handler::receive_audio(fc::NativeFunction<ReceiveAudioPacket> original, std::uint8_t sender, AudioPacket *packet,
                                    std::uint8_t relay, std::uint16_t missing_packets) noexcept {
    if (packet == nullptr || packet->data == nullptr ||
        (mode_.load(std::memory_order_acquire) == fc::GameMode::Dedicated && server_peers_.flags(sender) == 0)) {
        return 0;
    }

    const auto wire = std::span<const std::byte>{static_cast<const std::byte *>(packet->data), packet->size};
    if (mode_.load(std::memory_order_acquire) == fc::GameMode::Dedicated) {
        // Dedicated intake keeps the original wire bytes and native gap bookkeeping for relay.
        return parse_wire_frame(wire) ? original.call(sender, packet, relay, missing_packets).value_or(0) : 0;
    }

    NativeFrame current{};
    if (!expand_wire_frame(wire, current)) {
        return 0;
    }

    AudioPacket decoded{.data = current.data(),
                        .size = static_cast<std::uint32_t>(current.size()),
                        .app_data = packet->app_data,
                        .discontinuity = packet->discontinuity};
    const auto previous = gReceiveRecovery;
    gReceiveRecovery = {.packet = &decoded, .missing_packets = missing_packets};

    // Routing consumes each native recipient once; recovery is inserted at its selected queue call.
    const auto remaining = original.call(sender, &decoded, relay, std::uint16_t{0}).value_or(0);
    gReceiveRecovery = previous;
    return remaining;
}

void Handler::relay_audio(fc::NativeFunction<RelayAudioPacket> original, AudioPacket *packet, std::uint8_t sender,
                          std::uint16_t sequence) noexcept {
    // Network admission has already checked the authenticated slot and connection identity before native intake.
    if (packet == nullptr || packet->data == nullptr || server_peers_.flags(sender) == 0 ||
        !parse_wire_frame({static_cast<const std::byte *>(packet->data), packet->size})) {
        return;
    }

    // Native relay selects destinations and sends the wire packet. Its nested recipient checks read this thread's sender.
    // Restore the previous sender afterward so a nested relay call does not overwrite its caller's source.
    const auto previous = gRelaySource;
    gRelaySource = sender;
    static_cast<void>(original.call(packet, sender, sequence));
    gRelaySource = previous;
}

bool Handler::select_recipient(fc::NativeFunction<GetRelayRecipient> original, void *transport, std::uint8_t destination) noexcept {
    if (gRelaySource == 0xFF) {
        return original.call(transport, destination).value_or(false);
    }
    const auto native_selected = original.call(transport, destination).value_or(false);
    std::uint8_t exclusions{};
    // Preserve short-circuit policy evaluation and any effects of the native header lookup.
    const auto selected = native_selected && route_allowed(gRelaySource, destination, exclusions);
    if (diagnostics_enabled()) {
        const voice_diagnostics::RecipientObservation observation{.destination = destination,
                                                                  .native_selected = static_cast<std::uint8_t>(native_selected),
                                                                  .policy_evaluated = static_cast<std::uint8_t>(native_selected),
                                                                  .exclusions = exclusions};
        const auto error = GetLastError();
        diagnostics_.recipient(&observation);
        SetLastError(error);
    }
    return selected;
}

bool Handler::enqueue_audio(void *node, std::uint8_t sender, AudioPacket *packet) noexcept {
    const auto accepted = enqueue_audio_original_.call(node, sender, packet).value_or(false);
    observe_enqueue(node, sender, packet, accepted);
    if (accepted && sender < kMaximumPlayers && packet != nullptr && packet->data != nullptr && packet->size == kNativeFrameBytes &&
        voice_time_ != nullptr && node == find_voice_node_.call().value_or(nullptr)) {
        const auto frame = parse_native_frame(
            std::span<const std::byte, kNativeFrameBytes>{static_cast<const std::byte *>(packet->data), kNativeFrameBytes});
        // Recovery and empty discontinuities can enter this queue without new speech from the sender.
        if (frame && frame->mode == FrameMode::Normal) {
            received_speech_[sender].time.store(*voice_time_, std::memory_order_relaxed);
        }
    }
    return accepted;
}

void Handler::observe_enqueue(void *node, std::uint8_t sender, const AudioPacket *packet, bool accepted) const noexcept {
    if (packet == nullptr || !diagnostics_enabled()) {
        return;
    }
    std::uint8_t recovery{};
    if (packet->data != nullptr && packet->size >= kFrameHeaderBytes) {
        recovery = std::to_integer<std::uint8_t>(static_cast<const std::byte *>(packet->data)[5]);
    }
    const voice_diagnostics::EnqueueObservation observation{.node = reinterpret_cast<std::uintptr_t>(node),
                                                            .packet = reinterpret_cast<std::uintptr_t>(packet),
                                                            .payload_bytes = packet->size,
                                                            .sender = sender,
                                                            .recovery = recovery,
                                                            .discontinuity = packet->discontinuity,
                                                            .accepted = static_cast<std::uint8_t>(accepted)};
    const auto error = GetLastError();
    diagnostics_.enqueue(&observation);
    SetLastError(error);
}

} // namespace fc_plugins::voice_chat
