#include "presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>

namespace fc_plugins::spectator_camera_smoothing {
namespace {

// Slot 63 is reserved by native map lookups and never identifies a displayed object.
bool valid_slot(int index) noexcept {
    return index >= 0 && index < kNullObjectSlot;
}

float distance(const Vector3 &a, const Vector3 &b) noexcept {
    float squared{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float delta = a[axis] - b[axis];
        squared += delta * delta;
    }
    return std::sqrt(squared);
}

} // namespace

void Presentation::plan(fc::Plan &plan, const Layout &layout) {
    objects_memory_ = plan.access(layout.objects);
    map_memory_ = plan.access(layout.object_map);
    collision_memory_ = plan.access(layout.collision_manager);
    on_turn_memory_ = plan.access(layout.on_turn);
    seconds_memory_ = plan.access(layout.seconds_per_turn);
    set_matrix_ = plan.function(layout.set_matrix);
    set_render_pos_ = plan.function(layout.set_render_pos);
    set_render_radius_ = plan.function(layout.set_render_radius);
    ray_hit_ = plan.function(layout.ray_hit);
}

fc::Result<> Presentation::prepare() {
    const auto bind = []<class T>(const fc::Memory<const T> &memory, const T *&pointer) -> fc::Result<> {
        const auto view = memory.view();
        if (!view) {
            return std::unexpected(view.error());
        }
        pointer = view->data();
        return {};
    };

    if (auto result = bind(objects_memory_, objects_); !result) {
        return result;
    }

    if (auto result = bind(map_memory_, map_); !result) {
        return result;
    }

    if (auto result = bind(collision_memory_, collision_manager_); !result) {
        return result;
    }

    if (auto result = bind(on_turn_memory_, on_turn_); !result) {
        return result;
    }

    return bind(seconds_memory_, seconds_per_turn_);
}

void Presentation::reset() noexcept {
    reset_camera();
    pending_target_ = nullptr;
}

void Presentation::reset_camera() noexcept {
    size_ = 0;
    interpolator_ = nullptr;
    target_ = nullptr;
    clock_seeded_ = false;
    displayed_ = false;
}

void Presentation::reset_slot(void *table, int index) noexcept {
    if (table != objects_ || index < 0 || index >= static_cast<int>(kObjectSlotCount)) {
        return;
    }

    ++resets_[index];
    display_[index].valid = false;
    if (valid_slot(index) && target_ != nullptr && (*map_)[index] != nullptr && (*map_)[index]->object == target_) {
        reset();
    }
}

void Presentation::safe_point(void *target, const Matrix *root, const Vector3 &local, std::uint32_t mask) noexcept {
    if (target == nullptr || root == nullptr) {
        return;
    }

    // TransformCoord uses the exact caller's row-vector working matrix, including any native root adjustment.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        pending_safe_[axis] = local[0] * (*root)[axis] + local[1] * (*root)[4 + axis] + local[2] * (*root)[8 + axis] + (*root)[12 + axis];
    }
    pending_target_ = target;
    pending_mask_ = mask;
}

Presentation::Identity Presentation::identity(int index) const noexcept {
    if (!valid_slot(index)) {
        return {};
    }

    auto *entry = (*map_)[index];
    if (entry == nullptr || entry->object == nullptr || fc::read_field<int>(entry->object, kObjectIndexOffset) != index) {
        return {};
    }
    return {.entry = entry, .object = entry->object, .unique_id = entry->unique_id, .reset = resets_[index]};
}

bool Presentation::mapped(int index, void *object) const noexcept {
    return object != nullptr && identity(index).object == object;
}

void Presentation::seed(const Scene &scene) noexcept {
    // Repeated first poses make a new lifetime visible immediately while real completed samples fill the window.
    for (std::size_t index = 0; index < history_.size(); ++index) {
        history_[index] = scene;
        history_[index].serial = scene.serial - static_cast<std::int32_t>(history_.size() - 1 - index);
    }
    size_ = history_.size();
    clock_seeded_ = false;
    displayed_ = false;
}

