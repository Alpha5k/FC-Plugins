#include "motion.hpp"

#include <cmath>
#include <cstddef>

namespace fc_plugins::spectator_camera_smoothing {
namespace {

// Removes floating-point length drift from a nonzero quaternion.
Quaternion normalize(Quaternion rotation) noexcept {
    float length_squared = 0.0F;
    for (float component : rotation) {
        length_squared += component * component;
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    for (float &component : rotation) {
        component *= inverse_length;
    }
    return rotation;
}

// Composes rotations in the cumulative quaternion curve's order.
Quaternion multiply(const Quaternion &a, const Quaternion &b) noexcept {
    return {a[3] * b[0] + b[3] * a[0] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] + b[3] * a[1] + a[2] * b[0] - a[0] * b[2],
            a[3] * b[2] + b[3] * a[2] + a[0] * b[1] - a[1] * b[0], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// Returns the four nonnegative position weights for one cubic interval.
std::array<float, 4> weights(float u) noexcept {
    const float v = 1.0F - u;
    const float u2 = u * u;
    const float u3 = u2 * u;
    return {v * v * v / 6.0F, (3.0F * u3 - 6.0F * u2 + 4.0F) / 6.0F, (-3.0F * u3 + 3.0F * u2 + 3.0F * u + 1.0F) / 6.0F, u3 / 6.0F};
}

// Combines native vector components without normalizing the result.
Vector3 blend(const std::array<Vector3, 4> &vectors, const std::array<float, 4> &basis) noexcept {
    Vector3 result{};
    for (std::size_t sample = 0; sample < vectors.size(); ++sample) {
        for (std::size_t axis = 0; axis < result.size(); ++axis) {
            result[axis] += basis[sample] * vectors[sample][axis];
        }
    }
    return result;
}

// Applies a fraction of the shortest rotation between normalized adjacent controls.
Quaternion rotation_step(const Quaternion &from, const Quaternion &to, float weight) noexcept {
    const Quaternion inverse{-from[0], -from[1], -from[2], from[3]};
    Quaternion relative = normalize(multiply(inverse, to));
    if (relative[3] < 0.0F) {
        for (float &component : relative) {
            component = -component;
        }
    }

    // atan2 keeps small rotations usable without subtracting nearly equal cosine values.
    const float sine = std::sqrt(relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2]);
    if (sine == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    const float angle = weight * std::atan2(sine, relative[3]);
    const float scale = std::sin(angle) / sine;
    return {relative[0] * scale, relative[1] * scale, relative[2] * scale, std::cos(angle)};
}

} // namespace

Pose pose_from_matrix(const Matrix &matrix) noexcept {
    Quaternion rotation;
    const float trace = matrix[0] + matrix[5] + matrix[10];

    // Choose the largest quaternion component to avoid division by a small value near a half turn.
    // Off-diagonal differences follow D3DX's row-vector signs.
    if (trace > 0.0F) {
        const float scale = 2.0F * std::sqrt(1.0F + trace);
        rotation = {(matrix[6] - matrix[9]) / scale, (matrix[8] - matrix[2]) / scale, (matrix[1] - matrix[4]) / scale, scale / 4.0F};
    } else if (matrix[0] > matrix[5] && matrix[0] > matrix[10]) {
        const float scale = 2.0F * std::sqrt(1.0F + matrix[0] - matrix[5] - matrix[10]);
        rotation = {scale / 4.0F, (matrix[1] + matrix[4]) / scale, (matrix[2] + matrix[8]) / scale, (matrix[6] - matrix[9]) / scale};
    } else if (matrix[5] > matrix[10]) {
        const float scale = 2.0F * std::sqrt(1.0F + matrix[5] - matrix[0] - matrix[10]);
        rotation = {(matrix[1] + matrix[4]) / scale, scale / 4.0F, (matrix[6] + matrix[9]) / scale, (matrix[8] - matrix[2]) / scale};
    } else {
        const float scale = 2.0F * std::sqrt(1.0F + matrix[10] - matrix[0] - matrix[5]);
        rotation = {(matrix[2] + matrix[8]) / scale, (matrix[6] + matrix[9]) / scale, scale / 4.0F, (matrix[1] - matrix[4]) / scale};
    }
    return {{matrix[12], matrix[13], matrix[14]}, normalize(rotation)};
}

Matrix matrix_from_pose(const Pose &pose) noexcept {
    const Quaternion rotation = normalize(pose.rotation);
    const float x = rotation[0];
    const float y = rotation[1];
    const float z = rotation[2];
    const float w = rotation[3];
    return {1.0F - 2.0F * (y * y + z * z),
            2.0F * (x * y + z * w),
            2.0F * (x * z - y * w),
            0.0F,
            2.0F * (x * y - z * w),
            1.0F - 2.0F * (x * x + z * z),
            2.0F * (y * z + x * w),
            0.0F,
            2.0F * (x * z + y * w),
            2.0F * (y * z - x * w),
            1.0F - 2.0F * (x * x + y * y),
            0.0F,
            pose.position[0],
            pose.position[1],
            pose.position[2],
            1.0F};
}

Pose smooth_poses(const std::array<Pose, 4> &poses, float u) noexcept {
    const auto basis = weights(u);
    const std::array<Vector3, 4> positions{poses[0].position, poses[1].position, poses[2].position, poses[3].position};
    std::array<Quaternion, 4> rotations;
    for (std::size_t sample = 0; sample < poses.size(); ++sample) {
        rotations[sample] = normalize(poses[sample].rotation);
    }

    // Cumulative weights spread each adjacent rotation over the same interval as position.
    // Normalizing each composition prevents length drift across the three rotations.
    Quaternion rotation = rotations[0];
    const std::array<float, 3> cumulative{basis[1] + basis[2] + basis[3], basis[2] + basis[3], basis[3]};
    for (std::size_t sample = 1; sample < rotations.size(); ++sample) {
        rotation = normalize(multiply(rotation, rotation_step(rotations[sample - 1], rotations[sample], cumulative[sample - 1])));
    }
    return {blend(positions, basis), rotation};
}

Vector3 smooth_vectors(const std::array<Vector3, 4> &vectors, float u) noexcept {
    return blend(vectors, weights(u));
}

void Playback::reset(double source) noexcept {
    phase_ = source;
    rate_ = 1.0;
}

double Playback::advance(double source, float frame_delta, float seconds_per_turn) noexcept {
    const double h = static_cast<double>(frame_delta) / static_cast<double>(seconds_per_turn);
    const double error = phase_ - (source - h);
    const double velocity = rate_ - 1.0;
    const double temporary = (velocity + error) * h;
    const double decay = std::exp(-h);

    // This exact critically damped update assumes the source advances one turn per turn within the frame.
    // Small publication corrections change playback rate smoothly rather than copying their phase jump.
    phase_ = source + (error + temporary) * decay;
    rate_ = 1.0 + (velocity - temporary) * decay;
    return phase_;
}

} // namespace fc_plugins::spectator_camera_smoothing
