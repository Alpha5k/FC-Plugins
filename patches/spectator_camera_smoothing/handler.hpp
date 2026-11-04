#pragma once

#include "layout.hpp"
#include "presentation.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <cstdint>

namespace fc_plugins::spectator_camera_smoothing {

// Smooths the validated spectator's camera and native scene interpolation at one shared presentation time.
class Handler final {
  public:
    // Declares the hooks and native data used to smooth the spectator view.
    void plan(fc::Plan &plan);

    // Resolves native views before installation.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // Binds a target from the soldier root matrix or ends tracking when this callback observes an inactive target.
    void record_spectator_root(void *soldier, const float *matrix) noexcept;

    // Binds the selected target's cameras and interpolator for the next completed prediction pass.
    void record_camera_publication(void *interpolator, void *game_camera, void *red_camera) noexcept;

    // Clears the history and target when selection changes or the native soldier is reset or destroyed.
    void deactivate() noexcept;

    // Revalidates target selection without dereferencing a cached soldier.
    bool current_target() noexcept;

    // Revalidates the concrete Chase owner and current native camera pointers before using the display cache.
    bool current_view() noexcept;

    // Copies ordinary or blended settings metadata from the exact native call arguments.
    void capture_safe_point(const fc::CpuContext &context, bool blended) noexcept;

    // Retrieves the native spectator index, or false when no network spectator is active.
    bool is_actively_spectating(int &spectator_index) const noexcept;

    fc::Memory<const float> turn_ratio_memory_;
    const float *turn_ratio_{};

    fc::Memory<const std::uint8_t> network_in_shell_memory_;
    const std::uint8_t *network_in_shell_{};

    fc::Memory<const std::uint8_t> network_enabled_next_memory_;
    const std::uint8_t *network_enabled_next_{};

    fc::Memory<const std::uint8_t> network_enabled_memory_;
    const std::uint8_t *network_enabled_{};

    fc::Memory<void *const> camera_manager_memory_;
    void *const *camera_manager_{};

    fc::NativeFunction<GetLocalIndex> get_local_index_;
    fc::NativeFunction<GetSpectator> get_spectator_;
    fc::NativeFunction<GetMatrix> get_matrix_;
    fc::NativeFunction<GetVector> get_vector_;
    fc::NativeFunction<Render> render_;
    Presentation presentation_;

    void *active_soldier_{};         // Borrowed soldier identity established by the root callback.
    void *active_red_camera_{};      // Borrowed RedCamera identity established by a matching camera endpoint.
    void *active_game_camera_{};     // Concrete Chase camera verified against CameraManager.
    void *active_interpolator_{};    // CameraInterpolator whose complete publications seed the scene history.
    int tracked_object_index_{-1};   // Index into GameObjectInterpolator, distinct from the player index.
    int active_spectator_index_{-1}; // Native target player index.
    // Filters camera and frame callbacks to the thread that selected this target.
    // Selection and lifetime callbacks can replace or clear the target directly.
    std::uint32_t owner_thread_{};
};

} // namespace fc_plugins::spectator_camera_smoothing
