#pragma once

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::voice_diagnostics {

// Borrows the native audio descriptor and payload during a synchronous callback; only x86 retail variants are supported.
struct AudioPacket {
    void *data{};                 // Borrowed payload bytes, sometimes carried by a stack descriptor.
    std::uint32_t size{};         // Payload byte count; zero can mark a discontinuity flush.
    void *app_data{};             // Native release owner; diagnostics never changes or uses it as frame identity.
    std::uint8_t discontinuity{}; // Raw flags; native consumers test bit zero and preserve other bits.
    std::array<std::byte, 3> padding{};
};

static_assert(sizeof(AudioPacket) == 0x10);
static_assert(offsetof(AudioPacket, discontinuity) == 0x0C);

// Retail uses fixed header/transport objects and passes the transport and NetPacketAudio wrapper on the stack.
using ReadHeader = bool(__stdcall *)(void *transport, void *packet) noexcept;
using ReceivePacket = void *(__cdecl *)() noexcept;
// Retail ignores the relay stack argument; the result counts recipients remaining after local delivery.
using ReceiveAudio =
    fc::NativeCall<std::uint8_t(std::uint8_t sender, AudioPacket *packet, std::uint8_t relay, std::uint16_t missing_packets) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>, fc::abi::Stack<4>>>>;
using RelayAudio = fc::NativeCall<void(AudioPacket *packet, std::uint8_t sender, std::uint16_t sequence) noexcept,
                                  fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;
using GetMethodType = std::int32_t(__thiscall *)(void *method) noexcept;
using GetNumNodes = std::uint8_t(__thiscall *)(void *transport) noexcept;
using GetReceiveNode = bool(__stdcall *)(void *transport, std::uint8_t node) noexcept;
using SendRelayPacket = fc::NativeCall<bool(AudioPacket *packet, std::uint8_t destination, float *last_send_time) noexcept,
                                       fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>>>;
using SendVoicePacket = fc::NativeCall<bool(void *packet, std::uint8_t destination, std::uint32_t header_bytes) noexcept,
                                       fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;
using DecoderReceivePacket = bool(__thiscall *)(void *decoder, std::uint32_t stream, AudioPacket *packet) noexcept;
using UpdateLatency = std::uint32_t(__thiscall *)(void *node, std::uint8_t stream) noexcept;
using DecodeStream = bool(__thiscall *)(void *node, std::uint8_t sender, AudioPacket *packet) noexcept;
using NodeClose = void(__thiscall *)(void *node, std::uint8_t send, std::uint8_t receive) noexcept;
using FlushPacket = bool(__thiscall *)(void *node, void *sender_queue, AudioPacket *marker) noexcept;

// Describes the stock intake, relay, decoder-queue, and jitter boundaries shared by retail clients and servers.
struct Layout {
    fc::FunctionLocation<ReceivePacket> receive_packet;
    fc::CallLocation<ReadHeader> read_header_call;
    fc::CallLocation<ReceiveAudio> receive_audio_call;
    fc::FunctionLocation<RelayAudio> relay_audio;
    fc::CallLocation<GetMethodType> relay_method_type_call;
    fc::CallLocation<GetNumNodes> relay_node_count_call;
    fc::CallLocation<GetReceiveNode> relay_recipient_call;
    fc::CallLocation<SendRelayPacket> send_relay_call; // Method-4 carrier construction/send attempt, distinct from submission.
    fc::FunctionLocation<SendVoicePacket> send_voice_packet;
    fc::FunctionLocation<DecoderReceivePacket> decoder_receive_packet;
    fc::FunctionLocation<UpdateLatency> update_latency; // Returns the target packet count after VoiceChat's register clamp.
    fc::CodeLocation admitted_identity;                 // EBP-0xC contains the parsed ten-bit sequence before local delivery.
    fc::FunctionLocation<DecodeStream> decode_stream;
    fc::FunctionLocation<NodeClose> node_close;
    fc::CodeLocation queue_retired; // EDX=node, ESI=sender queue, EDI=retiring descriptor, BL=effective receive policy.
    fc::FunctionLocation<FlushPacket> flush_packet;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::voice_diagnostics
