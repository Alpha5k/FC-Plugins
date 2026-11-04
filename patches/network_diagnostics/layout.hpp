#pragma once

#include "native.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::network_diagnostics {

// Retail link-time optimization keeps destination and bytes in registers while the caller cleans up length.
using FinalSend = fc::NativeCall<std::int32_t(std::int32_t destination, const void *bytes, std::int32_t length) noexcept,
                                 fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;

// NetComm::Send likewise promotes its first two arguments while retaining the packet group on the caller's stack.
using GroupSend = fc::NativeCall<void(std::int32_t destination, std::int32_t packet_type, void *group) noexcept,
                                 fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>>>>;

// Shared receive polling and connection lifecycle entries use explicit optimized x86 call descriptions.
using ReceiveService = fc::NativeCall<void() noexcept, fc::abi::X86<fc::abi::Args<>>>;
using Disconnect = fc::NativeCall<void(std::int32_t destination) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using NetworkCreate = fc::NativeCall<void(bool preserve) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// Fastcall intake takes ownership of the packet and can queue or free it; its void return reports no acceptance result.
using PacketIntake = void(__fastcall *)(void *packet, void *endpoint) noexcept;

// Fastcall retrieval returns whether a complete group was available; the cdecl turn reader only peeks at that group.
using ReceiveGroup = bool(__fastcall *)(void *group, std::int32_t packet_type) noexcept;
using GetUpdateTurn = std::uint32_t(__cdecl *)(void *group) noexcept;

// NetPacket::Get performs a read-only pool lookup with the index in ECX.
using GetPacket = void *(__fastcall *)(std::int32_t index) noexcept;

using LagEstimator = std::int32_t(__cdecl *)(std::int32_t destination) noexcept;
enum class ClockField : std::size_t {
    PresentationClock,
    PresentationDelta,
    Interpolation,
    FrameDelta,
    NativeTime,
    DecisionDeadline,
    AdditiveCorrection,
    Count,
};

// Qualified client reset/consumer boundaries and read-only seconds-valued native globals.
struct ClientClockLayout {
    fc::CodeLocation reset_accumulator;
    fc::CodeLocation resume_accumulator;
    fc::CodeLocation presentation_complete;
    fc::DataLocation<const std::int32_t> acknowledged_turn;
    std::array<fc::DataLocation<const float>, static_cast<std::size_t>(ClockField::Count)> values;
};

// Native branch and serializer observations retain the estimator's actual result without repeating it.
struct HostLagLayout {
    fc::FunctionLocation<LagEstimator> estimator;
    fc::CodeLocation serialized_lag;
    fc::DataLocation<const std::int32_t> lagversion;
    fc::DataLocation<const std::uint8_t> drop_lag_jumps;
};

// Native send attempts, receive polling, group retrieval, and connection lifecycle shared by both launch roles.
struct NetworkLayout {
    fc::FunctionLocation<FinalSend> final_send;
    fc::FunctionLocation<GroupSend> group_send;
    fc::FunctionLocation<ReceiveService> receive_service;

    fc::FunctionLocation<PacketIntake> packet_intake;
    fc::FunctionLocation<Disconnect> disconnect;

    fc::FunctionLocation<NetworkCreate> create;
    fc::FunctionLocation<ReceiveGroup> receive_group;

    fc::FunctionLocation<GetUpdateTurn> get_update_turn;
    fc::FunctionLocation<GetPacket> get_packet;
};

// Qualified x86 object and stack offsets used to copy combat state into trace records.
struct CombatNativeLayout {
    std::size_t weapon_owner;
    std::size_t weapon_aimer;
    // ESP-relative matrix after GetFireMatrix, before the instructions at the observation site execute.
    std::size_t fire_matrix_stack;

    std::size_t projectile_lifetime;
    std::size_t projectile_position;
    std::size_t projectile_velocity;

