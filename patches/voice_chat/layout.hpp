#pragma once

#include "transport.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::voice_chat {

// The optimized retail helper takes milliseconds in XMM0 and returns samples in EAX.
using CalculateSamples = fc::NativeCall<std::uint32_t(float milliseconds) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Xmm0>>>;

// The GSM thiscall state consumes 33-byte encoded blocks or 160-sample PCM16 blocks; Init resets that state.
using GsmDecode = void(__thiscall *)(void *state, const std::byte *encoded, std::int16_t *pcm) noexcept;
using GsmEncode = void(__thiscall *)(void *state, const std::int16_t *pcm, std::byte *encoded) noexcept;
using GsmInit = void(__thiscall *)(void *state) noexcept;
// Native close detaches the stream pipeline and releases its buffers before returning.
using VoiceStreamClose = void(__thiscall *)(void *stream) noexcept;

// Capture takes packet bytes and count; playback takes a buffer duration in milliseconds.
using CaptureSourceOpen = bool(__thiscall *)(void *source, std::uint8_t device, void *format, std::uint32_t packet_bytes,
                                             std::uint8_t packet_count, void *packets) noexcept;
using AuxRendererOpen = bool(__thiscall *)(void *renderer, std::uint8_t device, void *format, float buffer_ms) noexcept;

// Profile and options pointers are borrowed game storage; ApplySettings and ResetToDefault receive the embedded options object.
using GetCurrentProfile = std::byte *(__cdecl *)() noexcept;
using OnlineOptionsMethod = void(__thiscall *)(void *options) noexcept;

// OpenStatic derives milliseconds as (2 / open_rate) * 1000; open_devices preserves the native caller's audio mode.
using GameVoiceOpen =
    fc::NativeCall<bool(float open_rate, bool open_devices) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Xmm0, fc::abi::Ecx>>>;
using GameVoiceClose = void(__cdecl *)() noexcept;
using GameVoiceIsOpen = bool(__cdecl *)() noexcept;
using FindVoiceNode = void *(__cdecl *)() noexcept;
// Native player queries take the transport and one voice ID, returning a Boolean in AL.
using VoicePlayerQuery = bool(__thiscall *)(void *transport, std::uint8_t player) noexcept;
// Node::GetID and SendAudioPacket read this byte in both retail builds.
inline constexpr std::size_t kVoicePlayerOffset = 0x3C;
using SendAudioPacket = void(__thiscall *)(void *node, AudioPacket *packet) noexcept;
// Copies payload into one local node's receive queue; false retains native backpressure/discontinuity behavior.
using EnqueueAudioPacket = bool(__thiscall *)(void *node, std::uint8_t sender, AudioPacket *packet) noexcept;
// Retail VoiceLocalEchoEnable receives its Boolean in CL and updates the native test flag only when voice is open.
using LocalEchoEnable = fc::NativeCall<void(bool enabled) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// This optimized static function uses ECX and EDX for its first two arguments,
// followed by two stack arguments that its caller releases. Its byte result counts remaining recipients.
using ReceiveAudioPacket =
    fc::NativeCall<std::uint8_t(std::uint8_t sender, AudioPacket *packet, std::uint8_t relay, std::uint16_t missing_packets) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>, fc::abi::Stack<4>>>>;

// Relay receives packet and sender in ECX/EDX and a 10-bit sequence in its caller-cleaned stack argument.
using RelayAudioPacket = fc::NativeCall<void(AudioPacket *packet, std::uint8_t sender, std::uint16_t sequence) noexcept,
                                        fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;

// This helper reads the native header's recipient bitmap; the first argument is that header.
using GetRelayRecipient = bool(__stdcall *)(void *transport, std::uint8_t destination) noexcept;

// Send queries use (source, destination); receive queries use (receiver, source).
using VoicePossible = bool(__thiscall *)(void *transport, std::uint8_t source, std::uint8_t destination) noexcept;
using SetSending = void(__stdcall *)(std::uint8_t player, bool speaking) noexcept;
// The static native tick receives elapsed seconds in XMM0 and completes capture, intake and local decode.
using VoiceUpdate = fc::NativeCall<void(float seconds) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Xmm0>>>;
using GetLobbyPlayerIndex = std::int32_t(__fastcall *)(std::int32_t lobby_index) noexcept;

