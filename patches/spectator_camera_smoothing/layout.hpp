#pragma once

#include "motion.hpp"

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::spectator_camera_smoothing {

using LifetimeFunction = void(__thiscall *)(void *soldier) noexcept;

// Retail cdecl lookups map a camera/local-view index to a local player and then to its spectator target.
using GetLocalIndex = std::int32_t(__cdecl *)(std::int32_t camera_index) noexcept;
using GetSpectator = std::int32_t(__cdecl *)(std::int32_t local_player) noexcept;

// Retail getters ignore ECX and remove their two explicit stack arguments.
using GetMatrix = void(__stdcall *)(int index, Matrix *output) noexcept;
using GetVector = void(__stdcall *)(int index, Vector3 *output) noexcept;
using SetMatrix = void(__thiscall *)(void *camera, const Matrix *matrix) noexcept;
using SetRenderPos = void(__thiscall *)(void *scene, const Vector3 *position) noexcept;
using SetRenderRadius = void(__thiscall *)(void *scene, float radius) noexcept;
using Render = void(__cdecl *)() noexcept;

// Both qualified retail profiles share these native counts and offsets.
inline constexpr std::size_t kObjectSlotCount = 64;
inline constexpr int kNullObjectSlot = 63;
inline constexpr std::size_t kObjectIndexOffset = 0x21C;
inline constexpr std::size_t kSceneObjectOffset = 0x94; // RedSceneObject embedded in GameObject.
inline constexpr std::size_t kSceneSphereOffset = 0x30;
inline constexpr std::size_t kSceneRadiusOffset = 0x3C;
inline constexpr std::size_t kObjectRootPositionOffset = 0x120; // Translation in GameObject's raw root matrix at +0xF0.
inline constexpr std::size_t kCameraCountOffset = 0x18;
inline constexpr std::size_t kCameraEndpointsOffset = 0x20;
inline constexpr std::size_t kInterpolatorRedCameraOffset = 0xA0;
inline constexpr std::size_t kInterpolatorGameCameraOffset = 0xA4;
inline constexpr std::size_t kCameraNearPlaneOffset = 0x130;
inline constexpr std::size_t kLastRayBodyOffset = 0xE0;
inline constexpr std::size_t kSettingSafePointOffset = 0x24;

// Endpoint RayHit uses register endpoints, caller cleanup and an SSE float result.
using RayHit = fc::NativeCall<float(const Vector3 *start, const Vector3 *end, void **hit, Vector3 *normal, void **ignored,
                                    int ignored_count, std::uint32_t mask, bool enabled) noexcept,
                              fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx, fc::abi::Stack<0>, fc::abi::Stack<4>,
                                                         fc::abi::Stack<8>, fc::abi::Stack<12>, fc::abi::Stack<16>, fc::abi::Stack<20>>,
                                           fc::abi::Result<fc::abi::Xmm0>>>;

// The native service retains two endpoints for each of its 64 slots; slot 63 is the null sentinel.
struct NativeObjects {
    std::array<std::int32_t, kObjectSlotCount> count;
    std::array<std::array<Matrix, 2>, kObjectSlotCount> matrix;
    std::array<std::array<std::array<float, 2>, 3>, kObjectSlotCount> scalar;
    std::array<std::array<Vector3, 2>, kObjectSlotCount> vector;
};

// clientObjMap holds pointers to these native records; uniqId plus observed resets distinguishes slot lifetimes.
struct NativeMapEntry {
    void *object;
    std::int32_t unique_id;
    std::int32_t used_turn;
};

using NativeMap = std::array<NativeMapEntry *, kObjectSlotCount>;
static_assert(sizeof(NativeObjects) == 11520);
static_assert(offsetof(NativeObjects, matrix) == 0x100);
static_assert(offsetof(NativeObjects, vector) == 0x2700);
static_assert(sizeof(NativeMapEntry) == 12);

// Describes the spectator camera, object publication, and render boundaries for one retail executable.
struct Layout {
    fc::FunctionLocation<LifetimeFunction> soldier_reset;
    fc::FunctionLocation<LifetimeFunction> soldier_destroy;
    fc::CodeLocation spectator_root;
    fc::CodeLocation camera_publication;
    fc::CodeLocation camera_render;

    fc::CodeLocation root_context;
    fc::CodeLocation camera_publication_context;
    fc::CodeLocation camera_render_context;

    fc::CodeLocation spectator_mode_context;
    fc::CodeLocation active_spectator_context;
    fc::CodeLocation turn_ratio_context;

    fc::DataLocation<const float> turn_ratio; // Shared object/camera interpolation fraction.

    fc::DataLocation<const std::uint8_t> network_in_shell;
    fc::DataLocation<const std::uint8_t> network_enabled_next;
    fc::DataLocation<const std::uint8_t> network_enabled;

    fc::DataLocation<void *const> camera_manager; // CameraManager::sInstance, borrowed from the game.

    fc::FunctionLocation<GetLocalIndex> get_local_index;
    fc::FunctionLocation<GetSpectator> get_spectator;

    fc::CodeLocation prediction_complete;
    fc::CodeLocation epsilon_complete;
    fc::CodeLocation frame_dispatch;
    fc::CodeLocation safe_point;
    fc::CodeLocation blended_safe_point;
    fc::CodeLocation object_reset;
    fc::FunctionLocation<GetMatrix> get_matrix;
    fc::FunctionLocation<GetVector> get_vector;
    fc::FunctionLocation<SetMatrix> set_matrix;
    fc::FunctionLocation<SetRenderPos> set_render_pos;
    fc::FunctionLocation<SetRenderRadius> set_render_radius;
    fc::FunctionLocation<RayHit> ray_hit;
    fc::FunctionLocation<Render> render;
    fc::DataLocation<const NativeObjects> objects;
    fc::DataLocation<const NativeMap> object_map;
    fc::DataLocation<void *const> collision_manager;
    fc::DataLocation<const std::uint8_t> on_turn;
    fc::DataLocation<const float> seconds_per_turn;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::spectator_camera_smoothing
