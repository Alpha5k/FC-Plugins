#include "handler.hpp"

#include <Windows.h>

#include <cstddef>
#include <bit>
#include <format>
#include <cstring>
#include <expected>

namespace fc_plugins::spectator_camera_smoothing {
namespace {

// Both retail CameraManager layouts place mUpdatedCameraId at this offset.
constexpr std::size_t kUpdatedCameraIdOffset = 0x50;

// The soldier base stores its index into the 64-entry object interpolator here.
constexpr std::size_t kSpectatorObjectIndexOffset = 0x21C;

// SetupCameraTrackMatrix receives EntitySoldier's embedded Trackable at this offset.
constexpr std::size_t kSpectatorTrackableOffset = 0x258;

// The concrete Chase camera keeps its Trackable owner at +0x0C.
constexpr std::size_t kChaseCameraOwnerOffset = 0x0C;

constexpr int kMaximumObjectIndex = 64;

// Tests the native bound shared by the separate spectator-player and object-interpolation index domains.
bool valid_object_index(int index) noexcept {
    return index >= 0 && index < kMaximumObjectIndex;
}

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    for (const auto *entry : {&layout->soldier_reset, &layout->soldier_destroy}) {
        plan.observe(*entry, fc::before([this](void *soldier) noexcept {
            if (active_soldier_ == soldier) {
                deactivate();
            }
        }));
    }

    turn_ratio_memory_ = plan.access(layout->turn_ratio);

    network_in_shell_memory_ = plan.access(layout->network_in_shell);
    network_enabled_next_memory_ = plan.access(layout->network_enabled_next);
    network_enabled_memory_ = plan.access(layout->network_enabled);

    camera_manager_memory_ = plan.access(layout->camera_manager);

    get_local_index_ = plan.function(layout->get_local_index);
    get_spectator_ = plan.function(layout->get_spectator);
    presentation_.plan(plan, *layout);

    // The original getters always finish first; only their presentation output is substituted.
    get_matrix_ = plan.hook(layout->get_matrix, [this](int index, Matrix *output) noexcept {
        static_cast<void>(get_matrix_.call(index, output));
        if (current_view() && !presentation_.on_turn()) {
            presentation_.matrix(index, output);
        }
    });
    get_vector_ = plan.hook(layout->get_vector, [this](int index, Vector3 *output) noexcept {
        static_cast<void>(get_vector_.call(index, output));
        if (current_view() && !presentation_.on_turn()) {
            presentation_.vector(index, output);
        }
    });
    plan.observe_instruction(layout->object_reset, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (owner_thread_ == GetCurrentThreadId()) {
                                     presentation_.reset_slot(reinterpret_cast<void *>(context.ecx), static_cast<int>(context.edx));
                                 }
                             }));
    for (const auto *site : {&layout->prediction_complete, &layout->epsilon_complete}) {
        plan.observe_instruction(*site, fc::before([this](const fc::CpuContext &) noexcept {
            if (owner_thread_ != 0 && owner_thread_ != GetCurrentThreadId()) {
                return;
            }
            const bool active = current_view();
            presentation_.complete(active ? active_interpolator_ : nullptr, active ? active_soldier_ : nullptr);
        }));
    }
    plan.observe_instruction(layout->safe_point, fc::before([this](const fc::CpuContext &context) noexcept {
                                 capture_safe_point(context, false);
                             }));
    plan.observe_instruction(layout->blended_safe_point, fc::before([this](const fc::CpuContext &context) noexcept {
                                 capture_safe_point(context, true);
                             }));
    plan.observe_instruction(layout->frame_dispatch, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (owner_thread_ != 0 && owner_thread_ != GetCurrentThreadId()) {
                                     return;
                                 }
                                 if (!current_view() || presentation_.on_turn()) {
                                     presentation_.reset();
                                     return;
                                 }
                                 const float delta = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
                                 presentation_.frame(active_interpolator_, active_soldier_, active_red_camera_, tracked_object_index_,
                                                     delta, *turn_ratio_);
                             }));
    render_ = plan.hook(layout->render, [this]() noexcept {
        Presentation::Bounds bounds{};
        if (current_view() && !presentation_.on_turn()) {
            presentation_.begin_bounds(bounds);
        }
        static_cast<void>(render_.call());
        presentation_.end_bounds(bounds);
    });

    // Check the instructions that supply observer registers, stack arguments and native globals.
    for (const auto *site : {&layout->root_context, &layout->camera_publication_context, &layout->camera_render_context,
                             &layout->spectator_mode_context, &layout->active_spectator_context, &layout->turn_ratio_context}) {
        plan.check(*site, site->evidence.bytes.size());
    }

    // The root and camera callbacks establish the target used by the display cache.
    plan.observe_instruction(layout->spectator_root, fc::before([this](const fc::CpuContext &context) noexcept {
                                 // ESI identifies the embedded Trackable; EAX points to the soldier root matrix before its copy.
                                 auto *soldier = context.esi < kSpectatorTrackableOffset
                                                     ? nullptr
                                                     : reinterpret_cast<void *>(context.esi - kSpectatorTrackableOffset);
                                 record_spectator_root(soldier, reinterpret_cast<const float *>(context.eax));
                             }));

    plan.observe_instruction(layout->camera_publication, fc::before([this](const fc::CpuContext &context) noexcept {
                                 if (context.ecx == 0 || fc::stack_pointer(context) == 0) {
                                     return;
                                 }

                                 // SetNext receives GameCamera and RedCamera on the stack, with the interpolator in ECX.
                                 const auto stack = fc::stack_pointer(context);
                                 record_camera_publication(reinterpret_cast<void *>(context.ecx), fc::read_field<void *>(stack),
                                                           fc::read_field<void *>(stack, sizeof(void *)));
                             }));

    plan.hook_instruction(layout->camera_render, [this](fc::CpuContext &context) noexcept {
        if (context.ecx != 0 && fc::stack_pointer(context) != 0) {
            auto *matrix = fc::read_field<Matrix *>(fc::stack_pointer(context));
            if (current_view() && !presentation_.on_turn() && reinterpret_cast<void *>(context.ecx) == active_red_camera_ &&
                matrix != nullptr) {
                presentation_.camera(matrix);
            }
        }
    });
}