// Node device open/close clear transmit sequence and last-send time; the hooks retain that actual activity for the same node.
using NodeOpen = bool(__thiscall *)(void *node, std::uint32_t capture, std::uint32_t playback, std::uint32_t flags) noexcept;
using NodeClose = void(__thiscall *)(void *node, std::uint8_t send, std::uint8_t receive) noexcept;
using DecodeStream = bool(__thiscall *)(void *node, std::uint8_t sender, AudioPacket *packet) noexcept;
using FlushPacket = bool(__thiscall *)(void *node, void *queue, AudioPacket *packet) noexcept;
using GetNumNodes = std::uint8_t(__thiscall *)(void *transport) noexcept;
// These network calls return/consume the native packet; NetComm::Send's two stack arguments are caller-cleaned.
using GetNetworkPacket =
    fc::NativeCall<void *(std::uint32_t type, std::uint32_t priority) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx>>>;
using SendNetworkPacket = fc::NativeCall<bool(std::uint8_t destination, void *packet, std::uint32_t header_bytes) noexcept,
                                         fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>>>;
using PacketMethod = void(__thiscall *)(void *packet) noexcept;
using GetVoicePacket = void *(__cdecl *)() noexcept;
using PacketAudio = void(__thiscall *)(void *audio, void *packet) noexcept;
using ReadHeader = bool(__stdcall *)(void *transport, void *audio) noexcept;
using HeaderSender = std::uint8_t(__stdcall *)(void *transport) noexcept;
using ClassifyPacket = fc::NativeCall<bool(std::uint32_t type) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using LastReceiveTime =
    fc::NativeCall<float(std::uint8_t player) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>, fc::abi::Result<fc::abi::Xmm0>>>;

// Maps capture, playback, volume, codec, routing and saved-option operations to one exact x86 retail executable.
struct Layout {
    fc::FunctionLocation<CalculateSamples> calculate_samples;
    // MOVSS stores XMM0 into the static packet target in milliseconds.
    fc::CodeLocation packet_duration;
    // Branch around the encoder's 200 ms minimum duration.
    fc::CodeLocation encoder_packet_duration;
    fc::CodeLocation encoder_sample_rate;
    fc::CodeLocation decoder_sample_rate;
    fc::CodeLocation vad_sample_rate;
    // VAD+0x34 stores a PCM byte budget, decremented on below-threshold packets.
    fc::CodeLocation vad_release;
    fc::CodeLocation vad_frequency_filter;
    // SignalPower16 leaves 64-bit mean-square power in EDX:EAX before the native energy comparison.
    fc::CodeLocation vad_power_result;

    // Target anchors validate the two scoped voice stream call sites.
    fc::FunctionLocation<CaptureSourceOpen> capture_source_open;
    fc::CallLocation<CaptureSourceOpen> voice_capture_open;
    fc::FunctionLocation<AuxRendererOpen> aux_renderer_open;
    fc::CallLocation<AuxRendererOpen> voice_playback_open;
    // ADD EAX,EAX doubles the capture ring allocation after the modifier's scale.
    fc::CodeLocation capture_ring_size;

    fc::CodeLocation scale_capture_gain;      // Shared StreamAGC::GetPacket Q15 product shift.
    fc::CodeLocation source_gain_store;       // ECX is the encoder; low XMM1 is the normalized microphone setting.
    fc::CodeLocation playback_renderer_ready; // ESI is the decoder with an opened auxiliary renderer.
    // Final output scaling in StreamMixer's four-sample loop and remainder loop.
    std::array<fc::CodeLocation, 5> scale_playback_gain;

    fc::FunctionLocation<GsmDecode> gsm_decode;
    fc::FunctionLocation<GsmEncode> gsm_encode;
    fc::FunctionLocation<GsmInit> gsm_init;
    fc::FunctionLocation<VoiceStreamClose> encoder_close;
    fc::FunctionLocation<VoiceStreamClose> decoder_close;

