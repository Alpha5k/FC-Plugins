#include "layout.hpp"

#include <array>
#include <string>
#include <string_view>

namespace fc_plugins::hero_animation_fix {
namespace {

// RVAs and expected bytes for melee hooks, turn counters and network flags in each supported image.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t update;
    std::uint32_t read;
    std::uint32_t set_network_state;
    std::uint32_t enter_state;
    std::uint32_t clear_input;
    std::uint32_t destroy;
    std::uint32_t jump_restart;
    std::uint32_t upper_selection;
    std::uint32_t lower_selection;
    std::uint32_t soldier_reset;
    std::uint32_t soldier_read;
    std::uint32_t gait;
    std::uint32_t melee_vtable;
    std::uint32_t melee_vtable_operand;
    std::uint32_t get_joystick_index;
    std::uint32_t completed_turn;
    std::uint32_t fallback;
    std::uint32_t turn_duration;
    std::uint32_t predict;
    std::uint32_t predict_turn;
    std::uint32_t collision_start;
    std::uint32_t collision_return;
    std::uint32_t effects_start;
    std::uint32_t effects_timer;
    std::uint32_t ordnance_render;

    std::uint32_t prediction_turn;
    std::uint32_t acknowledged_turn;

    std::uint32_t local_prediction_pass;

    std::uint32_t network_enabled;

    std::uint32_t prediction_pass_context;
    std::uint32_t acknowledged_turn_context;

