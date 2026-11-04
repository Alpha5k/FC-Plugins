#pragma once

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::collision_history_fix {

// Retail Predict and PredictTurn are cdecl; Predict's result compares completed turns after world work.
using PredictFunction = bool(__cdecl *)() noexcept;
using PredictTurnFunction = void(__cdecl *)() noexcept;
// Native simulation receives Soldier+0x240 in ECX and elapsed seconds on the stack, and returns AL.
using UpdateFunction = bool(__thiscall *)(void *controllable, float delta) noexcept;
// ReadObject receives the complete Soldier in ECX and two stack arguments, consumed by RET 8.
using ReadFunction = void(__thiscall *)(void *soldier, void *stream, void *state) noexcept;
// ResetObject and the CollisionObject destructor receive their respective native this pointer in ECX.
using ResetFunction = void(__thiscall *)(void *object) noexcept;
// GetJoystickIndex maps a native player index to local input slot 0/1, or -1, using cdecl.
using GetJoystickIndex = std::int32_t(__cdecl *)(std::int32_t player) noexcept;
// IsSkipped receives the complete GameObject and may replace elapsed seconds; its boolean result is in AL.
using IsSkipped = bool(__cdecl *)(void *object, float *delta) noexcept;
// CollisionCallback receives CollisionObject in ECX, consumes three stack arguments and returns AL.
using CollisionResponse = bool(__thiscall *)(void *object, void *result, void *other, void *restrictor) noexcept;

// Selects native movement boundaries, turn counters and client-world flags for one retail image.
struct Layout {
    fc::FunctionLocation<PredictFunction> predict;
    fc::FunctionLocation<PredictTurnFunction> predict_turn;
    fc::FunctionLocation<UpdateFunction> update;
    fc::FunctionLocation<ReadFunction> read;
    fc::FunctionLocation<ResetFunction> reset;
    fc::FunctionLocation<ResetFunction> destroy_collision_object;
    fc::FunctionLocation<GetJoystickIndex> get_joystick_index;
    fc::FunctionLocation<IsSkipped> is_skipped;
    fc::FunctionLocation<CollisionResponse> collision_response;
    fc::CodeLocation pair_admission;
    fc::CodeLocation accept_pair;
    fc::CodeLocation soldier_class_getter;
    fc::DataLocation<const std::int32_t> prediction_turn;
    fc::DataLocation<const std::int32_t> update_turn;
    fc::DataLocation<const std::int32_t> local_prediction_turn;
    fc::DataLocation<std::int32_t> max_update_steps;
    fc::DataLocation<const std::uint8_t> local_prediction_pass;
    fc::DataLocation<const std::uint8_t> late_prediction_pass;
    fc::DataLocation<const std::uint8_t> network_enabled;
    fc::DataLocation<const std::uint8_t> network_client_active;
    std::array<fc::CodeLocation, 2> contexts;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::collision_history_fix