    fc::FunctionLocation<SendAudioPacket> send_audio;
    // EAX holds the stored method before SUB EAX,2 dispatches the outgoing packet format.
    fc::CodeLocation send_method_dispatch;
    // CL holds the native current/next network flag before TEST CL,CL selects delivery.
    fc::CodeLocation send_network_dispatch;
    fc::FunctionLocation<LocalEchoEnable> local_echo_enable;
    fc::DataLocation<const std::uint8_t> local_echo_enabled;
    // The native BL store qualifies the test flag address independently of the callback's requested value.
    fc::CodeLocation local_echo_store;
    // Open-time method selection and command-line override; later method setters remain active.
    fc::CodeLocation default_transport_method;
    fc::CodeLocation transport_override;
    // Delivery to every local player mutates the shared read header's relay-format recipient bits.
    fc::FunctionLocation<ReceiveAudioPacket> receive_audio;
    fc::FunctionLocation<EnqueueAudioPacket> enqueue_audio;
    fc::CallLocation<EnqueueAudioPacket> receive_enqueue_call;
    // EAX is the admitted sequence gap before MOVZX EBX,AX and the native cap.
    fc::CodeLocation sequence_gap;
    fc::FunctionLocation<RelayAudioPacket> relay_audio;
    fc::FunctionLocation<GetRelayRecipient> get_relay_recipient;
    fc::CallLocation<GetRelayRecipient> relay_recipient_call;
    // ESI holds the node-count field width before the native INC starts the recipient scan.
    fc::CodeLocation recipient_count_start;

    fc::FunctionLocation<VoicePossible> send_enable;
    fc::FunctionLocation<VoicePossible> receive_enable;
    fc::FunctionLocation<VoicePossible> send_possible;
    fc::FunctionLocation<VoicePossible> receive_possible;
    // Native eligibility check for a player in the speaker HUD.
    fc::CallLocation<VoicePossible> hud_receive_possible;
    fc::CodeLocation hud_font; // Font key passed to the native registry during VoiceChatGroup::Bind.
    fc::CodeLocation hud_row;  // ESI is the text row, EDI its font, and EBX the remaining row count.
    fc::FunctionLocation<SetSending> set_sending;
    fc::FunctionLocation<VoiceUpdate> native_update;

    // Stores CX as a per-sender packet target; independent of capture/playback device buffer duration.
    fc::CodeLocation jitter_target;

    fc::FunctionLocation<GetCurrentProfile> get_current_profile;
    fc::FunctionLocation<OnlineOptionsMethod> apply_online_options;
    fc::FunctionLocation<OnlineOptionsMethod> reset_online_options;
    fc::FunctionLocation<GameVoiceOpen> game_voice_open;
    fc::FunctionLocation<GameVoiceClose> game_voice_close;
    fc::FunctionLocation<GameVoiceIsOpen> game_voice_is_open;
    fc::FunctionLocation<FindVoiceNode> find_voice_node;
    fc::FunctionLocation<FindVoiceNode> get_voice_transport;
    fc::FunctionLocation<VoicePlayerQuery> is_local;
    fc::FunctionLocation<VoicePlayerQuery> send_connected;
    fc::FunctionLocation<VoicePlayerQuery> receive_connected;
    fc::FunctionLocation<VoicePlayerQuery> get_sending;
    fc::FunctionLocation<GetLobbyPlayerIndex> get_lobby_player_index;
    fc::FunctionLocation<NodeOpen> node_open;
    fc::FunctionLocation<NodeClose> node_close;
    fc::FunctionLocation<DecodeStream> decode_stream;
    fc::FunctionLocation<FlushPacket> flush_packet;
    fc::FunctionLocation<GetNumNodes> get_num_nodes;
    fc::CodeLocation sequence_admission;
    fc::CallLocation<VoicePlayerQuery> hud_activity;
    fc::CallLocation<LastReceiveTime> hud_last_activity;
    fc::CallLocation<GetNetworkPacket> network_read;
    fc::FunctionLocation<GetVoicePacket> receive_packet;
    fc::CallLocation<SendNetworkPacket> network_send;
    fc::CallLocation<ClassifyPacket> voice_lookup;
    fc::FunctionLocation<PacketMethod> packet_reset;
    fc::FunctionLocation<PacketMethod> packet_release;
    fc::FunctionLocation<PacketAudio> packet_audio;
    fc::FunctionLocation<ReadHeader> read_header;
    fc::FunctionLocation<HeaderSender> header_sender;
    // Borrowed native global pointers; receive metrics have 0x24-byte rows, and nodes form a sentinel list.
    fc::DataLocation<const std::uintptr_t> receive_metrics;
    fc::DataLocation<const float> voice_time;
    fc::DataLocation<const std::uintptr_t> voice_nodes;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::voice_chat
