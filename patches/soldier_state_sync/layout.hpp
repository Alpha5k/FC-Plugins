#pragma once

#include <fc/native.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace fc_plugins::soldier_state_sync {

using SendUpdate = void(__cdecl *)(std::int32_t destination) noexcept;
using Update = void(__cdecl *)(void *group) noexcept;
// Complete Soldier codecs take the group and object state on the stack, consuming both words.
using SoldierObject = void(__thiscall *)(void *soldier, void *group, void *object_state) noexcept;
// Native sender promotes destination/type to ECX/EDX and keeps the stream pointer on the stack.
using GroupSend = fc::NativeCall<void(std::int32_t destination, std::int32_t type, void *group) noexcept,
                                 fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;
// Native retrieval uses ECX for the output group and EDX for the owned incoming packet.
using GetGroup = bool(__fastcall *)(void *group, void *packet) noexcept;
// FIFO retrieval accepts either type in ECX/EDX and owns the removed packet.
using ReceivePacket = void *(__fastcall *)(std::int32_t first, std::int32_t second) noexcept;
using GetPacket = void *(__fastcall *)(std::int32_t index) noexcept;
using FreePacket = void(__fastcall *)(void *packet) noexcept;
// Queue removal consumes a packet pointer from the stack and the owning group in ECX.
using RemovePacket = void(__thiscall *)(void *group, void *packet) noexcept;
using Connect =
    fc::NativeCall<void(std::uint32_t slot, const void *address) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx>>>;
using Disconnect = fc::NativeCall<void(std::uint32_t slot) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using FreeGroup = void(__thiscall *)(void *group) noexcept;
// The native Trigger wrappers take the group in ECX and Trigger word in EDX, with no stack arguments.
using Trigger = void(__fastcall *)(void *group, std::uint32_t *trigger) noexcept;
using Energy = void(__thiscall *)(void *energy, void *group) noexcept;
using WriteMelee = void(__thiscall *)(void *weapon, void *group) noexcept;
using WriteBits = void(__thiscall *)(void *group, std::uint32_t value, std::int32_t count) noexcept;
using ReadBits = void(__thiscall *)(void *group, std::uint32_t *value, std::int32_t count) noexcept;

// Receive allocation must reserve the same three header bytes for all update packet formats.
struct ReceivePrefix {
    fc::CodeLocation compare;
    fc::CodeLocation branch;
    fc::DataLocation<std::uint8_t> type;
};

// Exact native boundaries keep the format change local to Soldier update groups.
struct Layout {
    fc::FunctionLocation<SendUpdate> send_update;
    fc::FunctionLocation<Update> write_update;
    fc::FunctionLocation<Update> read_update;
    fc::FunctionLocation<GroupSend> send_group;
    fc::FunctionLocation<GetGroup> get_group;
    fc::FunctionLocation<ReceivePacket> receive_packet;
    fc::FunctionLocation<GetPacket> get_packet;
    fc::FunctionLocation<FreePacket> free_packet;
    fc::FunctionLocation<RemovePacket> remove_packet;
    fc::FunctionLocation<Connect> connect;
    fc::FunctionLocation<Disconnect> disconnect;
    fc::DataLocation<std::byte, 64 * 0x60> endpoints;
    fc::FunctionLocation<FreeGroup> free_group;
    fc::FunctionLocation<WriteBits> write_bits;
    fc::FunctionLocation<ReadBits> read_bits;
    fc::CodeLocation write_velocity;
    fc::CodeLocation read_velocity;
    fc::CodeLocation write_state_clock;
    fc::CodeLocation read_state_clock;
    fc::FunctionLocation<Trigger> write_trigger;
    fc::FunctionLocation<Trigger> read_trigger;
    fc::FunctionLocation<WriteMelee> write_melee;
    fc::FunctionLocation<SoldierObject> write_soldier;
    fc::FunctionLocation<SoldierObject> read_soldier;
    fc::DataLocation<const std::byte, 4> melee_vtable;
    fc::DataLocation<const std::int32_t> update_turn;
    fc::CodeLocation write_heat;
    fc::CodeLocation read_heat;
    fc::CodeLocation write_energy;
    fc::CodeLocation read_energy;
    fc::CodeLocation energy_write_count;
    fc::CodeLocation energy_read_count;
    fc::CodeLocation receive_match;
    fc::CodeLocation receive_type;
    std::array<ReceivePrefix, 2> receive_prefix;
    fc::CodeLocation send_fragment;
    fc::CodeLocation single_fragment;
    fc::CodeLocation group_identity;
    fc::CodeLocation next_group;
    std::array<fc::CodeLocation, 2> order;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::soldier_state_sync
