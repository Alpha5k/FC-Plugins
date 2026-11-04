#include "../patches/spectator_camera_smoothing/motion.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace motion = fc_plugins::spectator_camera_smoothing;

namespace {

// Makes a rotation around a unit axis for the full-pose motion scenarios.
motion::Quaternion rotation(float angle, motion::Vector3 axis = {0.0F, 1.0F, 0.0F}) {
    const float sine = std::sin(angle / 2.0F);
    return {axis[0] * sine, axis[1] * sine, axis[2] * sine, std::cos(angle / 2.0F)};
}

// Compares the transformed axes and position, so quaternion sign does not affect equality.
void check_pose(const motion::Pose &actual, const motion::Pose &expected, float tolerance = 0.00001F) {
    const auto actual_matrix = motion::matrix_from_pose(actual);
    const auto expected_matrix = motion::matrix_from_pose(expected);
    for (std::size_t element = 0; element < actual_matrix.size(); ++element) {
        CHECK(std::abs(actual_matrix[element] - expected_matrix[element]) < tolerance);
    }
}

} // namespace

TEST_CASE("Spectator motion keeps steady translation and turning two turns behind native interpolation") {
    motion::Playback playback;
    playback.reset(20.0);
    constexpr float seconds_per_turn = 1.0F / 30.0F;
    for (int frame = 1; frame <= 24; ++frame) {
        const double source = 20.0 + static_cast<double>(frame) / 4.0;
        const double phase = playback.advance(source, seconds_per_turn / 4.0F, seconds_per_turn);
        const int latest = static_cast<int>(std::floor(phase)) - 1;
        const float fraction = static_cast<float>(phase - std::floor(phase));
        std::array<motion::Pose, 4> poses;
        for (std::size_t sample = 0; sample < poses.size(); ++sample) {
            const float turn = static_cast<float>(latest - 3 + static_cast<int>(sample));
            poses[sample] = {{2.0F * turn, -turn, 0.5F * turn}, rotation(0.1F * turn)};
        }

        // Native interpolation represents source - 1; the shared delayed curve represents source - 3.
        const float displayed_turn = static_cast<float>(source - 3.0);
        check_pose(motion::smooth_poses(poses, fraction),
                   {{2.0F * displayed_turn, -displayed_turn, 0.5F * displayed_turn}, rotation(0.1F * displayed_turn)});
        CHECK(std::abs(phase - source) < 0.0000001);
        CHECK(std::abs(playback.rate() - 1.0) < 0.0000001);
    }
}

TEST_CASE("Spectator motion retains the game's row-vector axes and translation") {
    const motion::Matrix native{0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 3.0F, 4.0F, 5.0F, 1.0F};
    const auto pose = motion::pose_from_matrix(native);
    CHECK(pose.position == motion::Vector3{3.0F, 4.0F, 5.0F});
    const auto result = motion::matrix_from_pose(pose);
    for (std::size_t element = 0; element < native.size(); ++element) {
        CHECK(std::abs(result[element] - native[element]) < 0.000001F);
    }

    // Large rotations exercise the component branches used when the matrix trace is negative.
    for (const motion::Vector3 axis :
         {motion::Vector3{1.0F, 0.0F, 0.0F}, motion::Vector3{0.0F, 1.0F, 0.0F}, motion::Vector3{0.0F, 0.0F, 1.0F}}) {
        const motion::Pose original{{3.0F, 4.0F, 5.0F}, rotation(3.0F, axis)};
        check_pose(motion::pose_from_matrix(motion::matrix_from_pose(original)), original);
    }
}

