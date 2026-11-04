#include "layout.hpp"

#include <array>

namespace fc_plugins::collision_history_fix {
namespace {

// Steam/GOG retain the same low-address movement code; network readers and globals differ.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t predict;
    std::uint32_t predict_turn;
    std::uint32_t read;
    std::uint32_t reset;
    std::uint32_t get_joystick_index;
    std::uint32_t is_skipped;
    std::uint32_t prediction_turn;
    std::uint32_t update_turn;
    std::uint32_t local_prediction_turn;
    std::uint32_t max_update_steps;
    std::uint32_t local_prediction_pass;
    std::uint32_t network_enabled;
    std::uint32_t prediction_context;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .predict = 0x1BA810,
                 .predict_turn = 0x1BE3A0,
                 .read = 0x1EA140,
                 .reset = 0x1EB1C0,
                 .get_joystick_index = 0x1B73C0,
                 .is_skipped = 0x1B7D70,
                 .prediction_turn = 0x1BA87B4,
                 .update_turn = 0x1BA65B0,
                 .local_prediction_turn = 0x1BA8790,
                 .max_update_steps = 0x3EC504,
                 .local_prediction_pass = 0x1A62F10,
                 .network_enabled = 0x1A62EA9,
                 .prediction_context = 0x1BAA6D},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .predict = 0x1BB7C0,
                 .predict_turn = 0x1BF330,
                 .read = 0x1EB1E0,
                 .reset = 0x1EC260,
                 .get_joystick_index = 0x1B8370,
                 .is_skipped = 0x1B8D20,
                 .prediction_turn = 0x1BA9C68,
                 .update_turn = 0x1BA7A64,
                 .local_prediction_turn = 0x1BA9C44,
                 .max_update_steps = 0x3ED494,
                 .local_prediction_pass = 0x1A643C0,
                 .network_enabled = 0x1A64359,
                 .prediction_context = 0x1BBA1D},
};

// Absolute global operands are relocation-dependent; the selected profile supplies their RVAs above.
constexpr std::array<std::uint8_t, 32> kPredictionBytes{0x8A, 0x4D, 0xD4, 0x88, 0x0D, 0,    0, 0, 0,    0x8B, 0x15,
                                                        0,    0,    0,    0,    0x3B, 0x15, 0, 0, 0,    0,    0x75,
                                                        0x09, 0xC7, 0x45, 0xD0, 0x01, 0,    0, 0, 0xEB, 0x07};
constexpr std::array<std::uint8_t, 32> kPredictionMask{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF,
                                                       0,    0,    0,    0,    0xFF, 0xFF, 0,    0,    0,    0,    0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 41> kNetworkBytes{0x8A, 0x15, 0,    0,    0,    0,    0x80, 0x3D, 0,    0,    0,    0,    0,    0x0F,
                                                     0xB6, 0x05, 0,    0,    0,    0,    0x0F, 0xB6, 0xCA, 0x0F, 0x45, 0xC8, 0x84, 0xC9,
                                                     0x74, 0x4F, 0x84, 0xD2, 0x74, 0x4B, 0x80, 0x3D, 0,    0,    0,    0,    0};
constexpr std::array<std::uint8_t, 41> kNetworkMask{0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF,
                                                    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,    0,    0,    0,    0xFF};

// Constructs qualified callbacks and the instructions selecting native prediction and object worlds.
Layout make_layout(const ProfileSites &sites) {
    return {
        .predict = {.rva = sites.predict,
                    .name = "NetGame::Predict",
                    .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x80, 0, 0, 0})},
        .predict_turn = {.rva = sites.predict_turn,
                         .name = "NetGame::PredictTurn",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .update = {.rva = 0xE8920, .name = "Soldier native simulation", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .read = {.rva = sites.read, .name = "EntitySoldier::ReadObject", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .reset = {.rva = sites.reset,
                  .name = "EntitySoldier::ResetObject",
                  .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        // Both destructors replace CollisionObject's vtable, then jump to native manager removal.
        .destroy_collision_object = {.rva = 0x63BA0,
                                     .name = "CollisionObject destructor",
                                     .evidence = fc::masked_bytes({0xC7, 0x01, 0, 0, 0, 0, 0xE9, 0xB5, 0, 0, 0},
                                                                  {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})},
        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},
        .is_skipped = {.rva = sites.is_skipped,
                       .name = "NetGame::IsSkipped",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        // Both retail Soldier CollisionObject vtables select this bool thiscall response at +18.
        .collision_response = {.rva = 0xE5380,
                               .name = "EntitySoldier::CollisionCallback",
                               .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        // EDI owns the active object; ECX is the candidate. Native admission is BL != 0 or signed ECX > EDI.
        .pair_admission = {.rva = 0x5F93B,
                           .name = "collision pair ownership",
                           .evidence = fc::exact_bytes({0x84, 0xDB, 0x75, 0x04, 0x3B, 0xCF, 0x7E, 0x0F})},
        .accept_pair = {.rva = 0x5F943, .name = "accepted collision pair", .evidence = fc::exact_bytes({0x8B, 0x07})},
        // GameObject vtable +4 identifies Soldier by this getter; its absolute global operand relocates.
        .soldier_class_getter = {.rva = 0xDE050,
                                 .name = "EntitySoldier class getter",
                                 .evidence = fc::masked_bytes({0xA1, 0, 0, 0, 0, 0xC3}, {0xFF, 0, 0, 0, 0, 0xFF})},
        .prediction_turn = {.rva = sites.prediction_turn, .name = "prediction turn"},
        .update_turn = {.rva = sites.update_turn, .name = "client update turn"},
        // The local frontier precedes replay; prediction_turn is the loop counter assigned inside Predict.
        .local_prediction_turn = {.rva = sites.local_prediction_turn, .name = "local prediction frontier"},
        .max_update_steps = {.rva = sites.max_update_steps, .name = "maximum remote update steps"},
        .local_prediction_pass = {.rva = sites.local_prediction_pass, .name = "local prediction pass"},
        .late_prediction_pass = {.rva = sites.local_prediction_pass + 1, .name = "late prediction pass"},
        .network_enabled = {.rva = sites.network_enabled, .name = "network enabled"},
        .network_client_active = {.rva = sites.network_enabled + 2, .name = "client object world active"},
        .contexts = {{{.rva = sites.prediction_context,
                       .name = "prediction turn and update frontier checks",
                       .evidence = fc::masked_bytes(kPredictionBytes, kPredictionMask)},
                      {.rva = 0x83E75,
                       .name = "network enabled and client world checks",
                       .evidence = fc::masked_bytes(kNetworkBytes, kNetworkMask)}}},
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

} // namespace fc_plugins::collision_history_fix