    std::size_t ray_origin;
    std::size_t ray_direction;
    // Stack arguments: auxiliary/filter pointer, excluded-object array, and excluded-object count.
    std::size_t ray_filter;
    std::size_t ray_filter_secondary;
    std::size_t ray_filter_word;
    std::size_t ray_flags;
    std::size_t ray_flags_high;

    std::size_t damage_health;
    std::size_t damage_shield;
};

// Retail x86 fields for decoded soldier state and smoothing-history publication.
struct ClientNativeLayout {
    // Only type zero is sampled; player is the native player index recorded by SmoothInfo.
    std::size_t smooth_type;
    std::size_t smooth_player;

    std::size_t soldier_player;
    std::size_t soldier_matrix;
};

// Qualified x86 server fields expose controller input, reconstructed acknowledgements, and soldier poses.
struct ServerNativeLayout {
    std::size_t descriptor_direction;

    std::size_t controller_controllable;
    std::size_t controller_input;

    std::size_t controllable_player;

    std::size_t input_buttons;
    std::size_t input_joystick;

    // Native trigger switch state on the Controllable subobject.
    std::size_t primary_switch;
    std::size_t secondary_switch;

    // Positive distances below EBP at the ACK reconstruction site, before native acceptance checks.
    std::size_t acknowledgement_turn_local;
    std::size_t acknowledgement_player_local;

    // Subtract this offset from a soldier's Controllable pointer to read its entity fields.
    std::size_t controllable_subobject;
    std::size_t soldier_position;
    std::size_t soldier_velocity;
};

// Cannon and bullet observations associate nested collision and base damage calls with an active bullet update.
struct CombatLayout {
    CombatNativeLayout native;

    fc::FunctionLocation<native::WeaponFire> weapon_fire;
    fc::CodeLocation fire_matrix;

    fc::FunctionLocation<native::BulletBuild> bullet_build;
    fc::FunctionLocation<native::BulletUpdate> bullet_update;

    fc::CodeLocation ray_query;
    // Common I_RayHit epilogue: EAX and XMM0 hold the result before displaced instructions run.
    fc::CodeLocation ray_result;

    fc::FunctionLocation<native::ApplyDamage> apply_damage;
    fc::FunctionLocation<native::ApplyDamageAtPosition> apply_damage_at_position;
};

// Read-only native world/pass bytes and the common Soldier simulation boundary.
struct SimulationLayout {
    fc::FunctionLocation<native::SoldierUpdate> soldier_update;
    fc::FunctionLocation<native::GetJoystickIndex> get_joystick_index;
    fc::DataLocation<const std::int32_t> max_update_steps;
    fc::DataLocation<const std::int32_t> prediction_turn;
    std::array<fc::DataLocation<const std::uint8_t>, 4> world_pass;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<SimulationLayout> select_simulation_layout(std::string_view profile);

// Client locations cover turn intake, prediction, move codecs, and presentation sampling.
struct ClientLayout {
    ClientNativeLayout native;
    ClientClockLayout clock;

    fc::CodeLocation game_frame;
    fc::FunctionLocation<native::SubmitMove> submit_move;
    fc::FunctionLocation<native::VoidFunction> receive_client;
    fc::FunctionLocation<native::PacketFunction> read_update;
    // Before JA, EBP-1C holds the advertised turn and CF/ZF are the actual unsigned guard result.
    fc::CodeLocation update_turn_result;
    fc::FunctionLocation<native::Predict> predict;
    fc::FunctionLocation<native::VoidFunction> adjust_clock;

    fc::FunctionLocation<native::WriteMove> write_move;
    fc::FunctionLocation<native::WriteMiniMove> write_mini_move;
    fc::FunctionLocation<native::PacketFunction> read_net_event;
    fc::FunctionLocation<native::PacketFunction> receive_reliable_events;

    fc::FunctionLocation<native::SetRenderMatrix> set_render_matrix;

