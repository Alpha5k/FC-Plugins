#include "layout.hpp"

#include <array>
#include <string_view>

namespace fc_plugins::spectator_camera_smoothing {
namespace {

// RVAs and expected bytes for camera updates, spectator lookups and interpolation state.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t soldier_reset;
    std::uint32_t camera_update;

    std::uint32_t turn_ratio_context;

    std::uint32_t turn_ratio;

    std::uint32_t network_in_shell;
    std::uint32_t network_enabled_next;
    std::uint32_t camera_manager;

    std::uint32_t get_local_index;
    std::uint32_t get_spectator;

    std::uint32_t prediction_complete;
    std::uint32_t epsilon_complete;
    std::uint32_t frame_dispatch;
    std::uint32_t get_matrix;
    std::uint32_t get_vector;
    std::uint32_t set_matrix;
    std::uint32_t set_render_pos;
    std::uint32_t set_render_radius;
    std::uint32_t render;
    std::uint32_t objects;
    std::uint32_t object_map;
    std::uint32_t collision_manager;
    std::uint32_t on_turn;
    std::uint32_t seconds_per_turn;
    std::array<std::uint8_t, 7> prediction_complete_bytes;
    std::array<std::uint8_t, 7> epsilon_complete_bytes;
    std::array<std::uint8_t, 5> frame_dispatch_bytes;
    std::array<std::uint8_t, 5> safe_point_bytes;
    std::array<std::uint8_t, 5> blended_safe_point_bytes;

    // Expected instruction bytes, including absolute operands ignored by the masks.
    std::array<std::uint8_t, 5> camera_render_bytes;
    std::array<std::uint8_t, 33> spectator_mode_context_bytes;
    std::array<std::uint8_t, 22> active_spectator_context_bytes;
    std::array<std::uint8_t, 6> turn_ratio_context_bytes;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .soldier_reset = {0x001EB1C0},
        .camera_update = {0x0004EFD0},
        .turn_ratio_context = {0x001BA740},
        .turn_ratio = {0x01BA87B8},
        .network_in_shell = {0x003E8007},
        .network_enabled_next = {0x01A62EA8},
        .camera_manager = {0x01A30324},
        .get_local_index = {0x001B73B0},
        .get_spectator = {0x001C3950},
        .prediction_complete = 0x001BAB73,
        .epsilon_complete = 0x001BACA7,
        .frame_dispatch = 0x00132B17,
        .get_matrix = 0x00137460,
        .get_vector = 0x001378F0,
        .set_matrix = 0x002CBEF0,
        .set_render_pos = 0x002E2130,
        .set_render_radius = 0x002E21A0,
        .render = 0x002BD080,
        .objects = 0x01AC3240,
        .object_map = 0x01BA6608,
        .collision_manager = 0x01A3036C,
        .on_turn = 0x003E8E7B,
        .seconds_per_turn = 0x01A62EAC,
        .prediction_complete_bytes = {0x0F, 0xB6, 0x05, 0x10, 0x2F, 0xE6, 0x01},
        .epsilon_complete_bytes = {0xC6, 0x05, 0x10, 0x2F, 0xE6, 0x01, 0x01},
        .frame_dispatch_bytes = {0xE8, 0x94, 0x4C, 0x12, 0x00},
        .safe_point_bytes = {0xE8, 0x70, 0x69, 0x16, 0x00},
        .blended_safe_point_bytes = {0xE8, 0x36, 0x73, 0x16, 0x00},
        .camera_render_bytes = {0xE8, 0x60, 0xC6, 0x27, 0x00},
        .spectator_mode_context_bytes = {0x38, 0x15, 0x07, 0x80, 0x7E, 0x00, 0x0F, 0xB6, 0x0D, 0xA9, 0x2E,
                                         0xE6, 0x01, 0xF3, 0x0F, 0x7E, 0x44, 0x24, 0x24, 0x89, 0x44, 0x24,
                                         0x38, 0x0F, 0xB6, 0x05, 0xA8, 0x2E, 0xE6, 0x01, 0x0F, 0x45, 0xC8},
        .active_spectator_context_bytes = {0xA1, 0x24, 0x03, 0xE3, 0x01, 0xFF, 0x70, 0x50, 0xE8, 0xAB, 0x4F,
                                           0x0C, 0x00, 0x83, 0xC4, 0x04, 0x50, 0xE8, 0x42, 0x15, 0x0D, 0x00},
        .turn_ratio_context_bytes = {0xD9, 0x05, 0xB8, 0x87, 0xFA, 0x01},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .soldier_reset = {0x001EC260},
        .camera_update = {0x0004EFB0},
        .turn_ratio_context = {0x001BB6F0},
        .turn_ratio = {0x01BA9C6C},
        .network_in_shell = {0x003E9007},
        .network_enabled_next = {0x01A64358},
        .camera_manager = {0x01A317C4},
        .get_local_index = {0x001B8360},
        .get_spectator = {0x001C48E0},
        .prediction_complete = 0x001BBB23,
        .epsilon_complete = 0x001BBC57,
        .frame_dispatch = 0x00133877,
        .get_matrix = 0x001381D0,
        .get_vector = 0x00138660,
        .set_matrix = 0x002CCF90,
        .set_render_pos = 0x002E31D0,
        .set_render_radius = 0x002E3240,
        .render = 0x002BE110,
        .objects = 0x01AC46F0,
        .object_map = 0x01BA7AC0,
        .collision_manager = 0x01A3180C,
        .on_turn = 0x003E9E7B,
        .seconds_per_turn = 0x01A64368,
        .prediction_complete_bytes = {0x0F, 0xB6, 0x05, 0xC0, 0x43, 0xE6, 0x01},
        .epsilon_complete_bytes = {0xC6, 0x05, 0xC0, 0x43, 0xE6, 0x01, 0x01},
        .frame_dispatch_bytes = {0xE8, 0xD4, 0x4F, 0x12, 0x00},
        .safe_point_bytes = {0xE8, 0x10, 0x7A, 0x16, 0x00},
        .blended_safe_point_bytes = {0xE8, 0xD6, 0x83, 0x16, 0x00},
        .camera_render_bytes = {0xE8, 0x20, 0xD7, 0x27, 0x00},
        .spectator_mode_context_bytes = {0x38, 0x15, 0x07, 0x90, 0x7E, 0x00, 0x0F, 0xB6, 0x0D, 0x59, 0x43,
                                         0xE6, 0x01, 0xF3, 0x0F, 0x7E, 0x44, 0x24, 0x24, 0x89, 0x44, 0x24,
                                         0x38, 0x0F, 0xB6, 0x05, 0x58, 0x43, 0xE6, 0x01, 0x0F, 0x45, 0xC8},
        .active_spectator_context_bytes = {0xA1, 0xC4, 0x17, 0xE3, 0x01, 0xFF, 0x70, 0x50, 0xE8, 0x5B, 0x5F,
                                           0x0C, 0x00, 0x83, 0xC4, 0x04, 0x50, 0xE8, 0xD2, 0x24, 0x0D, 0x00},
        .turn_ratio_context_bytes = {0xD9, 0x05, 0x6C, 0x9C, 0xFA, 0x01},
    },
};

