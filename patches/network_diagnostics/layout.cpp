#include "layout.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace fc_plugins::network_diagnostics {
namespace {

// Image-relative transport entry RVAs use the optimized x86 call descriptions in layout.hpp.
struct NetworkSites {
    std::string_view profile;
    std::uint32_t final_send;
    std::uint32_t group_send;
    std::uint32_t receive_service;

    std::uint32_t packet_intake;
    std::uint32_t disconnect;

    std::uint32_t create;
    std::uint32_t receive_group;

    std::uint32_t get_update_turn;
    std::uint32_t get_packet;
};

constexpr std::array kNetworkProfiles{
    NetworkSites{
        .profile = "BF2.Steam.59EDE353",
        .final_send = {0x00217E50},
        .group_send = {0x001B2900},
        .receive_service = {0x001B31C0},
        .packet_intake = {0x001B3290},
        .disconnect = {0x001B2AF0},
        .create = {0x001B2430},
        .receive_group = {0x001B2A60},
        .get_update_turn = {0x001BB570},
        .get_packet = 0x001D8F00,
    },
    NetworkSites{
        .profile = "BF2.GOG.59EDF52B",
        .final_send = {0x00218EC0},
        .group_send = {0x001B38A0},
        .receive_service = {0x001B4170},
        .packet_intake = {0x001B4240},
        .disconnect = {0x001B3A90},
        .create = {0x001B33D0},
        .receive_group = {0x001B3A00},
        .get_update_turn = {0x001BC520},
        .get_packet = 0x001D9FB0,
    },
};

// Builds the networking locations for one retail profile's optimized x86 ABIs.
NetworkLayout make_network_layout(const NetworkSites &sites) {
    return {
        .final_send = {.rva = sites.final_send, .name = "NetSocket::SendSocket"},
        .group_send = {.rva = sites.group_send, .name = "NetComm::Send(int, PktType, NetPktGroup*)"},
        .receive_service = {.rva = sites.receive_service, .name = "DoRecvs"},
        .packet_intake = {.rva = sites.packet_intake, .name = "NetCommP::RecvPacket"},
        .disconnect = {.rva = sites.disconnect, .name = "NetComm::Disconnect"},
        .create = {.rva = sites.create, .name = "NetComm::Create"},
        .receive_group = {.rva = sites.receive_group, .name = "NetComm::Recv"},
        .get_update_turn = {.rva = sites.get_update_turn, .name = "GetUpdateTurn"},
        .get_packet = {.rva = sites.get_packet,
                       .name = "NetPacket::Get",
                       .evidence = fc::exact_bytes({0x85, 0xC9, 0x78, 0x2B, 0x81, 0xF9, 0x00, 0x01, 0x00, 0x00, 0x7D, 0x0C})},
    };
}

// Image-relative weapon and projectile entries feed the shared combat location construction.
struct CombatSites {
    std::uint32_t weapon_fire;
    std::uint32_t bullet_build;
    std::uint32_t bullet_update;
};

// Both frame loops consume the reset before accumulating time and publish presentation delta after clamping.
struct ClockSites {
    std::uint32_t reset_accumulator;
    std::uint32_t resume_accumulator;
    std::uint32_t presentation_complete;
    std::uint32_t acknowledged_turn;
    std::array<std::uint32_t, static_cast<std::size_t>(ClockField::Count)> values;
    std::array<std::uint8_t, 5> resume_bytes;
};

// Keeps each client's network boundaries and globals together by image profile.
struct ClientSites {
    std::string_view profile;
    ClockSites clock;
    std::uint32_t game_frame;

    std::uint32_t submit_move;
    std::uint32_t receive_client;
    std::uint32_t read_update;
    std::uint32_t predict;
    std::uint32_t adjust_clock;
    std::uint32_t write_move;
    std::uint32_t write_mini_move;
    std::uint32_t read_net_event;
    std::uint32_t receive_reliable_events;
    std::uint32_t set_render_matrix;
    std::uint32_t soldier_read;
    CombatSites combat;