    // Expected instruction bytes, including absolute operands ignored by the masks.
    std::array<std::uint8_t, 32> prediction_pass_context_bytes;
    std::array<std::uint8_t, 15> authoritative_pass_context_bytes;
    std::array<std::uint8_t, 12> acknowledged_turn_context_bytes;
    std::array<std::uint8_t, 41> network_state_context_bytes;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .update = {0x00289290},
        .read = {0x001E8B20},
        .set_network_state = {0x0028B100},
        .enter_state = {0x0028BC30},
        .clear_input = {0x0028B330},
        .destroy = {0x00288430},
        .jump_restart = 0x0023EC60,
        .upper_selection = 0x00241E10,
        .lower_selection = 0x002420D0,
        .soldier_reset = 0x001EB1C0,
        .soldier_read = 0x001EA140,
        .gait = 0x000F3DB0,
        .melee_vtable = 0x003B1578,
        .melee_vtable_operand = 0x002882AB,
        .get_joystick_index = {0x001B73C0},
        .completed_turn = 0x001BABDC,
        .fallback = 0x001BAC9A,
        .turn_duration = 0x01A62EAC,
        .predict = 0x001BA810,
        .predict_turn = 0x001BE3A0,
        .collision_start = 0x0028C3C3,
        .collision_return = 0x0028D171,
        .effects_start = 0x00289024,
        .effects_timer = 0x00289264,
        .ordnance_render = 0x002034E0,
        .prediction_turn = {0x01BA87B4},
        .acknowledged_turn = {0x01BA6594},
        .local_prediction_pass = {0x01A62F10},
        .network_enabled = {0x01A62EA9},
        .prediction_pass_context = {0x001BAA6D},
        .acknowledged_turn_context = {0x001BBA39},
        .prediction_pass_context_bytes = {0x8A, 0x4D, 0xD4, 0x88, 0x0D, 0x10, 0x2F, 0xE6, 0x01, 0x8B, 0x15, 0xB4, 0x87, 0xFA, 0x01, 0x3B,
                                          0x15, 0xB0, 0x65, 0xFA, 0x01, 0x75, 0x09, 0xC7, 0x45, 0xD0, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x07},
        .authoritative_pass_context_bytes = {0xC7, 0x45, 0xD0, 0, 0, 0, 0, 0x8A, 0x45, 0xD0, 0xA2, 0x2C, 0x4A, 0xE6, 0x01},
        .acknowledged_turn_context_bytes = {0xA1, 0xE4, 0x4D, 0xE6, 0x01, 0x2B, 0xC2, 0xA3, 0x94, 0x65, 0xFA, 0x01},
        .network_state_context_bytes = {0x8A, 0x15, 0xA9, 0x2E, 0xE6, 0x01, 0x80, 0x3D, 0x07, 0x80, 0x7E, 0x00, 0x00, 0x0F,
                                        0xB6, 0x05, 0xA8, 0x2E, 0xE6, 0x01, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9,
                                        0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B, 0x80, 0x3D, 0xAB, 0x2E, 0xE6, 0x01, 0x00},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .update = {0x0028A320},
        .read = {0x001E9BC0},
        .set_network_state = {0x0028C190},
        .enter_state = {0x0028CCC0},
        .clear_input = {0x0028C3C0},
        .destroy = {0x002894B0},
        .jump_restart = 0x0023FD00,
        .upper_selection = 0x00242EB0,
        .lower_selection = 0x00243170,
        .soldier_reset = 0x001EC260,
        .soldier_read = 0x001EB1E0,
        .gait = 0x000F3DB0,
        .melee_vtable = 0x003B24F0,
        .melee_vtable_operand = 0x0028932B,
        .get_joystick_index = {0x001B8370},
        .completed_turn = 0x001BBB8C,
        .fallback = 0x001BBC4A,
        .turn_duration = 0x01A64368,
        .predict = 0x001BB7C0,
        .predict_turn = 0x001BF330,
        .collision_start = 0x0028D453,
        .collision_return = 0x0028E201,
        .effects_start = 0x0028A0B4,
        .effects_timer = 0x0028A2F4,
        .ordnance_render = 0x00204570,
        .prediction_turn = {0x01BA9C68},
        .acknowledged_turn = {0x01BA7A48},
        .local_prediction_pass = {0x01A643C0},
        .network_enabled = {0x01A64359},
        .prediction_pass_context = {0x001BBA1D},
        .acknowledged_turn_context = {0x001BC9E9},
        .prediction_pass_context_bytes = {0x8A, 0x4D, 0xD4, 0x88, 0x0D, 0xC0, 0x43, 0xE6, 0x01, 0x8B, 0x15, 0x68, 0x9C, 0xFA, 0x01, 0x3B,
                                          0x15, 0x64, 0x7A, 0xFA, 0x01, 0x75, 0x09, 0xC7, 0x45, 0xD0, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x07},
        .authoritative_pass_context_bytes = {0xC7, 0x45, 0xD0, 0, 0, 0, 0, 0x8A, 0x45, 0xD0, 0xA2, 0xDC, 0x5E, 0xE6, 0x01},
        .acknowledged_turn_context_bytes = {0xA1, 0x94, 0x62, 0xE6, 0x01, 0x2B, 0xC2, 0xA3, 0x48, 0x7A, 0xFA, 0x01},
        .network_state_context_bytes = {0x8A, 0x15, 0x59, 0x43, 0xE6, 0x01, 0x80, 0x3D, 0x07, 0x90, 0x7E, 0x00, 0x00, 0x0F,
                                        0xB6, 0x05, 0x58, 0x43, 0xE6, 0x01, 0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9,
                                        0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B, 0x80, 0x3D, 0x5B, 0x43, 0xE6, 0x01, 0x00},
    },
};

