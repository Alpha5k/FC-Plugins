#pragma once

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::late_update_recovery {

// Construction receives its group in ECX; receive uses ECX/EDX, and update parsing receives a stack argument.
using ConstructGroup = fc::NativeCall<void *(void *group) noexcept,
                                      fc::abi::X86<fc::abi::Args<fc::abi::Ecx>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using ReceiveUpdate = void(__cdecl *)(void *group) noexcept;
using GetLastReceive = fc::NativeCall<float *(std::int32_t pipe) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using ReceiveGroup = bool(__fastcall *)(void *group, std::int32_t packet_type) noexcept;
using GetUpdateTurn = std::uint32_t(__cdecl *)(void *group) noexcept;

// Describes the native receive path and host activity timer sites.
struct Layout {
    fc::CodeLocation pipe_state_capture;
    fc::CodeLocation timestamp_capture;
    fc::CodeLocation stock_drain;
    fc::CodeLocation receive_resume;

    fc::FunctionLocation<ConstructGroup> construct_group;
    fc::FunctionLocation<ReceiveUpdate> receive_update;
    fc::FunctionLocation<GetLastReceive> get_last_receive;
    fc::FunctionLocation<ReceiveGroup> receive_group;
    fc::FunctionLocation<GetUpdateTurn> get_update_turn;

    fc::CodeLocation timer_refresh;

    fc::DataLocation<const float> host_timeout_extension; // Native allowance added to the captured network time.
    fc::DataLocation<float> host_timeout_deadline;        // hostCycleTime; separate from the disconnect timeout.
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::late_update_recovery