    std::uint32_t host_turn;
    std::uint32_t client_turn;
    std::uint32_t update_turn;
    std::uint32_t predict_turn;
    std::uint32_t local_predict_turn;
    std::uint32_t move_ring;
    std::uint32_t accumulator;
    std::uint32_t adjust_time;
    std::uint32_t stable_count;
    std::uint32_t time_scale;
    std::uint32_t client_lag;
    std::uint32_t reliable_frontier;
    std::uint32_t get_joystick_index;
    std::uint32_t max_update_steps;
    std::uint32_t network_enabled;
    std::uint32_t local_prediction_pass;
};

constexpr ClientSites kSteamSites{
    .profile = "BF2.Steam.59EDE353",
    .clock = {.reset_accumulator = 0x0013275C,
              .resume_accumulator = 0x00132766,
              .presentation_complete = 0x00132AE2,
              .acknowledged_turn = 0x01BA6594,
              .values = {0x01A56048, 0x01A56058, 0x01BA87B8, 0x01A56054, 0x01BA3D60, 0x01BA3D7C, 0x01BA8764},
              .resume_bytes = {0xE8, 0x65, 0x80, 0x08, 0x00}},
    .game_frame = {0x001323E0},
    .submit_move = {0x001B9620},
    .receive_client = {0x001B8F70},
    .read_update = {0x001BB5B0},
    .predict = {0x001BA810},
    .adjust_clock = {0x001C8590},
    .write_move = {0x001CAD00},
    .write_mini_move = {0x001CAF90},

    .read_net_event = {0x001BEF70},
    .receive_reliable_events = {0x001BF9B0},
    .set_render_matrix = {0x001DA0A0},
    .soldier_read = {0x001EA140},
    .combat = {.weapon_fire = 0x0027F320, .bullet_build = 0x001FC230, .bullet_update = 0x001FAEE0},
    .host_turn = {0x01A64DE4},
    .client_turn = {0x01A64BA4},
    .update_turn = {0x01BA65B0},
    .predict_turn = {0x01BA87B4},
    .local_predict_turn = {0x01BA8790},
    .move_ring = {0x01AE4B40},
    .accumulator = {0x01A56038},
    .adjust_time = {0x01BAA6AC},
    .stable_count = {0x01BA8780},
    .time_scale = {0x01BA877C},
    .client_lag = {0x01A64B34},
    .reliable_frontier = {0x01BA65FC},
    .get_joystick_index = 0x001B73C0,
    .max_update_steps = 0x003EC504,
    .network_enabled = 0x01A62EA9,
    .local_prediction_pass = 0x01A62F10,
};

constexpr ClientSites kGogSites{
    .profile = "BF2.GOG.59EDF52B",
    .clock = {.reset_accumulator = 0x001334BC,
              .resume_accumulator = 0x001334C6,
              .presentation_complete = 0x00133842,
              .acknowledged_turn = 0x01BA7A48,
              .values = {0x01A574E0, 0x01A574F0, 0x01BA9C6C, 0x01A574EC, 0x01BA5214, 0x01BA5230, 0x01BA9C18},
              .resume_bytes = {0xE8, 0xB5, 0x82, 0x08, 0x00}},
    .game_frame = {0x00133140},
    .submit_move = {0x001BA5D0},
    .receive_client = {0x001B9F20},
    .read_update = {0x001BC560},
    .predict = {0x001BB7C0},
    .adjust_clock = {0x001C9510},
    .write_move = {0x001CBC80},
    .write_mini_move = {0x001CBF10},

    .read_net_event = {0x001BFF00},
    .receive_reliable_events = {0x001C0940},
    .set_render_matrix = {0x001DB150},
    .soldier_read = {0x001EB1E0},
    .combat = {.weapon_fire = 0x002803A0, .bullet_build = 0x001FD2D0, .bullet_update = 0x001FBF80},
    .host_turn = {0x01A66294},
    .client_turn = {0x01A66054},
    .update_turn = {0x01BA7A64},
    .predict_turn = {0x01BA9C68},
    .local_predict_turn = {0x01BA9C44},
    .move_ring = {0x01AE5FF0},
    .accumulator = {0x01A574D0},
    .adjust_time = {0x01BABB5C},
    .stable_count = {0x01BA9C34},
    .time_scale = {0x01BA9C30},
    .client_lag = {0x01A65FE4},
    .reliable_frontier = {0x01BA7AB0},
    .get_joystick_index = 0x001B8370,
    .max_update_steps = 0x003ED494,
    .network_enabled = 0x01A64359,
    .local_prediction_pass = 0x01A643C0,
};

constexpr std::array kProfiles{kSteamSites, kGogSites};
constexpr std::uint32_t kEventCursorBase{0x01ACEF88};
constexpr std::uint32_t kFilledTurnCountBase{0x01ACF088};

// Retail x86 object and stack offsets for cannon, bullet, collision, and damage observations.
constexpr CombatNativeLayout kCombatNative{
    .weapon_owner = 0x6C,
    .weapon_aimer = 0x70,
    .fire_matrix_stack = 0x90,
    .projectile_lifetime = 0x3C,
    .projectile_position = 0x48,
    .projectile_velocity = 0xFC,
    .ray_origin = 0x04,
    .ray_direction = 0x08,
    .ray_filter = 0x10,
    .ray_filter_secondary = 0x14,
    .ray_filter_word = 0x18,
    .ray_flags = 0x1C,
    // I_RayHit's collision-world argument occupies 0x20 before the secondary flags.
    .ray_flags_high = 0x24,
    .damage_health = 0x04,
    .damage_shield = 0x10,
};

constexpr ClientNativeLayout kClientNative{
    .smooth_type = 0x198,
    .smooth_player = 0x19C,

    .soldier_player = 0x314,
    .soldier_matrix = 0xF0,
};

constexpr ServerNativeLayout kServerNative{
    .descriptor_direction = 0x0C,

    .controller_controllable = 0x04,
    .controller_input = 0x88,
    .controllable_player = 0xD4,
    .input_buttons = 0x10,
    .input_joystick = 0x14,
    .primary_switch = 0x74,
    .secondary_switch = 0x78,
    .acknowledgement_turn_local = 0x30,
    .acknowledgement_player_local = 0x10,
    .controllable_subobject = 0x240,

    .soldier_position = 0x120,
    .soldier_velocity = 0x4DC,
};

// Combines each image's weapon and projectile sites with shared collision and damage locations.
CombatLayout make_combat_layout(const CombatSites &sites) {
    return {
        .native = kCombatNative,
        .weapon_fire = {.rva = sites.weapon_fire, .name = "WeaponCannon::Fire"},
        .fire_matrix =
            {
                .rva = (sites.weapon_fire + 0x54),
                .name = "cannon aim matrix before spread",
                .evidence = fc::exact_bytes({0x8B, 0x56, 0x64}),
            },
        .bullet_build = {.rva = sites.bullet_build, .name = "OrdnanceBulletClass::Build"},
        .bullet_update = {.rva = sites.bullet_update, .name = "OrdnanceBullet::Update"},
        .ray_query =
            {
                // Pair I_RayHit's entry with its common epilogue to keep nested query depth balanced.
                .rva = {0x0005F130U},
                .name = "projectile collision query",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x70, 0x8B, 0x55, 0x0C, 0x0F, 0x28, 0xCB}),
            },
        .ray_result =
            {
                .rva = {0x0005F2F9U},
                .name = "projectile collision result",
                .evidence = fc::exact_bytes({0x5F, 0x5E, 0x89, 0x01, 0x5B}),
            },
        .apply_damage = {.rva = {0x000895E0U}, .name = "direct damage application"},
        .apply_damage_at_position = {.rva = {0x00089340U}, .name = "damage application with position"},
    };
}

