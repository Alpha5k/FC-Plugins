#include "layout.hpp"

#include <array>
#include <string_view>
#include <string>

namespace fc_plugins::weapon_swap_replay_fix {
namespace {

// RVAs and expected bytes for weapon selection, presentation and input history in each supported image.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t read_object;
    std::uint32_t weapon_select;
    std::uint32_t hud_weapon;
    std::uint32_t player_controller_update;

    std::uint32_t update_turn_context;
    std::uint32_t prediction_turn_context;
    std::uint32_t acknowledged_turn_context;
    std::uint32_t client_turn_context;

    std::uint32_t acknowledged_turn;
    std::uint32_t prediction_turn;

    std::uint32_t network_enabled;

    std::uint32_t client_turn;
    std::uint32_t local_move_history;

    std::uint32_t get_joystick_index;
    std::uint32_t get_local_player;
    std::uint32_t reset_soldier;
    std::uint32_t submitted_move;

    // Expected instruction bytes, including absolute operands ignored by the masks.
    std::array<std::uint8_t, 10> update_turn_context_bytes;
    std::array<std::uint8_t, 8> prediction_turn_context_bytes;
    std::array<std::uint8_t, 12> acknowledged_turn_context_bytes;
    std::array<std::uint8_t, 41> network_state_context_bytes;
    std::array<std::uint8_t, 8> client_turn_context_bytes;
    std::array<std::uint8_t, 7> move_history_context_bytes;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .read_object = {0x001EA140},
        .weapon_select = {0x00278080},
        .hud_weapon = {0x00160AF3},
        .player_controller_update = {0x0021A2B0},
        .update_turn_context = {0x001BB6C0},
        .prediction_turn_context = {0x001BE3A0},
        .acknowledged_turn_context = {0x001BBA39},
        .client_turn_context = {0x001B8700},
        .acknowledged_turn = {0x01BA6594},
        .prediction_turn = {0x01BA87B4},
        .network_enabled = {0x01A62EA9},
        .client_turn = {0x01A64BA4},
        .local_move_history = {0x01AE4B40},
        .get_joystick_index = {0x001B73C0},
        .get_local_player = {0x001B26E0},
        .reset_soldier = {0x001EB1C0},
        .submitted_move = {0x001B9997},
        .update_turn_context_bytes = {0xA1, 0xE4, 0x4D, 0xE6, 0x01, 0xA3, 0xB0, 0x65, 0xFA, 0x01},
        .prediction_turn_context_bytes = {0xA1, 0xB4, 0x87, 0xFA, 0x01, 0x83, 0xE0, 0x1F},
        .acknowledged_turn_context_bytes = {0xA1, 0xE4, 0x4D, 0xE6, 0x01, 0x2B, 0xC2, 0xA3, 0x94, 0x65, 0xFA, 0x01},
        .network_state_context_bytes = {0x8A, 0x15, 0xA9, 0x2E, 0xE6, 0x01, 0x80, 0x3D, 0x07, 0x80, 0x7E, 0x00, 0x00, 0x0F,
                                        0xB6, 0x05, 0xA8, 0x2E, 0xE6, 0x01, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9,
                                        0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B, 0x80, 0x3D, 0xAB, 0x2E, 0xE6, 0x01, 0x00},
        .client_turn_context_bytes = {0x8B, 0x45, 0x08, 0xA3, 0xA4, 0x4B, 0xE6, 0x01},
        .move_history_context_bytes = {0x8D, 0x84, 0x0A, 0x40, 0x4B, 0xEE, 0x01},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .read_object = {0x001EB1E0},
        .weapon_select = {0x00279120},
        .hud_weapon = {0x00161873},
        .player_controller_update = {0x0021B320},
        .update_turn_context = {0x001BC670},
        .prediction_turn_context = {0x001BF330},
        .acknowledged_turn_context = {0x001BC9E9},
        .client_turn_context = {0x001B96B0},
        .acknowledged_turn = {0x01BA7A48},
        .prediction_turn = {0x01BA9C68},
        .network_enabled = {0x01A64359},
        .client_turn = {0x01A66054},
        .local_move_history = {0x01AE5FF0},
        .get_joystick_index = {0x001B8370},
        .get_local_player = {0x001B3680},
        .reset_soldier = {0x001EC260},
        .submitted_move = {0x001BA947},
        .update_turn_context_bytes = {0xA1, 0x94, 0x62, 0xE6, 0x01, 0xA3, 0x64, 0x7A, 0xFA, 0x01},
        .prediction_turn_context_bytes = {0xA1, 0x68, 0x9C, 0xFA, 0x01, 0x83, 0xE0, 0x1F},
        .acknowledged_turn_context_bytes = {0xA1, 0x94, 0x62, 0xE6, 0x01, 0x2B, 0xC2, 0xA3, 0x48, 0x7A, 0xFA, 0x01},
        .network_state_context_bytes = {0x8A, 0x15, 0x59, 0x43, 0xE6, 0x01, 0x80, 0x3D, 0x07, 0x90, 0x7E, 0x00, 0x00, 0x0F,
                                        0xB6, 0x05, 0x58, 0x43, 0xE6, 0x01, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9,
                                        0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B, 0x80, 0x3D, 0x5B, 0x43, 0xE6, 0x01, 0x00},
        .client_turn_context_bytes = {0x8B, 0x45, 0x08, 0xA3, 0x54, 0x60, 0xE6, 0x01},
        .move_history_context_bytes = {0x8D, 0x84, 0x0A, 0xF0, 0x5F, 0xEE, 0x01},
    },
};