constexpr std::array<std::uint8_t, 32> kPredictionPassContextMask{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF,
                                                                  0,    0,    0,    0,    0xFF, 0xFF, 0,    0,    0,    0,    0xFF,
                                                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 15> kAuthoritativePassContextMask{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0xFF, 0,    0,    0,    0};
constexpr std::array<std::uint8_t, 12> kAcknowledgedTurnContextMask{0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 41> kNetworkStateContextMask{
    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,   0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0, 0, 0, 0xFF};

// Describes melee hooks, counters and flags with their instruction evidence for the selected build.
Layout make_layout(const ProfileSites &sites) {
    // Function entries support synchronous hooks and observers; combo audio uses two qualified CALL sites.
    return {
        // Predict has completed ThreadUpdateAll and advanced its counter before the optional 0.0001 update.
        .completed_turn = {.rva = sites.completed_turn,
                           .name = "completed native prediction turn",
                           .evidence = fc::masked_bytes({0x0F, 0xB6, 0x05, 0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0, 0, 0, 0})},
        .fallback = {.rva = sites.fallback,
                     .name = "native fallback update",
                     .evidence = fc::masked_bytes({0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0})},
        .turn_duration = {.rva = sites.turn_duration, .name = "native network turn duration"},
        .predict = {.rva = sites.predict,
                    .name = "NetGame::Predict",
                    .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x80, 0, 0, 0})},
        .predict_turn = {.rva = sites.predict_turn,
                         .name = "NetGame::PredictTurn",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .update = {.rva = sites.update, .name = "WeaponMelee::Update"},
        // Both retail readers use ECX=this and a four-byte packet argument; RET 4 ends the full read.
        .read = {.rva = sites.read,
                 .name = "WeaponMelee::Read",
                 .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0x75, 0x08, 0x57, 0x56, 0x8B, 0xF9})},
        .set_network_state = {.rva = sites.set_network_state, .name = "WeaponMelee::SetNetworkState"},
        .enter_state = {.rva = sites.enter_state, .name = "WeaponMelee::EnterState"},
        // Select, Deselect and soldier-state notifications call this reset of thrust, time, buttons and queue.
        .clear_input = {.rva = sites.clear_input,
                        .name = "WeaponMelee::ClearInput",
                        .evidence = fc::exact_bytes({0xC7, 0x81, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
        // The vtable's deleting destructor calls this body before storage can be freed or reused.
        .destroy = {.rva = sites.destroy, .name = "WeaponMelee::~WeaponMelee", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        .soldier_update = {.rva = 0x000E8920,
                           .name = "Soldier native simulation",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .soldier_read = {.rva = sites.soldier_read,
                         .name = "EntitySoldier::ReadObject",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        // Both retail images call this saved-position classifier from 4ECF8D and 4F3229.
        .gait = {.rva = sites.gait,
                 .name = "EntitySoldier gait classifier",
                 .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .soldier_reset = {.rva = sites.soldier_reset,
                          .name = "EntitySoldier::ResetObject",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .soldier_destroy = {.rva = 0x000DFA20,
                            .name = "EntitySoldier destructor",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        .jump_restart = {.rva = sites.jump_restart,
                         .name = "SoldierAnimator::RestartJumpAnimation",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08})},
        .animation_update = {.rva = sites.jump_restart + 0x1C00,
                             .name = "SoldierAnimator jump reset consumer",
                             .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        // This helper only binds and starts native sounds; AC/F8/C4 writes remain in its caller.
        .fire_audio = {.rva = sites.profile == "BF2.Steam.59EDE353" ? 0x00278AA0U : 0x00279B40U,
                       .name = "Weapon fire audio helper",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
        // Retail SetWeaponComboState consumes five stack arguments and writes animator presentation.
        .animator_state = {.rva = sites.jump_restart + 0xD30, .name = "SoldierAnimator::SetWeaponComboState"},
        // Both retail entries check the Renderable's owner at +0x58, its generation and the weapon pointers.
        .ordnance_render = {.rva = sites.ordnance_render,
                            .name = "OrdnanceMeleeThrow::Render",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x41, 0x58})},
        .combo_audio = {{{.rva = sites.enter_state + 0x210,
                          .name = "melee entry sound",
                          .evidence = sites.profile == "BF2.Steam.59EDE353" ? fc::exact_bytes({0xE8, 0xCB, 0xC1, 0xEA, 0xFF})
                                                                            : fc::exact_bytes({0xE8, 0xAB, 0xBE, 0xEA, 0xFF})},
                         {.rva = sites.enter_state + 0x41D,
                          .name = "melee timed sound",
                          .evidence = sites.profile == "BF2.Steam.59EDE353" ? fc::exact_bytes({0xE8, 0xBE, 0xBF, 0xEA, 0xFF})
                                                                            : fc::exact_bytes({0xE8, 0x9E, 0xBC, 0xEA, 0xFF})}}},
        .animation_selection = {{{.rva = sites.upper_selection,
                                  .name = "upper animation selection",
                                  .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C})},
                                 {.rva = sites.lower_selection,
                                  .name = "lower animation selection",
                                  .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24})}}},
        // Native SetAnim CALL returns after five bytes; EDI still holds this selector's animator.
        .animation_installed = {{{.rva = sites.upper_selection + 0x1C2, .name = "upper animation installed"},
                                 {.rva = sites.lower_selection + 0x1B3, .name = "lower animation installed"}}},
        // Matching clips with no reset tick playback and return before SetAnim; EDI still holds the animator.
        .animation_unchanged = {{{.rva = sites.upper_selection + 0x42,
                                  .name = "upper animation unchanged",
                                  .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x87, 0xD0, 0x1F, 0x00, 0x00})},
                                 {.rva = sites.lower_selection + 0x3E,
                                  .name = "lower animation unchanged",
                                  .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x87, 0xCC, 0x1F, 0x00, 0x00})}}},
        // The constructor's relocated MOV operand supplies the actual melee vtable address.
        .melee_vtable = {.rva = sites.melee_vtable_operand,
                         .name = "WeaponMelee constructor vtable operand",
                         .evidence = fc::points_to(fc::Location{.image = std::string(sites.profile), .rva = sites.melee_vtable})},
        // UpdateFire has advanced generic time and trails before this MOVSS; EDI holds the weapon.
        .collision_start = {.rva = sites.collision_start,
                            .name = "melee collision sweep",
                            .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0xA7, 0x88, 0x01, 0x00, 0x00})},
        // The native epilogue dismantles the aligned frame and returns true with RET 4.
        .collision_return = {.rva = sites.collision_return,
                             .name = "melee collision return",
                             .evidence = fc::exact_bytes({0x5F, 0xB0, 0x01, 0x5E, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00})},
        // UpdateEffects has saved its XMM1 duration at ESP+14; EBX holds the melee weapon.
        .effects_start = {.rva = sites.effects_start,
                          .name = "melee sound resource updates",
                          .evidence = fc::exact_bytes({0x8A, 0x83, 0x79, 0x01, 0x00, 0x00})},
        // This native tail advances saber turn-on time before restoring registers and returning.
        .effects_timer = {.rva = sites.effects_timer,
                          .name = "melee effects timer tail",
                          .evidence = fc::exact_bytes(
                              {0xF3, 0x0F, 0x10, 0x83, 0xA0, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x58, 0x44, 0x24, 0x14, 0x5F, 0x5E})},
        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},

        .prediction_turn = {.rva = sites.prediction_turn, .name = "prediction turn"},
        .acknowledged_turn = {.rva = sites.acknowledged_turn, .name = "acknowledged local input turn"},
        .update_turn = {.rva = sites.acknowledged_turn + 0x1C, .name = "client update turn"},

        .local_prediction_pass = {.rva = sites.local_prediction_pass, .name = "local prediction pass"},

        .network_enabled = {.rva = sites.network_enabled, .name = "network enabled"},
        .network_client_active = {.rva = sites.network_enabled + 2, .name = "client object world active"},

        // Context checks mask global addresses while retaining the counter and flag access instructions.
        .prediction_pass_context =
            {
                .rva = sites.prediction_pass_context,
                .name = "prediction pass state",
                .evidence = fc::masked_bytes(sites.prediction_pass_context_bytes, kPredictionPassContextMask),
            },
        .authoritative_pass_context =
            {
                .rva = sites.prediction_pass_context + 0x20,
                .name = "client update pass flag assignment",
                .evidence = fc::masked_bytes(sites.authoritative_pass_context_bytes, kAuthoritativePassContextMask),
            },
        .acknowledged_turn_context =
            {
                .rva = sites.acknowledged_turn_context,
                .name = "acknowledged local input turn update",
                .evidence = fc::masked_bytes(sites.acknowledged_turn_context_bytes, kAcknowledgedTurnContextMask),
            },
        .network_state_context =
            {
                .rva = {0x00083E75},
                .name = "network enabled and client world checks",
                .evidence = fc::masked_bytes(sites.network_state_context_bytes, kNetworkStateContextMask),
            },
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

} // namespace fc_plugins::hero_animation_fix