// Builds the bounded client clock observations and their native time inputs.
ClientClockLayout make_clock_layout(const ClockSites &sites) {
    ClientClockLayout layout{
        .reset_accumulator = {.rva = sites.reset_accumulator,
                              .name = "input accumulator reset",
                              .evidence =
                                  fc::masked_bytes({0xC7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0}, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF})},
        .resume_accumulator = {.rva = sites.resume_accumulator,
                               .name = "input accumulator after reset",
                               .evidence = fc::exact_bytes(sites.resume_bytes)},
        .presentation_complete = {.rva = sites.presentation_complete,
                                  .name = "presentation clock and clamped delta published",
                                  .evidence = fc::masked_bytes({0xF3, 0x0F, 0x11, 0x0D, 0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0})},
        .acknowledged_turn = {.rva = sites.acknowledged_turn, .name = "host echoed acknowledgement turn"},
    };
    constexpr std::array names{"previous presentation clock", "clamped presentation delta", "input interpolation fraction",
                               "outer frame delta",           "native network time",        "clock decision deadline",
                               "additive input correction"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        layout.values[index] = {.rva = sites.values[index], .name = names[index]};
    }
    return layout;
}

// Builds the client's observation and data locations from its qualified profile row.
ClientLayout make_client_layout(const ClientSites &sites) {
    return {
        .native = kClientNative,
        .clock = make_clock_layout(sites.clock),
        .game_frame = {.rva = sites.game_frame, .name = "GameLoop::Update"},
        .submit_move = {.rva = sites.submit_move, .name = "NetGame::SubmitMove"},
        .receive_client = {.rva = sites.receive_client, .name = "NetGame::ReceiveClient"},
        .read_update = {.rva = sites.read_update, .name = "ReadUpdate"},
        .update_turn_result = {.rva = sites.read_update + 0xC0,
                               .name = "ReadUpdate unsigned turn decision",
                               .evidence = fc::exact_bytes({0x77, 0x05, 0xE9, 0xD0, 0x06, 0x00, 0x00})},
        .predict = {.rva = sites.predict, .name = "NetGame::Predict"},
        .adjust_clock = {.rva = sites.adjust_clock, .name = "AdjustClock"},
        .write_move = {.rva = sites.write_move, .name = "WriteMove"},
        .write_mini_move = {.rva = sites.write_mini_move, .name = "WriteMiniMove"},

        .read_net_event = {.rva = sites.read_net_event, .name = "ReadNetEvent"},
        .receive_reliable_events = {.rva = sites.receive_reliable_events, .name = "ReceiveReliableNetEvents"},
        .set_render_matrix = {.rva = sites.set_render_matrix, .name = "SmoothInfo::SetRenderMatrix"},
        .combat = make_combat_layout(sites.combat),
        .soldier_read = {.rva = sites.soldier_read, .name = "EntitySoldier::ReadObject"},
        .host_turn = {.rva = sites.host_turn, .name = "client host turn"},
        .client_turn = {.rva = sites.client_turn, .name = "client move turn"},
        .update_turn = {.rva = sites.update_turn, .name = "update turn"},
        .predict_turn = {.rva = sites.predict_turn, .name = "prediction turn"},
        .local_predict_turn = {.rva = sites.local_predict_turn, .name = "local prediction turn"},
        .move_ring = {.rva = sites.move_ring, .name = "client move history"},
        .accumulator = {.rva = sites.accumulator, .name = "simulation accumulator"},
        .adjust_time = {.rva = sites.adjust_time, .name = "last clock adjustment turn"},
        .stable_count = {.rva = sites.stable_count, .name = "stable clock update count"},
        .time_scale = {.rva = sites.time_scale, .name = "network time scale"},
        .client_lag = {.rva = sites.client_lag, .name = "client turn lag"},
        .reliable_frontier = {.rva = sites.reliable_frontier, .name = "latest reliable event sequence"},
    };
}

