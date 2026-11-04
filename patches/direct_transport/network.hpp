#pragma once

#include <fc/native.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::direct_transport {

// NetSocket::SendSocket takes destination in ECX, bytes in EDX and length on the caller-clean stack.
// Returns payload length, -1 on refusal, or 0 if the Galaxy facade is unavailable, in EAX.
using FinalSend = fc::NativeCall<
    std::int32_t(std::int32_t destination, const void *bytes, std::int32_t length) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CallerCleanup>>;

// NetComm::Send takes destination in ECX, type in EDX and group on the caller-clean stack.
// Its fragment sends can synchronously run DoRecvs before the group returns.
using GroupSend = fc::NativeCall<
    void(std::int32_t destination, std::int32_t packet_type, void *group) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CallerCleanup>>;

// DoRecvs takes no arguments; FC and diagnostics observe the same native receive service.
using ReceiveService = fc::NativeCall<void() noexcept, fc::abi::X86<fc::abi::Args<>>>;

// RecvPacket takes the pool packet in ECX and a borrowed 24-byte source address in EDX.
// It owns the packet and may queue or free it; returning does not report gameplay acceptance.
using PacketIntake = void(__fastcall *)(void *packet, void *endpoint) noexcept;

// Locates the final sender, packet-group sender, receive service and packet intake.
struct NetworkLayout {
    fc::FunctionLocation<FinalSend> final_send;
    fc::FunctionLocation<GroupSend> group_send;
    fc::FunctionLocation<ReceiveService> receive_service;
    fc::FunctionLocation<PacketIntake> packet_intake;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<NetworkLayout> select_network_layout(std::string_view profile);

} // namespace fc_plugins::direct_transport
