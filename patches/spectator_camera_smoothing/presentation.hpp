#pragma once

#include "layout.hpp"
#include "motion.hpp"

#include <fc/plugin.hpp>

#include <array>
#include <cstdint>

namespace fc_plugins::spectator_camera_smoothing {

// Smooths camera, object and aim movement together from completed prediction samples.
class Presentation final {
  public:
    // Declares the native data and functions used to display the smoothed frame.
    void plan(fc::Plan &plan, const Layout &layout);

    // Retains the prepared native views for the installed callbacks.
    fc::Result<> prepare();

    // Clears the shared history when the target or camera changes.
    void reset() noexcept;

    // Clears the old camera's history while preserving the new camera's captured collision origin.
    void reset_camera() noexcept;

    // Invalidates one native slot before its SetNext(null) reset runs.
    void reset_slot(void *table, int index) noexcept;

    // Converts the camera setting's safe point to world coordinates using the native call's root matrix.
    void safe_point(void *target, const Matrix *root, const Vector3 &local, std::uint32_t mask) noexcept;

    // Records camera and object endpoints after a complete prediction pass.
    void complete(void *interpolator, void *target) noexcept;

    // Builds the smoothed frame and checks camera clearance before native frame callbacks use it.
    void frame(void *interpolator, void *target, void *camera, int target_index, float delta, float ratio) noexcept;

    // Checks whether the native map still assigns this object to the given slot.
    bool mapped(int index, void *object) const noexcept;

    // Copies the cached result for a live slot, leaving the native output intact when unavailable.
    void matrix(int index, Matrix *output) const noexcept;

    // Copies the cached aim for a live slot, leaving the native output intact when unavailable.
    void vector(int index, Vector3 *output) const noexcept;

    // Copies the camera matrix already supplied to this frame's native callbacks.
    void camera(Matrix *output) const noexcept;

    struct Bounds;

    // Moves visual bounds with the displayed objects, saving their native values for this render call.
    void begin_bounds(Bounds &bounds) const noexcept;

    // Restores the saved bounds for objects that still occupy the same native slots.
    void end_bounds(const Bounds &bounds) const noexcept;

    // Reports whether native callbacks are advancing simulation rather than drawing a frame.
    bool on_turn() const noexcept;

  private:
    struct Identity {
        NativeMapEntry *entry{};
        void *object{};
        std::int32_t unique_id{};
        std::uint32_t reset{};
        bool operator==(const Identity &) const = default;
    };

    struct ObjectSample {
        Identity identity;
        Pose pose{};
        Vector3 aim{};
        std::int32_t count{};
        bool valid{};
        bool aim_valid{};
    };

    struct Scene {
        std::int32_t serial{}; // Camera publication count minus one, separate from the network update turn.
        Pose camera{};
        Vector3 safe{};
        std::uint32_t mask{};
        std::array<ObjectSample, kObjectSlotCount> objects{};
    };

    struct DisplayObject {
        Identity identity;
        Matrix matrix{};
        Vector3 aim{};
        bool valid{};
        bool aim_valid{};
    };

    // Reads the slot's current object and checks that the object still refers to this slot.
    Identity identity(int index) const noexcept;

    // Fills the history with one completed sample so a new target appears immediately.
    void seed(const Scene &scene) noexcept;

    // Checks five rays around the camera's near plane and preserves the native last-ray result.
    bool resolve_camera(const Vector3 &safe, std::uint32_t mask, void *target, void *camera) noexcept;

    fc::Memory<const NativeObjects> objects_memory_;
    fc::Memory<const NativeMap> map_memory_;
    fc::Memory<void *const> collision_memory_;
    fc::Memory<const std::uint8_t> on_turn_memory_;
    fc::Memory<const float> seconds_memory_;
    const NativeObjects *objects_{};
    const NativeMap *map_{};
    void *const *collision_manager_{};
    const std::uint8_t *on_turn_{};
    const float *seconds_per_turn_{};
    fc::NativeFunction<SetMatrix> set_matrix_;
    fc::NativeFunction<SetRenderPos> set_render_pos_;
    fc::NativeFunction<SetRenderRadius> set_render_radius_;
    fc::NativeFunction<RayHit> ray_hit_;

    std::array<std::uint32_t, kObjectSlotCount> resets_{};
    std::array<Scene, 6> history_{};
    std::size_t size_{};
    void *interpolator_{};
    void *target_{};
    void *pending_target_{};
    Vector3 pending_safe_{};
    std::uint32_t pending_mask_{};
    Playback playback_;
    bool clock_seeded_{};
    bool displayed_{};
    Matrix camera_{};
    std::array<DisplayObject, kObjectSlotCount> display_{};

  public:
    // Per-render backups belong to one native invocation, including a possible nested draw.
    struct Bounds {
      private:
        friend class Presentation;

        struct Sphere {
            Identity identity;
            Vector3 center{};
            float radius{};
            bool changed{};
        };

        std::array<Sphere, kObjectSlotCount> sphere{};
    };
};

} // namespace fc_plugins::spectator_camera_smoothing