// Builds the GOG dedicated server's authority, input, replication, and combat locations.
ServerLayout make_server_layout() {
    ServerLayout layout{
        .native = kServerNative,
        .lag = {.estimator = {.rva = {0x001C91E0U}, .name = "native destination lag estimator"},
                .serialized_lag = {.rva = {0x001BC239U},
                                   .name = "biased eight-bit lag before serialization",
                                   .evidence = fc::exact_bytes({0x50, 0x8B, 0x4D, 0x08})},
                .lagversion = {.rva = {0x01A643ECU}, .name = "host lagversion selector"},
                .drop_lag_jumps = {.rva = {0x01A643E8U}, .name = "host drop lag jumps"}},
        .game_frame = {.rva = {0x00133140U}, .name = "GameLoop::Update"},
        .submit_move = {.rva = {0x001BA5D0U}, .name = "NetGame::SubmitMove"},
        .receive_host = {.rva = {0x001BA2F0U}, .name = "NetGame::ReceiveHost"},
        .rollback_late_moves = {.rva = {0x001BB0C0U}, .name = "NetGame::ApplyLateMoves"},
        .save_late_state = {.rva = {0x001BAF50U}, .name = "NetGame::SaveLateState"},
        .finish_turn = {.rva = {0x001BB5F0U}, .name = "NetGame::FinishTurn"},
        .send_host = {.rva = {0x001BA200U}, .name = "NetGame::SendHost"},
        .write_update = {.rva = {0x001BC0B0U}, .name = "WriteUpdate"},
        .read_move = {.rva = {0x001CBDB0U}, .name = "ReadMove"},
        .read_mini_move = {.rva = {0x001CBFC0U}, .name = "ReadMiniMove"},
        .fill_turns = {.rva = {0x001C9AA0U}, .name = "FillTurns"},
        .receive_primary = {.rva = {0x001CAED0U}, .name = "ReceivePrimary"},
        .ack_reconstruction = {.rva = {0x001CB3FBU},
                               .name = "reconstructed acknowledgement before acceptance",
                               .evidence = fc::exact_bytes({0x83, 0xC4, 0x08, 0x83, 0x7D, 0xD0, 0xFF})},
        .write_objects = {.rva = {0x001CD6A0U}, .name = "WriteObjects"},

        .send_events = {.rva = {0x001BFD70U}, .name = "SendNetEvents"},
        .create_ordnance_event = {.rva = {0x001BF4E0U}, .name = "NetGame::CreateEvent_CreateOrdnance"},
        .controller_update = {.rva = {0x0021B320U}, .name = "PlayerController::Update"},
        .combat = make_combat_layout(kGogSites.combat),
        .soldier_controllable_vtable = {.rva = {0x0039DECCU}, .name = "soldier controllable vtable"},
        .host_turn = {.rva = {0x01BA9C4CU}, .name = "server host turn"},
        .client_turn = {.rva = {0x01A66054U}, .name = "server client turn"},
        .accumulator = {.rva = {0x01A574D0U}, .name = "simulation accumulator"},
        .simulation_delta = {.rva = {0x01A574ECU}, .name = "simulation delta"},
        .fixed_delta = {.rva = {0x01A64368U}, .name = "fixed network delta"},
        .time_scale = {.rva = {0x01BA9C30U}, .name = "network time scale"},
        .required_mask = {.rva = {0x01BA9D60U}, .name = "required player mask"},
        .received_masks = {.rva = {0x01A66298U}, .name = "received input masks"},
        .current_destination = {.rva = {0x01BA9C2CU}, .name = "current destination"},
        .move_ring = {.rva = {0x01AD9EB0U}, .name = "server move history"},
        .event_head = {.rva = {0x01BA9C40U}, .name = "network event head"},
        .network_enabled = {.rva = {0x01A64359U}, .name = "network enabled"},
        .waitlate = {.rva = {0x01A64400U}, .name = "late input waiting enabled"},
        .lead_turns = {.rva = {0x01BABB64U}, .name = "client lead turns"},
    };

    // Player state blocks are 0x208 bytes apart; reserve only the cursor and fill counter used by the recorder.
    for (std::size_t player = 0; player < native::kPlayerCapacity; ++player) {
        const auto player_byte_offset = static_cast<std::int32_t>(player * native::kPlayerStateStride);
        layout.event_cursors[player] = {.rva = (kEventCursorBase + player_byte_offset), .name = "player event cursor"};
        layout.filled_turn_counts[player] = {.rva = (kFilledTurnCountBase + player_byte_offset), .name = "filled move count"};
    }

    return layout;
}

} // namespace