void Presentation::complete(void *interpolator, void *target) noexcept {
    const auto pending_target = pending_target_;
    pending_target_ = nullptr;
    if (interpolator == nullptr || target == nullptr) {
        reset();
        return;
    }

    const auto count = fc::read_field<std::int32_t>(interpolator, kCameraCountOffset);
    if (count <= 0) {
        reset();
        return;
    }
    const auto serial = count - 1;
    if (interpolator_ == interpolator && target_ == target && size_ != 0 && serial == history_[size_ - 1].serial) {
        return;
    }
    if (pending_target != target) {
        reset();
        return;
    }

    Scene scene{.serial = serial,
                .camera = pose_from_matrix(fc::read_field<Matrix>(interpolator, kCameraEndpointsOffset + sizeof(Matrix) * (serial & 1))),
                .safe = pending_safe_,
                .mask = pending_mask_};
    bool target_continuous = true;
    for (int index = 0; index < kNullObjectSlot; ++index) {
        auto &object = scene.objects[index];
        object.identity = identity(index);
        object.count = objects_->count[index];
        if (object.identity.object == nullptr || object.count <= 0) {
            continue;
        }
        const auto endpoint = (object.count - 1) & 1;
        object.pose = pose_from_matrix(objects_->matrix[index][endpoint]);
        object.aim = objects_->vector[index][endpoint];
        object.valid = true;
        object.aim_valid = object.count >= 2;

        // A changed/reset slot starts at its first new pose rather than interpolating from its former occupant.
        if (size_ != 0) {
            const auto &previous = history_[size_ - 1].objects[index];
            if (!previous.valid || previous.identity != object.identity || object.count < previous.count ||
                (!previous.aim_valid && object.aim_valid)) {
                if (object.identity.object == target) {
                    target_continuous = false;
                }
                for (std::size_t sample = 0; sample < size_; ++sample) {
                    history_[sample].objects[index] = object;
                }
            }
        }
    }

    const bool continuous =
        target_continuous && interpolator_ == interpolator && target_ == target && size_ != 0 && serial == history_[size_ - 1].serial + 1;
    interpolator_ = interpolator;
    target_ = target;
    if (!continuous) {
        seed(scene);
        return;
    }
    if (size_ == history_.size()) {
        std::move(history_.begin() + 1, history_.end(), history_.begin());
        --size_;
    }
    history_[size_++] = scene;
}

void Presentation::frame(void *interpolator, void *target, void *camera, int target_index, float delta, float ratio) noexcept {
    displayed_ = false;
    if (size_ == 0 || interpolator_ != interpolator || target_ != target || camera == nullptr || !valid_slot(target_index) ||
        !std::isfinite(delta) || delta < 0.0F || !std::isfinite(ratio) || !std::isfinite(*seconds_per_turn_) ||
        *seconds_per_turn_ <= 0.0F) {
        reset();
        return;
    }

    const auto &latest = history_[size_ - 1];
    const double source = static_cast<double>(latest.serial) + ratio;
    if (fc::read_field<std::int32_t>(interpolator, kCameraCountOffset) != latest.serial + 1 ||
        identity(target_index) != latest.objects[target_index].identity || !latest.objects[target_index].valid) {
        reset();
        return;
    }

    const double previous_phase = playback_.phase();
    const bool had_clock = clock_seeded_;
    double phase = source;
    if (!clock_seeded_) {
        playback_.reset(source);
        clock_seeded_ = true;
    } else {
        phase = playback_.advance(source, delta, *seconds_per_turn_);
    }

    // Four controls ending one sample behind the clock place steady motion two turns behind native interpolation.
    // The unused newest sample gives playback room to continue through small native timing reversals.
    const double interval = std::floor(phase) - 1.0;
    const double first = interval - 3.0;
    if (!std::isfinite(phase) || (had_clock && phase < previous_phase) || first < history_[0].serial || interval > latest.serial) {
        seed(latest);
        return;
    }

    const auto start = static_cast<std::size_t>(first - history_[0].serial);
    const float fraction = static_cast<float>(phase - std::floor(phase));
    std::array<Pose, 4> poses;
    std::array<Vector3, 4> safe;
    for (std::size_t control = 0; control < 4; ++control) {
        poses[control] = history_[start + control].camera;
        safe[control] = history_[start + control].safe;
    }
    camera_ = matrix_from_pose(smooth_poses(poses, fraction));
    const auto safe_origin = smooth_vectors(safe, fraction);
    // The central earlier sample owns the displayed interval's native normal/transition collision policy.
    if (!resolve_camera(safe_origin, history_[start + 1].mask, target, camera)) {
        seed(latest);
        return;
    }

    for (int index = 0; index < kNullObjectSlot; ++index) {
        auto &display = display_[index];
        display = {};
        const auto live = identity(index);
        std::array<Vector3, 4> aim;
        bool aim_valid = true;
        bool valid = live.object != nullptr;
        for (std::size_t control = 0; control < 4; ++control) {
            const auto &object = history_[start + control].objects[index];
            valid = valid && object.valid && object.identity == live;
            aim_valid = aim_valid && object.aim_valid;
            poses[control] = object.pose;
            aim[control] = object.aim;
        }
        if (!valid || (index == target_index && !aim_valid)) {
            if (index == target_index) {
                seed(latest);
                return;
            }
            continue;
        }
        display.identity = live;
        display.matrix = matrix_from_pose(smooth_poses(poses, fraction));
        display.aim = smooth_vectors(aim, fraction);
        display.valid = true;
        display.aim_valid = aim_valid;
    }
    if (!set_matrix_.call(camera, &camera_)) {
        seed(latest);
        return;
    }
    displayed_ = true;
}

