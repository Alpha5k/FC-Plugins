#pragma once

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::update_scheduling {

inline constexpr std::size_t kMaximumPlayers = 64;

// NetObjStateMap::Alloc uses its free list before falling back to the heap; Free returns maps to that list.
using AllocateObjectState = void *(__cdecl *)() noexcept;
using FreeObjectState = fc::NativeCall<void(void *state) noexcept,
                                       fc::abi::X86<fc::abi::Args<fc::abi::Ecx>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

using ReceivedUpdateAck = void(__cdecl *)(std::int32_t destination, std::uint32_t turn) noexcept;

// NetComm::Create takes preserve in CL and resets netTime for both connection policies.
using NetworkCreate = fc::NativeCall<void(bool preserve) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// Groups the host scheduling boundaries with the object state used to preserve CREATE ordering.
struct Layout {
    fc::CodeLocation dedicated_render;
    fc::CodeLocation client_visit_count;

    fc::CodeLocation acknowledgement_guard;
    fc::CodeLocation acknowledgement_resume;

    fc::CodeLocation untracked_acknowledgement;
    fc::CodeLocation round_trip_update;

    fc::CodeLocation create_marker;

    fc::CodeLocation send_window_result;
    fc::CodeLocation send_window;
    fc::CodeLocation continue_destination_checks;
    fc::CodeLocation next_destination;

    fc::CodeLocation next_update_turn;

    fc::DataLocation<const std::int32_t> current_destination;
    fc::DataLocation<const std::uint32_t> host_turn;
    fc::DataLocation<const std::int32_t> turn_rate;

    std::array<fc::DataLocation<void *>, kMaximumPlayers> pending_states; // NetPlayer::nextObjState pointers.

    std::array<fc::DataLocation<const float, 2>, kMaximumPlayers> round_trip_estimates; // Mean and deviation, in seconds.
    fc::DataLocation<const float> network_time;                                         // netTime seconds, reset by native world creation.

    fc::FunctionLocation<AllocateObjectState> allocate_object_state;
    fc::FunctionLocation<FreeObjectState> free_object_state;

    fc::FunctionLocation<ReceivedUpdateAck> received_update_ack;
    fc::FunctionLocation<NetworkCreate> network_create;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::update_scheduling