std::optional<NetworkLayout> select_network_layout(const std::string_view profile) {
    for (const auto &row : kNetworkProfiles) {
        if (row.profile == profile) {
            return make_network_layout(row);
        }
    }

    return std::nullopt;
}

std::optional<ClientLayout> select_client_layout(const std::string_view profile) {
    for (const auto &row : kProfiles) {
        if (row.profile == profile) {
            return make_client_layout(row);
        }
    }

    return std::nullopt;
}

std::optional<ServerLayout> select_server_layout(const std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return make_server_layout();
}

std::optional<SimulationLayout> select_simulation_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile != profile) {
            continue;
        }
        // Retail IsSkipped and Fire consume these exact bytes; Update's ECX is Soldier+240.
        return SimulationLayout{
            .soldier_update = {.rva = 0x000E8920U,
                               .name = "Soldier native simulation",
                               .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
            .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},
            .max_update_steps = {.rva = sites.max_update_steps, .name = "maximum remote update steps"},
            .prediction_turn = {.rva = sites.predict_turn, .name = "native prediction turn"},
            .world_pass = {{{.rva = sites.network_enabled, .name = "network enabled"},
                            {.rva = sites.network_enabled + 2, .name = "client object world active"},
                            {.rva = sites.local_prediction_pass, .name = "local prediction pass"},
                            {.rva = sites.local_prediction_pass + 1, .name = "late prediction pass"}}},
        };
    }
    return std::nullopt;
}

} // namespace fc_plugins::network_diagnostics