bool Presentation::resolve_camera(const Vector3 &safe, std::uint32_t mask, void *target, void *camera) noexcept {
    const Vector3 desired{camera_[12], camera_[13], camera_[14]};
    const float length = distance(desired, safe);
    if (length * length <= 0.0001F) {
        return true;
    }

    const float near_plane = fc::read_field<float>(camera, kCameraNearPlaneOffset);
    Vector3 direction;
    Vector3 end;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        direction[axis] = (desired[axis] - safe[axis]) / length;
        end[axis] = desired[axis] - direction[axis] * near_plane;
    }
    auto *manager = *collision_manager_;
    const auto last_body = manager != nullptr ? fc::read_field<std::uint32_t>(manager, kLastRayBodyOffset) : 0;
    float fraction = 1.0F;
    void *ignored = target;
    for (int ray = 0; ray < 5; ++ray) {
        Vector3 start = safe;
        Vector3 finish = end;
        if (ray != 0) {
            const float right = (ray == 1 || ray == 2) ? 0.4F : -0.4F;
            const float up = (ray == 1 || ray == 3) ? 0.4F : -0.4F;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float offset = right * camera_[axis] + up * camera_[4 + axis];
                start[axis] += offset;
                finish[axis] += offset;
            }
        }
        void *hit{};
        const auto result = ray_hit_.call(&start, &finish, &hit, nullptr, &ignored, 1, mask, true);
        if (!result || !std::isfinite(*result)) {
            fraction = 0.0F;
            break;
        }
        fraction = std::min(fraction, *result);
    }
    if (manager != nullptr) {
        fc::write_field(manager, kLastRayBodyOffset, last_body);
    }
    if (fraction <= 0.0F) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        camera_[12 + axis] = safe[axis] + (end[axis] - safe[axis]) * fraction + direction[axis] * near_plane;
    }
    return true;
}

void Presentation::matrix(int index, Matrix *output) const noexcept {
    if (displayed_ && output != nullptr && valid_slot(index) && display_[index].valid && identity(index) == display_[index].identity) {
        *output = display_[index].matrix;
    }
}

void Presentation::vector(int index, Vector3 *output) const noexcept {
    if (displayed_ && output != nullptr && valid_slot(index) && display_[index].valid && display_[index].aim_valid &&
        identity(index) == display_[index].identity) {
        *output = display_[index].aim;
    }
}

void Presentation::camera(Matrix *output) const noexcept {
    if (displayed_ && output != nullptr) {
        *output = camera_;
    }
}

bool Presentation::on_turn() const noexcept {
    return *on_turn_ != 0;
}

void Presentation::begin_bounds(Bounds &bounds) const noexcept {
    if (!displayed_) {
        return;
    }
    for (int index = 0; index < kNullObjectSlot; ++index) {
        const auto &display = display_[index];
        if (!display.valid || identity(index) != display.identity) {
            continue;
        }
        auto &saved = bounds.sphere[index];
        saved.identity = display.identity;
        auto *scene = static_cast<std::byte *>(saved.identity.object) + kSceneObjectOffset;
        saved.center = fc::read_field<Vector3>(scene, kSceneSphereOffset);
        saved.radius = fc::read_field<float>(scene, kSceneRadiusOffset);
        const auto raw = fc::read_field<Vector3>(saved.identity.object, kObjectRootPositionOffset);
        const Vector3 position{display.matrix[12], display.matrix[13], display.matrix[14]};
        // Recenter about the displayed root and enlarge by the native root-to-sphere offset to contain rigid motion.
        saved.changed = true;
        static_cast<void>(set_render_pos_.call(scene, &position));
        static_cast<void>(set_render_radius_.call(scene, saved.radius + distance(saved.center, raw)));
    }
}

void Presentation::end_bounds(const Bounds &bounds) const noexcept {
    for (int index = 0; index < kNullObjectSlot; ++index) {
        const auto &saved = bounds.sphere[index];
        // A reset or map replacement during drawing retires the saved pointer before restoration dereferences it.
        if (!saved.changed || identity(index) != saved.identity) {
            continue;
        }
        auto *scene = static_cast<std::byte *>(saved.identity.object) + kSceneObjectOffset;
        static_cast<void>(set_render_pos_.call(scene, &saved.center));
        static_cast<void>(set_render_radius_.call(scene, saved.radius));
    }
}

} // namespace fc_plugins::spectator_camera_smoothing