constexpr std::uint32_t kSoldierUpdate{0x000E8920};
constexpr std::uint32_t kSoldierRender{0x000E23D0};
constexpr std::uint32_t kModelSelection{0x000E4CB4};
constexpr std::uint32_t kSwitchLatch{0x000839F1};
constexpr std::uint32_t kNetworkStateContext{0x00083E75};

// Ignore absolute addresses that the loader rebases while matching the surrounding instructions exactly.
constexpr std::array<std::uint8_t, 10> kTurnStoreMask = {0xFF, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kAcknowledgedTurnMask = {
    0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
};
constexpr std::array<std::uint8_t, 41> kNetworkStateMask = {
    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,    0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0, 0, 0, 0xFF,
};
constexpr std::array<std::uint8_t, 8> kClientTurnMask = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 7> kMoveHistoryMask = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0};

constexpr std::array<std::uint8_t, 8> kPredictionTurnContextMask{0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF};

// Describes selection hooks and input globals with their instruction evidence for the selected build.
Layout make_layout(const ProfileSites &sites) {
    return {
        // Complete soldier resets call ClearControllable; destruction releases its inventory before allocation reuse.
        .reset_soldier = {.rva = sites.reset_soldier,
                          .name = "EntitySoldier::ResetObject",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .destroy_soldier = {.rva = 0x000DFA20,
                            .name = "EntitySoldier::~EntitySoldier",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        // Each slot reaches this jump after copying/clearing input, or skipping a missing source; EBP retains loop locals.
        .submitted_move = {.rva = sites.submitted_move,
                           .name = "submitted local input slot",
                           .evidence = fc::exact_bytes({0xE9, 0xEF, 0xFE, 0xFF, 0xFF})},
        // Match the complete entry before its relocatable SEH operand; both native returns clean three stack arguments.
        .render = {.rva = kSoldierRender,
                   .name = "EntitySoldier::Render",
                   .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF})},
        .local_selection =
            {
                .rva = kSoldierUpdate + 0x3F19,
                .name = "local weapon selection",
                .evidence = fc::exact_bytes({0x6A, 0x00, 0xFF, 0x74, 0x24, 0x24, 0x88, 0x01, 0x0F, 0xBE, 0xC0}),
            },
        .packed_sync =
            {
                // Steam/GOG 0x004EA250: ECX is the channel, before either packed selection store.
                .rva = kSoldierUpdate + 0x1930,
                .name = "packed weapon selection before update",
                .evidence = fc::exact_bytes({0x8A, 0xD1, 0xC0, 0xE2, 0x04}),
            },
        .authority_selection =
            {
                .rva = sites.read_object + 0xA97,
                .name = "received weapon selection",
                .evidence = fc::exact_bytes({0x8A, 0x02, 0x3A, 0x44, 0x24, 0x1C}),
            },
        .packed_authority =
            {
                .rva = sites.read_object + 0x28E,
                .name = "received packed weapon selection",
                .evidence = fc::exact_bytes({0x85, 0xC9, 0x78, 0x0F, 0x8B, 0x8C, 0x8F, 0x20, 0x07, 0x00, 0x00, 0x6A, 0x00, 0x52}),
            },

        .weapon_select =
            {
                .rva = sites.weapon_select + 0x3A,
                .name = "weapon selection muzzle-flash timestamp and change sound",
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x87, 0xC4, 0x00, 0x00, 0x00, 0x75, 0x2A}),
            },
        .hud_weapon =
            {
                .rva = sites.hud_weapon,
                .name = "HUD selected weapon read result",
                .evidence = fc::exact_bytes({0x8B, 0xF0, 0x89, 0x75, 0xF0}),
            },
        .render_selection =
            {
                .rva = kSoldierRender + 0xC4D,
                .name = "render selection lookup",
                .evidence = fc::exact_bytes({0x33, 0xF6, 0x8A, 0xC1, 0x89, 0x74, 0x24, 0x38, 0xC0, 0xE0, 0x04, 0x8A, 0xD0}),
            },
        .render_channel =
            {
                .rva = kSoldierRender + 0xCA5,
                .name = "render active weapon channel test",
                .evidence = fc::exact_bytes({0x24, 0x30, 0x3C, 0x10, 0x75, 0x62}),
            },
        .model_selection =
            {
                .rva = kModelSelection,
                .name = "weapon model selection lookup",
                .evidence =
                    fc::exact_bytes({0x8A, 0xC2, 0xC0, 0xE0, 0x04, 0xA8, 0xF0, 0x0F, 0x8C, 0x27, 0x01, 0x00, 0x00, 0x0F, 0xBE, 0xC0}),
            },
        // AL has just loaded primary slot +0x740; preserve its signed none sentinel before TEST/MOVSX.
        .render_companion = {{{.rva = 0x000E3081,
                               .name = "render primary companion selection",
                               .evidence = fc::exact_bytes({0x84, 0xC0, 0x78, 0x58, 0x0F, 0xBE, 0xC0})},
                              {.rva = 0x000E30AE,
                               .name = "render offhand companion selection",
                               .evidence = fc::exact_bytes({0x84, 0xC0, 0x78, 0x16, 0x0F, 0xBE, 0xC0})}}},
        // EAX's full channel index is destroyed by MOV AL; derive the opposite channel from the captured active byte.
        .model_companion = {.rva = 0x000E4D08,
                            .name = "weapon model companion selection",
                            .evidence = fc::exact_bytes({0x84, 0xC0, 0x78, 0x7F, 0x0F, 0xBE, 0xC0})},

        .switch_latch =
            {
                .rva = kSwitchLatch,
                .name = "weapon switch input latch",
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x8C, 0x91, 0x90, 0x00, 0x00, 0x00, 0x5D, 0xC2, 0x04, 0x00}),
            },
        .primary_switch_call =
            {
                .rva = sites.player_controller_update + 0x8B5,
                .name = "primary weapon switch call",
                .evidence = fc::direct_call_to(fc::Location{.image = std::string{sites.profile}, .rva = 0x000839E0}),
            },
        .secondary_switch_call =
            {
                .rva = sites.player_controller_update + 0x8CD,
                .name = "secondary weapon switch call",
                .evidence = fc::direct_call_to(fc::Location{.image = std::string{sites.profile}, .rva = 0x000839E0}),
            },

        .update_turn_context =
            {
                .rva = sites.update_turn_context,
                .name = "network update turn store",
                .evidence = fc::masked_bytes(sites.update_turn_context_bytes, kTurnStoreMask),
            },
        .prediction_turn_context =
            {
                .rva = sites.prediction_turn_context + 6,
                .name = "prediction turn read",
                .evidence = fc::masked_bytes(sites.prediction_turn_context_bytes, kPredictionTurnContextMask),
            },
        .acknowledged_turn_context =
            {
                .rva = sites.acknowledged_turn_context,
                .name = "acknowledged turn store",
                .evidence = fc::masked_bytes(sites.acknowledged_turn_context_bytes, kAcknowledgedTurnMask),
            },
        .network_state_context =
            {
                .rva = kNetworkStateContext,
                .name = "network client state check",
                .evidence = fc::masked_bytes(sites.network_state_context_bytes, kNetworkStateMask),
            },
        .client_turn_context =
            {
                .rva = sites.client_turn_context + 9,
                .name = "client input turn initialization",
                .evidence = fc::masked_bytes(sites.client_turn_context_bytes, kClientTurnMask),
            },
        .move_history_context =
            {
                .rva = sites.prediction_turn_context + 0x71,
                .name = "local move history read",
                .evidence = fc::masked_bytes(sites.move_history_context_bytes, kMoveHistoryMask),
            },

        .update_turn = {.rva = sites.acknowledged_turn + 0x1C, .name = "network update turn"},
        .prediction_turn = {.rva = sites.prediction_turn, .name = "prediction turn"},
        .acknowledged_turn = {.rva = sites.acknowledged_turn, .name = "acknowledged turn"},

        .network_enabled = {.rva = sites.network_enabled, .name = "network enabled"},
        .network_client_active = {.rva = sites.network_enabled + 2, .name = "native client world active"},

        .client_turn = {.rva = sites.client_turn, .name = "next queued input turn"},
        .local_move_history = {.rva = sites.local_move_history, .name = "queued local player input history"},

        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "local input slot lookup"},
        // ECX local slot selects the four-byte player table entry; RET has no stack cleanup.
        .get_local_player = {.rva = sites.get_local_player,
                             .name = "local input player identity",
                             .evidence = fc::masked_bytes({0x8B, 0x04, 0x8D, 0, 0, 0, 0, 0xC3}, {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF})},
    };
}

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

} // namespace fc_plugins::weapon_swap_replay_fix