fc::Result<> Handler::prepare(fc::PrepareContext &) {
    // Retain pointers into runtime-owned views for the installed observers.
    const auto bind_view = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }

        pointer = view->data();
        return {};
    };

    if (auto result = bind_view(turn_ratio_memory_, turn_ratio_); !result) {
        return result;
    }

    if (auto result = bind_view(network_in_shell_memory_, network_in_shell_); !result) {
        return result;
    }

    if (auto result = bind_view(network_enabled_next_memory_, network_enabled_next_); !result) {
        return result;
    }

    if (auto result = bind_view(network_enabled_memory_, network_enabled_); !result) {
        return result;
    }

    if (auto result = bind_view(camera_manager_memory_, camera_manager_); !result) {
        return result;
    }
    return presentation_.prepare();
}

void Handler::record_spectator_root(void *soldier, const float *matrix) noexcept {
    if (soldier == nullptr || matrix == nullptr) {
        deactivate();
        return;
    }

    int spectator_index{};
    if (!is_actively_spectating(spectator_index)) {
        deactivate();
        return;
    }

    // This native callback also visits other soldiers; bind only the selected target player.
    if (fc::read_field<int>(soldier, 0x314) != spectator_index) {
        return;
    }
    const int object_index = fc::read_field<int>(soldier, kSpectatorObjectIndexOffset);
    if (!valid_object_index(object_index) || object_index == kNullObjectSlot) {
        deactivate();
        return;
    }

    const auto thread = GetCurrentThreadId();
    const bool target_changed = active_soldier_ != soldier || active_spectator_index_ != spectator_index ||
                                tracked_object_index_ != object_index || owner_thread_ != thread;

    // Clear the old target's history before selecting its replacement, including a thread change.
    if (target_changed && active_soldier_ != nullptr) {
        deactivate();
    }

    if (target_changed) {
        active_soldier_ = soldier;
        active_spectator_index_ = spectator_index;
        tracked_object_index_ = object_index;
        owner_thread_ = thread;
        active_red_camera_ = nullptr;
    }
}

