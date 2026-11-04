#pragma once

#include <array>

namespace fc_plugins::spectator_camera_smoothing {

using Vector3 = std::array<float, 3>;
using Quaternion = std::array<float, 4>; // Components are x, y, z, w.
using Matrix = std::array<float, 16>;

// Retains the position and orientation of a native rigid transform.
struct Pose {
    Vector3 position;
    Quaternion rotation;
};

// Extracts a pose from a finite rigid matrix in the game's D3DX row-vector convention.
Pose pose_from_matrix(const Matrix &matrix) noexcept;

// Builds a D3DX row-vector matrix, with translation in elements 12 through 14.
Matrix matrix_from_pose(const Pose &pose) noexcept;

// Blends four consecutive poses with a cubic B-spline and shortest adjacent rotations.
// The caller supplies finite rigid poses and an interval fraction from zero through one.
Pose smooth_poses(const std::array<Pose, 4> &poses, float u) noexcept;

// Blends four consecutive native vectors with the same cubic weights as smooth_poses.
// The caller supplies an interval fraction from zero through one; the result is not normalized.
Vector3 smooth_vectors(const std::array<Vector3, 4> &vectors, float u) noexcept;

// Follows native source time in turns; the caller handles history bounds and discontinuities.
class Playback final {
  public:
    // Seeds presentation time from the source and starts playback at one turn per turn.
    void reset(double source) noexcept;

    // Advances the critically damped clock using positive seconds_per_turn and elapsed frame seconds.
    // Returns the presentation phase in turns; the caller reseeds if history cannot supply its interval.
    double advance(double source, float frame_delta, float seconds_per_turn) noexcept;

    double phase() const noexcept {
        return phase_;
    }

    double rate() const noexcept {
        return rate_;
    }

  private:
    double phase_{};
    double rate_{1.0};
};

} // namespace fc_plugins::spectator_camera_smoothing
