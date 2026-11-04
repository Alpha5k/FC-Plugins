#pragma once

#include <fc/native.hpp>
#include <cstdint>
#include <optional>
#include <array>
#include <string_view>

namespace fc_plugins::sound_diagnostics {

using Update = void(__thiscall *)(void *) noexcept;
using WriteData = std::uint32_t(__thiscall *)(void *, void *, std::uint32_t, std::int32_t) noexcept;
using SetFormat = bool(__thiscall *)(void *, std::uint32_t, std::uint32_t, std::uint32_t, bool, float, bool, void *, std::uint32_t,
                                     bool) noexcept;
using GamePlay = void(__thiscall *)(void *, void *, void *, bool, bool) noexcept;
using SoundPlay = void *(__cdecl *)(void *, void *, void *, void *, bool) noexcept;
using SetupVoice = void(__thiscall *)(void *, void *, void *, void *) noexcept;
using StartVoice = void(__thiscall *)(void *, bool) noexcept;
using Inactive = void(__cdecl *)(void *, void *) noexcept;
using ControllerPlay = void(__thiscall *)(void *, void *, void *, void *, bool) noexcept;
using LayeredPlay = void(__cdecl *)(void *, void *) noexcept;
using LayeredBind = void(__thiscall *)(void *, void *, void *, float) noexcept;
using StreamPlay = void(__cdecl *)(void *, void *, bool) noexcept;
using StreamSetup = bool(__thiscall *)(void *, void *, void *) noexcept;
using AssignVoice = void(__thiscall *)(void *, void *) noexcept;
using StreamConnect = void(__thiscall *)(void *, void *) noexcept;
using DevicePlay = bool(__thiscall *)(void *, bool, std::int32_t, bool, bool) noexcept;
using DeviceStop = std::int32_t(__thiscall *)(void *) noexcept;
using ControllerStop = void(__thiscall *)(void *, bool) noexcept;
using GetPacket = void *(__thiscall *)(void *, std::uint32_t) noexcept;
using GlobalUpdate = void(__cdecl *)() noexcept;
using MeleeRead = void(__thiscall *)(void *, void *) noexcept;
using MeleeControls = bool(__thiscall *)(void *) noexcept;
// Actual native cleanup: reset/assignment RET4, name/resolver RET0C, emitter RET10, state0 RET.
using BindingAssign = void *(__thiscall *)(void *, void *) noexcept;
using EffectEmit = void(__thiscall *)(void *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
using GroundReturn = std::uint32_t(__thiscall *)(void *) noexcept;
using Toggle = void(__thiscall *)(void *, bool) noexcept;

// Retail methods supply live borrowed objects at qualified lifecycle and service boundaries.
struct Layout {
    fc::FunctionLocation<Update> renderer_update;
    fc::FunctionLocation<Update> renderer_flush;
    fc::FunctionLocation<WriteData> renderer_write;
    fc::FunctionLocation<SetFormat> renderer_format;
    fc::CodeLocation mixer_created;
    fc::CodeLocation mixer_closing;
    fc::CodeLocation renderer_created;
    fc::CodeLocation renderer_closing;
    fc::CodeLocation position_result;
    fc::CodeLocation consumed;
    fc::CodeLocation cursor_reset;
    fc::CodeLocation requested;
    fc::CodeLocation lock_result;
    fc::CodeLocation unlock_result;
    fc::CodeLocation queue_exhausted;
    fc::CodeLocation zero_fill;
    fc::FunctionLocation<GamePlay> game_play;
    fc::FunctionLocation<SoundPlay> sound_play;
    fc::FunctionLocation<SetupVoice> setup_voice;
    fc::FunctionLocation<StartVoice> voice_start;
    fc::FunctionLocation<Update> voice_stop;
    fc::FunctionLocation<Inactive> voice_inactive;
    fc::FunctionLocation<ControllerPlay> controller_play;
    fc::FunctionLocation<ControllerStop> controller_stop;
    fc::CodeLocation owner_wait_before;
    fc::CodeLocation owner_wait_after;
    fc::CodeLocation owner_sleep_before;
    fc::CodeLocation owner_sleep_after;
    fc::CodeLocation game_permission;
    fc::CodeLocation ordinary_property;
    fc::CodeLocation ordinary_sample;
    fc::CodeLocation ordinary_gain;
    fc::CodeLocation ordinary_voice;
    fc::FunctionLocation<LayeredPlay> layered_play;
    fc::FunctionLocation<LayeredBind> layered_bind;
    fc::CodeLocation layered_due;
    fc::CodeLocation layered_due_end;
    fc::FunctionLocation<StreamPlay> stream_play;
    fc::FunctionLocation<StreamSetup> stream_setup;
    fc::FunctionLocation<StartVoice> stream_start;
    fc::FunctionLocation<DevicePlay> device_play;
    fc::FunctionLocation<DeviceStop> device_stop;
    fc::CodeLocation device_play_result;
    fc::CodeLocation stream_close;
    fc::FunctionLocation<StreamConnect> stream_connect;
    fc::CodeLocation stream_requested_stop;
    fc::CodeLocation stream_requested_pause;
    fc::CodeLocation stream_requested_play;
    fc::CodeLocation stream_downstream_stop;
    fc::CodeLocation device_position_result;
    fc::CodeLocation flush_position_result;
    fc::CodeLocation stop_position_result;
    fc::CodeLocation flush_lock_result;
    fc::CodeLocation flush_unlock_result;
    fc::FunctionLocation<AssignVoice> voice_assignment;
    fc::CodeLocation stream_queued;
    fc::CodeLocation stream_due;
    fc::CodeLocation stream_due_end;
    fc::CodeLocation stream_deferred_start;
    fc::CodeLocation game_permission_resolved;
    fc::CodeLocation ordinary_admission;
    fc::CodeLocation stream_slot_gate;
    fc::CodeLocation stream_bus_gate;
    fc::CodeLocation stream_bus_gain_gate;
    fc::CodeLocation stream_sample_gate;
    fc::CodeLocation stream_pitch_gate;
    fc::CodeLocation stream_property_gate;
    fc::CodeLocation stream_gain_gate;
    fc::CodeLocation stream_queue_gate;
    fc::CodeLocation stream_priority_gate;
    fc::CodeLocation layered_queue_gate;
    fc::DataLocation<std::uint32_t> flush_lock_operand;
    fc::DataLocation<std::uint32_t> sleep_operand;
    fc::CodeLocation source_route_software;
    fc::CodeLocation source_route_hardware;
    fc::CodeLocation physical_route;
    fc::FunctionLocation<Update> ui_dispatch;
    fc::CodeLocation ui_event;
    fc::CodeLocation ui_event_end;
    std::array<fc::CodeLocation, 5> ui_callbacks;
    std::array<fc::CodeLocation, 5> ui_callback_ends;
    fc::FunctionLocation<GetPacket> packet;
    std::array<fc::CodeLocation, 2> voice_ends;
    std::array<fc::CodeLocation, 5> saber_decisions;
    fc::CodeLocation source_end;
    fc::FunctionLocation<GlobalUpdate> sound_reset;
    fc::CallLocation<GlobalUpdate> stop_all;
    std::array<fc::CodeLocation, 2> health_decisions;
    std::array<fc::CodeLocation, 2> ui_queued;
    fc::FunctionLocation<MeleeRead> melee_read;
    fc::FunctionLocation<MeleeControls> melee_controls;
    fc::FunctionLocation<BindingAssign> binding_assign;
    fc::FunctionLocation<EffectEmit> effect_emit;
    fc::FunctionLocation<GroundReturn> ground_return;
    fc::FunctionLocation<Toggle> toggle;
    fc::FunctionLocation<Toggle> fire_audio;
    fc::CodeLocation ground_commit;
    std::array<fc::CodeLocation, 2> ground_sounds;
    std::array<fc::CodeLocation, 2> toggle_sounds;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::sound_diagnostics