    CombatLayout combat;
    fc::FunctionLocation<native::SoldierRead> soldier_read;

    fc::DataLocation<const std::int32_t> host_turn;
    fc::DataLocation<const std::int32_t> client_turn;
    fc::DataLocation<const std::int32_t> update_turn;
    fc::DataLocation<const std::int32_t> predict_turn;
    fc::DataLocation<const std::int32_t> local_predict_turn;
    fc::DataLocation<const native::PlayerMove, native::kTurnCapacity * 2> move_ring;

    fc::DataLocation<const float> accumulator;

    // Native turn of the last clock adjustment, despite the field's name.
    fc::DataLocation<const std::int32_t> adjust_time;
    fc::DataLocation<const std::int32_t> stable_count;
    fc::DataLocation<const float> time_scale;
    fc::DataLocation<const std::int32_t> client_lag;
    fc::DataLocation<const std::uint32_t> reliable_frontier;
};

// Server locations cover authority turns, input reconstruction, replication stages, and combat simulation.
struct ServerLayout {
    ServerNativeLayout native;
    HostLagLayout lag;

    fc::CodeLocation game_frame;
    fc::FunctionLocation<native::SubmitMove> submit_move;
    fc::FunctionLocation<native::VoidFunction> receive_host;
    fc::FunctionLocation<native::VoidFunction> rollback_late_moves;
    fc::FunctionLocation<native::VoidFunction> save_late_state;
    fc::FunctionLocation<native::VoidFunction> finish_turn;

    fc::FunctionLocation<native::VoidFunction> send_host;
    fc::FunctionLocation<native::PacketFunction> write_update;

    fc::FunctionLocation<native::ReadMove> read_move;
    fc::FunctionLocation<native::ReadMiniMove> read_mini_move;
    fc::FunctionLocation<native::FillTurns> fill_turns;
    fc::FunctionLocation<native::VoidFunction> receive_primary;
    // Runs before modifiers and displaced instructions, after the native low-bit turn reconstruction.
    fc::CodeLocation ack_reconstruction;

    fc::FunctionLocation<native::PacketFunction> write_objects;
    fc::FunctionLocation<native::PacketFunction> send_events;

    fc::FunctionLocation<native::CreateOrdnanceEvent> create_ordnance_event;
    fc::FunctionLocation<native::PlayerControllerUpdate> controller_update;

    CombatLayout combat;

    fc::VtableLocation soldier_controllable_vtable;

    fc::DataLocation<const std::int32_t> host_turn;
    fc::DataLocation<const std::int32_t> client_turn;

    fc::DataLocation<const float> accumulator;
    // Sampled after SendHost, before GameLoop restores the outer frame delta.
    fc::DataLocation<const float> simulation_delta;
    fc::DataLocation<const float> fixed_delta;
    fc::DataLocation<const float> time_scale;

    fc::DataLocation<const std::uint64_t> required_mask;
    fc::DataLocation<const std::uint64_t, native::kTurnCapacity> received_masks;

    fc::DataLocation<const std::int32_t> current_destination;
    fc::DataLocation<const native::PlayerMove, native::kTurnCapacity * native::kPlayerCapacity> move_ring;

    fc::DataLocation<const std::uint32_t> event_head;
    std::array<fc::DataLocation<const std::uint32_t>, native::kPlayerCapacity> event_cursors;

    std::array<fc::DataLocation<const std::uint32_t>, native::kPlayerCapacity> filled_turn_counts;

    fc::DataLocation<const std::uint8_t> network_enabled;
    fc::DataLocation<const std::uint8_t> waitlate;
    fc::DataLocation<const std::uint32_t> lead_turns;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<NetworkLayout> select_network_layout(std::string_view profile);

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ClientLayout> select_client_layout(const std::string_view profile);

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ServerLayout> select_server_layout(const std::string_view profile);

} // namespace fc_plugins::network_diagnostics