constexpr std::uint32_t kCameraTrackMatrix{0x000F2290};

// Mask only absolute operands that the loader rebases; instruction and control-flow bytes remain exact.
constexpr std::array<std::uint8_t, 33> kSpectatorModeMask = {
    0xFF, 0xFF, 0,    0,    0,    0,    0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 22> kActiveSpectatorMask = {
    0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 6> kTurnRatioMask = {0xFF, 0xFF, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 7> kPredictionMask = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 7> kEpsilonMask = {0xFF, 0xFF, 0, 0, 0, 0, 0xFF};

// Describes spectator observers and native globals with their instruction evidence for the selected build.
Layout make_layout(const ProfileSites &sites) {
    return {
        .soldier_reset = {.rva = sites.soldier_reset,
                          .name = "EntitySoldier::ResetObject",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .soldier_destroy = {.rva = {0x000DFA20U},
                            .name = "EntitySoldier destructor",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        .spectator_root =
            {
                .rva = (kCameraTrackMatrix + 0x24),
                .name = "spectated soldier matrix",
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x6F, 0x00}),
            },
        .camera_publication =
            {
                .rva = (sites.camera_update + 0x3A7),
                .name = "camera interpolation input",
                .evidence = fc::exact_bytes({0xE8, 0x34, 0x05, 0x00, 0x00}),
            },
        .camera_render =
            {
                .rva = (sites.camera_update + 0x8BB),
                .name = "matrix submitted to RedCamera",
                .evidence = fc::exact_bytes(sites.camera_render_bytes),
            },

        .root_context =
            {
                .rva = (kCameraTrackMatrix + 0xF),
                .name = "EntitySoldier::SetupCameraTrackMatrix",
                .evidence = fc::exact_bytes({0x8B, 0x86, 0xA8, 0xFD, 0xFF, 0xFF, 0x8D, 0x8E, 0xA8, 0xFD, 0xFF,
                                             0xFF, 0x57, 0x8B, 0x80, 0x10, 0x01, 0x00, 0x00, 0xFF, 0xD0}),
            },
        .camera_publication_context =
            {
                .rva = (sites.camera_update + 0x390),
                .name = "CameraManager::Update",
                .evidence =
                    fc::exact_bytes({0x8B, 0x46, 0x50, 0x69, 0xC8, 0xB0, 0x00, 0x00, 0x00, 0xFF, 0x74, 0x86, 0x24, 0xFF, 0x74, 0x86, 0x20}),
            },
        .camera_render_context =
            {
                .rva = (sites.camera_update + 0x87B),
                .name = "CameraInterpolator::Update",
                .evidence = fc::exact_bytes({0x8D, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x8B, 0x8E, 0xA0, 0x00, 0x00, 0x00, 0xF3, 0x0F,
                                             0x11, 0x84, 0x24, 0xE0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xF3, 0x0F,
                                             0x11, 0x84, 0x24, 0xE4, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x0C, 0x50}),
            },

        .spectator_mode_context =
            {
                .rva = (kCameraTrackMatrix + 0x139),
                .name = "spectator mode selection",
                .evidence = fc::masked_bytes(sites.spectator_mode_context_bytes, kSpectatorModeMask),
            },
        .active_spectator_context =
            {
                .rva = (kCameraTrackMatrix + 0x168),
                .name = "active spectator lookup",
                .evidence = fc::masked_bytes(sites.active_spectator_context_bytes, kActiveSpectatorMask),
            },
        .turn_ratio_context =
            {
                .rva = sites.turn_ratio_context + 3,
                .name = "NetGame::GetTurnRatio",
                .evidence = fc::masked_bytes(sites.turn_ratio_context_bytes, kTurnRatioMask),
            },

        .turn_ratio = {.rva = sites.turn_ratio, .name = "network turn ratio"},

        .network_in_shell = {.rva = sites.network_in_shell, .name = "netInShell"},
        .network_enabled_next = {.rva = sites.network_enabled_next, .name = "netEnabledNext"},
        .network_enabled = {.rva = (sites.network_enabled_next + 1), .name = "netEnabled"},
        .camera_manager = {.rva = sites.camera_manager, .name = "CameraManager::sInstance"},

        .get_local_index = {.rva = sites.get_local_index, .name = "NetGame::GetLocalIndex"},
        .get_spectator = {.rva = sites.get_spectator, .name = "NetGame::GetSpectator"},

        // Both prediction return edges precede the next native pass; epsilon also has a no-pass join.
        .prediction_complete = {.rva = sites.prediction_complete,
                                .name = "completed prediction pass",
                                .evidence = fc::masked_bytes(sites.prediction_complete_bytes, kPredictionMask)},
        .epsilon_complete = {.rva = sites.epsilon_complete,
                             .name = "completed epsilon prediction pass",
                             .evidence = fc::masked_bytes(sites.epsilon_complete_bytes, kEpsilonMask)},
        .frame_dispatch = {.rva = sites.frame_dispatch,
                           .name = "client frame Thread::UpdateAll dispatch",
                           .evidence = fc::exact_bytes(sites.frame_dispatch_bytes)},
        // Setting calls expose the exact working root and effective normal/blended local safe point.
        .safe_point = {.rva = 0x000F271B, .name = "ordinary camera safe point", .evidence = fc::exact_bytes(sites.safe_point_bytes)},
        .blended_safe_point = {.rva = 0x000F26A5,
                               .name = "blended camera safe point",
                               .evidence = fc::exact_bytes(sites.blended_safe_point_bytes)},
        .object_reset = {.rva = sites.get_matrix - 0xF,
                         .name = "GameObjectInterpolator null reset",
                         .evidence = fc::exact_bytes({0xC7, 0x04, 0x91, 0x00, 0x00, 0x00, 0x00})},
        .get_matrix = {.rva = sites.get_matrix,
                       .name = "GameObjectInterpolator::GetMatrix",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .get_vector = {.rva = sites.get_vector,
                       .name = "GameObjectInterpolator::GetVector",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18})},
        .set_matrix = {.rva = sites.set_matrix,
                       .name = "RedCamera::SetMatrix",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08})},
        .set_render_pos = {.rva = sites.set_render_pos,
                           .name = "RedSceneObject::SetRenderPos",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x80, 0x79, 0x48, 0x00})},
        .set_render_radius = {.rva = sites.set_render_radius,
                              .name = "RedSceneObject::SetRenderRadius",
                              .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x80, 0x79, 0x48, 0x00})},
        .ray_hit = {.rva = 0x0005E2B0,
                    .name = "CollisionManager endpoint RayHit",
                    .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C})},
        .render = {.rva = sites.render, .name = "FLRenderer::Render", .evidence = fc::exact_bytes({0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08})},
        .objects = {.rva = sites.objects, .name = "GameObjectInterpolator global"},
        .object_map = {.rva = sites.object_map, .name = "clientObjMap pointer slots"},
        .collision_manager = {.rva = sites.collision_manager, .name = "CollisionManager::sInstance"},
        .on_turn = {.rva = sites.on_turn, .name = "netOnTurn"},
        .seconds_per_turn = {.rva = sites.seconds_per_turn, .name = "native seconds per turn"},
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

} // namespace fc_plugins::spectator_camera_smoothing