TEST_CASE("Spectator motion joins successive curves and ignores quaternion sign") {
    const std::array<motion::Pose, 5> poses{{{{0.0F, 2.0F, 0.0F}, rotation(0.1F)},
                                             {{1.0F, 0.0F, 2.0F}, rotation(0.5F, {1.0F, 0.0F, 0.0F})},
                                             {{4.0F, 1.0F, 3.0F}, rotation(1.2F, {0.0F, 0.0F, 1.0F})},
                                             {{2.0F, 3.0F, 5.0F}, rotation(1.6F)},
                                             {{6.0F, 4.0F, 2.0F}, rotation(2.0F, {1.0F, 0.0F, 0.0F})}}};
    const std::array<motion::Pose, 4> left{poses[0], poses[1], poses[2], poses[3]};
    const std::array<motion::Pose, 4> right{poses[1], poses[2], poses[3], poses[4]};
    check_pose(motion::smooth_poses(left, 1.0F), motion::smooth_poses(right, 0.0F));

    // Compare matrix derivatives across the join, including rotations around different axes.
    constexpr float step = 0.001F;
    const auto join = motion::matrix_from_pose(motion::smooth_poses(left, 1.0F));
    const auto before = motion::matrix_from_pose(motion::smooth_poses(left, 1.0F - step));
    const auto after = motion::matrix_from_pose(motion::smooth_poses(right, step));
    for (std::size_t element = 0; element < join.size(); ++element) {
        CHECK(std::abs((join[element] - before[element]) / step - (after[element] - join[element]) / step) < 0.02F);
    }

    auto equivalent = left;
    for (std::size_t sample : {0U, 2U, 3U}) {
        for (float &component : equivalent[sample].rotation) {
            component = -component;
        }
    }
    for (float fraction : {0.0F, 0.2F, 0.5F, 0.8F, 1.0F}) {
        check_pose(motion::smooth_poses(equivalent, fraction), motion::smooth_poses(left, fraction));
    }
}

TEST_CASE("Spectator motion spreads a position correction without overshooting its endpoints") {
    const std::array<motion::Vector3, 7> positions{{{0.0F, 0.0F, 0.0F},
                                                    {0.0F, 0.0F, 0.0F},
                                                    {0.0F, 0.0F, 0.0F},
                                                    {9.0F, 0.0F, 0.0F},
                                                    {9.0F, 0.0F, 0.0F},
                                                    {9.0F, 0.0F, 0.0F},
                                                    {9.0F, 0.0F, 0.0F}}};
    float previous = 0.0F;
    int intermediate_frames = 0;
    for (std::size_t interval = 0; interval < 4; ++interval) {
        const std::array<motion::Vector3, 4> vectors{positions[interval], positions[interval + 1], positions[interval + 2],
                                                     positions[interval + 3]};
        std::array<motion::Pose, 4> poses;
        for (std::size_t sample = 0; sample < poses.size(); ++sample) {
            poses[sample] = {vectors[sample], rotation(0.0F)};
        }
        for (int frame = 0; frame <= 10; ++frame) {
            const float fraction = static_cast<float>(frame) / 10.0F;
            const auto position = motion::smooth_poses(poses, fraction).position;
            CHECK(position == motion::smooth_vectors(vectors, fraction));
            CHECK(position[0] >= previous - 0.00001F);
            CHECK(position[0] <= 9.0F + 0.00001F);
            if (position[0] > 0.0F && position[0] < 9.0F) {
                ++intermediate_frames;
            }
            previous = position[0];
        }
    }
    CHECK(intermediate_frames > 20);
    CHECK(std::abs(previous - 9.0F) < 0.000001F);
}

TEST_CASE("Spectator playback keeps both captured source rewinds forward within six retained samples") {
    // These source phases and frame durations surround camera records 21000 and 22807 in the capture.
    const std::array<std::array<double, 10>, 2> sources{
        {{485.10967015475035, 485.3668130040169, 485.6239558458328, 485.8810986876488, 486.1382414996624, 486.4525271952152,
          486.70967000722885, 486.96681278944016, 486.2988479733467, 486.54769587516785},
         {120.79601949453354, 121.05316227301955, 121.39919397234917, 121.65633684396744, 121.91347962617874, 122.17062245309353,
          122.42776528000832, 122.29884797334671, 122.54769587516785, 122.79654377698898}}};
    constexpr float frame_delta = 0.008294930681586266F;
    constexpr float seconds_per_turn = 1.0F / 30.0F;
    for (std::size_t rewind = 0; rewind < sources.size(); ++rewind) {
        motion::Playback playback;
        playback.reset(sources[rewind][0]);
        double previous = playback.phase();
        for (std::size_t frame = 1; frame < sources[rewind].size(); ++frame) {
            const float delta = rewind == 0 && frame == 5   ? 0.010138249024748802F
                                : rewind == 1 && frame == 2 ? 0.01116231456398964F
                                                            : frame_delta;
            const double source = sources[rewind][frame];
            const double phase = playback.advance(source, delta, seconds_per_turn);
            CHECK(phase > previous);
            CHECK(playback.rate() > 0.0);

            const int newest = static_cast<int>(std::floor(source));
            const int latest = static_cast<int>(std::floor(phase)) - 1;
            CHECK(latest <= newest);
            CHECK(latest - 3 >= newest - 5);
            previous = phase;
        }
    }
}