void Handler::record_camera_publication(void *interpolator, void *game_camera, void *red_camera) noexcept {
    if (!current_target() || active_soldier_ == nullptr || interpolator == nullptr || game_camera == nullptr || red_camera == nullptr ||
        owner_thread_ != GetCurrentThreadId()) {
        return;
    }

    // Paired native CameraManager code owns its concrete Chase camera at +0x28 (one retail camera slot).
    // Qualify that object before interpreting its Trackable member; other camera types remain unattributed.
    if (game_camera != fc::read_field<void *>(*camera_manager_, 0x28)) {
        presentation_.reset();
        active_red_camera_ = nullptr;
        active_game_camera_ = nullptr;
        active_interpolator_ = nullptr;
        return;
    }
    const auto adjusted_target = fc::read_field<void *>(game_camera, kChaseCameraOwnerOffset);
    const auto expected_target = static_cast<std::byte *>(active_soldier_) + kSpectatorTrackableOffset;
    if (adjusted_target != expected_target) {
        presentation_.reset();
        active_red_camera_ = nullptr;
        active_game_camera_ = nullptr;
        active_interpolator_ = nullptr;
        return;
    }

    if (active_red_camera_ != red_camera || active_game_camera_ != game_camera || active_interpolator_ != interpolator) {
        presentation_.reset_camera();
    }
    active_red_camera_ = red_camera;
    active_game_camera_ = game_camera;
    active_interpolator_ = interpolator;
}

void Handler::deactivate() noexcept {
    presentation_.reset();
    if (active_soldier_ == nullptr) {
        return;
    }

    active_soldier_ = nullptr;
    active_red_camera_ = nullptr;
    active_game_camera_ = nullptr;
    active_interpolator_ = nullptr;
    tracked_object_index_ = -1;
    active_spectator_index_ = -1;
    owner_thread_ = 0;
}

bool Handler::current_target() noexcept {
    if (active_soldier_ == nullptr || owner_thread_ != GetCurrentThreadId()) {
        return false;
    }
    int spectator_index{};
    if (!is_actively_spectating(spectator_index) || spectator_index != active_spectator_index_) {
        deactivate();
        return false;
    }
    if (!presentation_.mapped(tracked_object_index_, active_soldier_)) {
        deactivate();
        return false;
    }
    return true;
}

bool Handler::current_view() noexcept {
    if (!current_target() || active_game_camera_ == nullptr || active_red_camera_ == nullptr || active_interpolator_ == nullptr) {
        return false;
    }
    if (fc::read_field<void *>(*camera_manager_, 0x28) != active_game_camera_ ||
        fc::read_field<void *>(active_game_camera_, kChaseCameraOwnerOffset) !=
            static_cast<std::byte *>(active_soldier_) + kSpectatorTrackableOffset ||
        fc::read_field<void *>(active_interpolator_, kInterpolatorRedCameraOffset) != active_red_camera_ ||
        fc::read_field<void *>(active_interpolator_, kInterpolatorGameCameraOffset) != active_game_camera_) {
        presentation_.reset();
        return false;
    }
    return true;
}

void Handler::capture_safe_point(const fc::CpuContext &context, bool blended) noexcept {
    if (!current_target() || context.ecx == 0 ||
        context.esi != reinterpret_cast<std::uintptr_t>(active_soldier_) + kSpectatorTrackableOffset) {
        return;
    }
    const auto stack = fc::stack_pointer(context);
    if (stack == 0 || fc::read_field<void *>(stack, blended ? 0x20 : 0x18) != active_soldier_) {
        return;
    }
    const auto *root = fc::read_field<const Matrix *>(stack, blended ? 0x10 : 0x08);
    Vector3 local;
    std::memcpy(local.data(), reinterpret_cast<const void *>(context.ecx + kSettingSafePointOffset), sizeof(local));
    if (blended) {
        const auto *previous = fc::read_field<const std::byte *>(stack, 0x04);
        if (previous == nullptr) {
            return;
        }
        Vector3 old;
        std::memcpy(old.data(), previous + kSettingSafePointOffset, sizeof(old));
        const float fraction = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            local[axis] = old[axis] + (local[axis] - old[axis]) * fraction;
        }
    }
    presentation_.safe_point(active_soldier_, root, local, blended ? 0x90 : 0x98);
}

bool Handler::is_actively_spectating(int &spectator_index) const noexcept {
    spectator_index = -1;

    // The shell stages its next network state separately from the active in-game state.
    const bool network_active = *network_in_shell_ != 0 ? *network_enabled_next_ != 0 : *network_enabled_ != 0;
    if (!network_active || *camera_manager_ == nullptr) {
        return false;
    }

    const int camera_index = fc::read_field<int>(*camera_manager_, kUpdatedCameraIdOffset);

    if (camera_index != 0) {
        return false;
    }

    // CameraManager's current view selects a local player, whose spectator field names the target player.
    const auto local = get_local_index_.call(camera_index);
    if (!local) {
        return false;
    }

    const auto spectator = get_spectator_.call(*local);
    if (!spectator) {
        return false;
    }

    spectator_index = *spectator;
    return valid_object_index(spectator_index);
}

} // namespace fc_plugins::spectator_camera_smoothing
