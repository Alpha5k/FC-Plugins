#pragma once

#include "history.hpp"

#include <fc/native.hpp>

#include <optional>
#include <string_view>

namespace fc_plugins::input_sync {

// Native Create receives its preserve flag in ECX and resets both local input slots.
using NetworkCreate = fc::NativeCall<void(bool) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

struct Layout {
    fc::CodeLocation retire_received;
    fc::CodeLocation input_selection;
    fc::CodeLocation full_input;
    fc::CodeLocation completed_input;
    fc::CodeLocation prediction_input;
    fc::FunctionLocation<NetworkCreate> create;
    fc::DataLocation<const Row, 32> input_rows;
    fc::DataLocation<const std::uint32_t> current_turn;
    fc::DataLocation<const std::uint32_t> authority_turn;
    fc::DataLocation<const std::uint32_t> prediction_turn;
    fc::DataLocation<const std::uint32_t> turns_per_second;
    fc::DataLocation<const Players> players;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::input_sync
