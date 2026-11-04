#include "../patches/hero_animation_fix/history.hpp"
#include "../patches/hero_animation_fix/jump_history.hpp"
#include "../patches/hero_animation_fix/layout.hpp"
#include "../patches/hero_animation_fix/presentation_history.hpp"
#include "../patches/hero_animation_fix/replay_history.hpp"
#include "../patches/hero_animation_fix/sound_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <cmath>

namespace hero_animation = fc_plugins::hero_animation_fix;

namespace {

// Native animators store a complete Soldier pointer at +0x50 and have separate storage.
struct NativeAnimator {
    std::array<std::byte, 0x50> fields{};
    void *soldier{};
};

static_assert(offsetof(NativeAnimator, soldier) == 0x50);

// Supplies native player identities for both local acknowledged history and remote public-state history.
hero_animation::HeroIdentity identity(bool remote = false) noexcept {
    return {.weapon = 1, .owner = 2, .combo = 3, .player_id = 4, .local_player = static_cast<std::uint8_t>(remote ? 0xFF : 0)};
}

// Supplies a combo presentation request with an explicit native restart.
hero_animation::AnimationRequest attack(std::uint8_t animation = 7) noexcept {
    return {.mode = 7, .restart = true, .primary = animation, .secondary = 0xA4, .full_body = true};
}

} // namespace

TEST_CASE("Historical movement lock restores its flags and tiny positive expiry timer before controls", "[plugins][hero_animation]") {
    std::array<std::byte, 0x210> weapon{};
    std::array<std::byte, 0xA00> soldier{};
    fc::write_field(weapon.data(), 0x6C, static_cast<void *>(soldier.data() + 0x240));
    float timer = 0.3F;
    for (int tick{}; tick < 6; ++tick) {
        timer -= 0.05F;
    }
    REQUIRE(timer > 0.0F);
    fc::write_field(soldier.data(), 0x474, timer);
    fc::write_field(soldier.data(), 0x471, std::uint8_t{0x89});
    const auto state = hero_animation::ReplayState::capture(weapon.data());
    fc::write_field(soldier.data(), 0x474, 0.0F);
    fc::write_field(soldier.data(), 0x471, std::uint8_t{0x43});
    state.restore(weapon.data());
    CHECK(fc::read_field<float>(soldier.data(), 0x474) == timer);
    CHECK(fc::read_field<std::uint8_t>(soldier.data(), 0x471) == 0x4B);
    // Soldier Read supplies its timer first; restoring the companion flags must keep that authoritative value.
    fc::write_field(soldier.data(), 0x474, 0.2F);
    fc::write_field(soldier.data(), 0x471, std::uint8_t{0x43});
    state.restore(weapon.data(), true);
    CHECK(fc::read_field<float>(soldier.data(), 0x474) == 0.2F);
    CHECK(fc::read_field<std::uint8_t>(soldier.data(), 0x471) == 0x4B);
}

TEST_CASE("Historical gait classification preserves live position, direction, angle and run hysteresis", "[plugins][hero_animation]") {
    std::array<std::byte, 0xA00> soldier{};
    fc::write_field(soldier.data(), 0x764, 1.0F);
    fc::write_field(soldier.data(), 0x768, 2.0F);
    fc::write_field(soldier.data(), 0x76C, 3.0F);
    fc::write_field(soldier.data(), 0x770, -0.7F);
    fc::write_field(soldier.data(), 0x9D5, std::uint8_t{0xA0});
    const auto live = hero_animation::LiveGaitState::capture(soldier.data());
    // The native historical classifier saves its replayed position and clears both gait bits at negligible displacement.
    for (const auto offset : {0x764, 0x768, 0x76C}) {
        fc::write_field(soldier.data(), offset, 0.0F);
    }
    fc::write_field(soldier.data(), 0x9D5, std::uint8_t{0x49});
    fc::write_field(soldier.data(), 0x770, 0.0F);
    live.restore(soldier.data());
    CHECK(fc::read_field<float>(soldier.data(), 0x764) == 1.0F);
    CHECK(fc::read_field<float>(soldier.data(), 0x768) == 2.0F);
    CHECK(fc::read_field<float>(soldier.data(), 0x76C) == 3.0F);
    CHECK(fc::read_field<float>(soldier.data(), 0x770) == -0.7F);
    CHECK(fc::read_field<std::uint8_t>(soldier.data(), 0x9D5) == 0x61);
    const float displacement = 0.2530868316F;
    CHECK(displacement > 6.5F * 0.05F * 0.71F);
    CHECK(displacement < 6.5F * 0.05F * 0.78F);
}

TEST_CASE("Historical stationary classification cannot replace Luke's backward walk with forward walk", "[plugins][hero_animation]") {
    std::array<std::byte, 0xA00> soldier{};
    fc::write_field(soldier.data(), 0x9D5, std::uint8_t{0x90});
    fc::write_field(soldier.data(), 0x770, 3.1415927F);
    const auto live = hero_animation::LiveGaitState::capture(soldier.data());
    // Captured turns 4546-4548: historical stationary classification writes 88 and angle zero.
    fc::write_field(soldier.data(), 0x9D5, std::uint8_t{0x88});
    fc::write_field(soldier.data(), 0x770, 0.0F);
    live.restore(soldier.data());
    CHECK(fc::read_field<std::uint8_t>(soldier.data(), 0x9D5) == 0x90);
    CHECK(fc::read_field<float>(soldier.data(), 0x770) == 3.1415927F);
}

TEST_CASE("An accepted displayed historical jump preserves only its unchanged native action clock", "[plugins][hero_animation]") {
    hero_animation::JumpHistory history;
    const auto player = identity();
    std::array<std::byte, 0x2100> animator{};
    fc::write_field(animator.data(), 0x1FEC, std::int32_t{2});
    REQUIRE_FALSE(history.already_presented(player, 360, 0, 2));
    REQUIRE(history.consume(player, 2, true) == 3);
    history.present(player, 1, 2, true, true);
    history.present(player, 2, 2, true, true);
    const bool presented = history.already_presented(player, 360, 0, 2);
    REQUIRE(presented);
    CHECK(hero_animation::preserve_jump_clock(animator.data(), 2, 0.0497703552F, true, true, presented));
    CHECK(fc::read_field<float>(animator.data(), 0x1FF0) == 0.0497703552F);
    for (const auto condition : {0, 1, 2, 3}) {
        CAPTURE(condition);
        fc::write_field(animator.data(), 0x1FF0, 0.0F);
        fc::write_field(animator.data(), 0x1FEC, std::int32_t{condition == 3 ? 3 : 2});
        CHECK_FALSE(hero_animation::preserve_jump_clock(animator.data(), 2, 0.0497703552F, condition != 0, condition != 1, condition != 2));
        CHECK(fc::read_field<float>(animator.data(), 0x1FF0) == 0.0F);
    }
}

TEST_CASE("Authority-only audio continues one occurrence but native reentry and ambiguous samples remain audible",
          "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    const auto player = identity();
    const auto first = history.authority_entry(player, 7752, 0, 8, 1, 0.05F, true, 0.05F);
    REQUIRE(first);
    history.authority(player, hero_animation::AuthorityAction::Corrected, first);
    CHECK_FALSE(history.snapshot(player).current);
    history.present_audio(player, *first, 2, 0, 155354888);
    const auto continuing = history.authority_entry(player, 7753, 0, 8, 1, 0.10F, true, 0.05F);
    REQUIRE(continuing == first);
    history.authority(player, hero_animation::AuthorityAction::Native, continuing);
    CHECK_FALSE(history.snapshot(player).current);
    CHECK(history.audio_presented(player, *continuing, 2, 0, 155354888));
    CHECK_FALSE(history.audio_presented(player, *continuing, 3, 0, 155354888));
    CHECK_FALSE(history.audio_presented(player, *continuing, 2, 0, 155354889));
    SECTION("Same-ID reentry resets the native mode clock") {
        const auto next = history.authority_entry(player, 7754, 0, 8, 1, 0.05F, true, 0.05F);
        REQUIRE(next);
        CHECK(next != first);
        CHECK_FALSE(history.audio_presented(player, *next, 2, 0, 155354888));
    }
    SECTION("Same-turn native reentry has a distinct authority ordinal after traversal reset") {
        history.rewind(player.weapon);
        const auto next = history.authority_entry(player, 7753, 0, 8, 1, 0.05F, true, 0.05F);
        REQUIRE(next);
        CHECK(next != first);
        CHECK_FALSE(history.audio_presented(player, *next, 2, 0, 155354888));
    }
    SECTION("Skipped authority turns cannot establish one entry") {
        const auto next = history.authority_entry(player, 7755, 0, 8, 1, 0.20F, true, 0.05F);
        REQUIRE(next);
        CHECK(next != first);
    }
    SECTION("Mode changes release the old occurrence") {
        const auto next = history.authority_entry(player, 7754, 0, 8, 5, 0.15F, true, 0.05F);
        REQUIRE(next);
        CHECK(next != first);
    }
    SECTION("An observed authority exit releases the old occurrence") {
        history.authority_entry(player, 7754, 8, 0, 0, 0.05F, true, 0.05F);
        const auto next = history.authority_entry(player, 7755, 0, 8, 1, 0.10F, true, 0.05F);
        REQUIRE(next);
        CHECK(next != first);
    }
    SECTION("Owner replacement retires completed audio") {
        auto replacement = player;
        ++replacement.owner;
        const auto next = history.authority_entry(replacement, 7754, 0, 8, 1, 0.15F, true, 0.05F);
        REQUIRE(next);
        CHECK_FALSE(history.audio_presented(replacement, *next, 2, 0, 155354888));
    }
}

TEST_CASE("Empty secondary FireAudio assignment preserves only the same active intended melee ambient", "[plugins][hero_animation]") {
    std::array<std::byte, 0x210> weapon{};
    std::array<std::byte, 0x300> properties{};
    fc::write_field(weapon.data(), 0x64, static_cast<void *>(properties.data()));
    fc::write_field(weapon.data(), 0xD8, std::uint32_t{155355072});
    fc::write_field(weapon.data(), 0xDC, std::uint8_t{1});
    fc::write_field(weapon.data(), 0xCC, std::uint16_t{10});
    fc::write_field(weapon.data(), 0xCE, std::uint8_t{3});
    fc::write_field(weapon.data(), 0x179, std::uint8_t{6});
    fc::write_field(weapon.data(), 0x17C, std::uint8_t{0x10});
    fc::write_field(properties.data(), 0x264, std::uint32_t{155355072});
    fc::write_field(properties.data(), 0x268, std::uint8_t{1});
    const auto ambient = hero_animation::AmbientAssignment::capture(weapon.data(), true);
    REQUIRE(ambient.qualified);
    CHECK_FALSE(hero_animation::AmbientAssignment::capture(weapon.data(), false).qualified);
    fc::write_field(weapon.data(), 0xD8, std::uint32_t{0});
    fc::write_field(weapon.data(), 0xDC, std::uint8_t{0});
    SECTION("The unchanged active controller keeps its descriptor after the empty native copy") {
        REQUIRE(ambient.restore(weapon.data()));
        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0xD8) == 155355072);
        CHECK(fc::read_field<std::uint8_t>(weapon.data(), 0xDC) == 1);
        CHECK(fc::read_field<std::uint16_t>(weapon.data(), 0xCC) == 10);
        CHECK(fc::read_field<std::uint8_t>(weapon.data(), 0xCE) == 3);
    }
    SECTION("A real controller stop remains native") {
        fc::write_field(weapon.data(), 0xCE, std::uint8_t{0});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
    SECTION("A replacement controller remains native") {
        fc::write_field(weapon.data(), 0xCC, std::uint16_t{11});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
    SECTION("A changed intended ambient remains native") {
        fc::write_field(properties.data(), 0x264, std::uint32_t{17});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
    SECTION("A real secondary source remains native") {
        fc::write_field(properties.data(), 0x120, std::uint32_t{17});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
    SECTION("A changed assigned descriptor remains native") {
        fc::write_field(weapon.data(), 0xD8, std::uint32_t{17});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
    SECTION("Saber off remains native") {
        fc::write_field(weapon.data(), 0x179, std::uint8_t{0});
        CHECK_FALSE(ambient.restore(weapon.data()));
    }
}

TEST_CASE("Melee checkpoint restores queue and movement values while preserving native ownership and live sound",
          "[plugins][hero_animation]") {
    std::array<std::byte, 0x210> weapon{};
    for (std::size_t offset = 0x118; offset < 0x1A8; ++offset) {
        weapon[offset] = static_cast<std::byte>(offset & 0xFFU);
    }
    fc::write_field(weapon.data(), 0xAC, std::uint32_t{0xA50101FA});
    fc::write_field(weapon.data(), 0xB0, std::int32_t{7});
    fc::write_field(weapon.data(), 0xB4, 0.15F);
    const auto state = hero_animation::ReplayState::capture(weapon.data());
    const auto values = weapon;
    for (auto &byte : weapon) {
        byte = std::byte{0xCC};
    }
    fc::write_field(weapon.data(), 0xAC, std::uint32_t{0x5A020007});
    fc::write_field(weapon.data(), 0x179, std::uint8_t{0x04});
    fc::write_field(weapon.data(), 0x17D, std::uint8_t{0xB0});
    state.restore(weapon.data());
    CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0xAC) == ((0x5A020007U & ~0x1F8U) | (0xA50101FAU & 0x1F8U)));
    CHECK(fc::read_field<std::int32_t>(weapon.data(), 0xB0) == 7);
    CHECK(fc::read_field<float>(weapon.data(), 0xB4) == 0.15F);
    for (std::size_t offset = 0x118; offset < 0x1A8; ++offset) {
        auto expected = values[offset];
        if (offset == 0x179) {
            expected = static_cast<std::byte>((std::to_integer<unsigned>(expected) & ~0x04U) | 0x04U);
        } else if (offset == 0x17D) {
            expected = static_cast<std::byte>((std::to_integer<unsigned>(expected) & 0x0FU) | 0xB0U);
        }
        CHECK(weapon[offset] == expected);
    }
    for (const std::size_t offset : {0x6CU, 0x110U, 0x114U, 0x1A8U, 0x1B0U, 0x200U}) {
        CHECK(weapon[offset] == std::byte{0xCC});
    }

    // Native Read may receive server sound flags; only live resource bookkeeping is retained afterward.
    const auto sound = hero_animation::LiveSoundState::capture(weapon.data());
    fc::write_field(weapon.data(), 0x179, std::uint8_t{0x12});
    fc::write_field(weapon.data(), 0x17D, std::uint8_t{0x23});
    sound.restore(weapon.data());
    CHECK(fc::read_field<std::uint8_t>(weapon.data(), 0x179) == 0x16);
    CHECK(fc::read_field<std::uint8_t>(weapon.data(), 0x17D) == 0xB3);
}

TEST_CASE("Consecutive authority reads retain exact melee seeds until sequential replay replaces the future", "[plugins][hero_animation]") {
    auto history = std::make_unique<hero_animation::ReplayHistory>();
    auto player = identity();
    SECTION("Local uses the native host turn independently of its input acknowledgement") {
        REQUIRE(player.is_local());
    }
    SECTION("Remote uses the same native host turn without a local acknowledgement") {
        player = identity(true);
    }
    for (const bool batched : {false, true}) {
        CAPTURE(batched);
        history->clear_all();
        std::array<std::byte, 0x210> weapon{};
        fc::write_field(weapon.data(), 0x180, std::int32_t{20});
        fc::write_field(weapon.data(), 0xB0, std::int32_t{7});
        history->capture(player, 68402, hero_animation::ReplayState::capture(weapon.data()));
        // This supplies observed native arithmetic; it is not a substitute implementation of native combat.
        const auto advance = [&]() {
            for (const std::size_t offset : {0xB4U, 0x130U, 0x188U}) {
                fc::write_field(weapon.data(), offset, fc::read_field<float>(weapon.data(), offset) + 0.05F);
            }
        };
        for (std::int32_t turn = 68402; turn <= 68405; ++turn) {
            REQUIRE_FALSE(history->needs_rewind(player, turn));
            history->capture(player, turn, hero_animation::ReplayState::capture(weapon.data()));
            advance();
            history->advanced(player, turn);
        }
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.20F) < 0.00001F);
        const auto start = history->restore(player, 68403);
        REQUIRE(start);
        start->restore(weapon.data());
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.05F) < 0.00001F);
        // The complete native reader supplies generic age; a same-ID combo retains its restored private clock.
        fc::write_field(weapon.data(), 0xB4, 0.10F);
        history->capture(player, 68403, hero_animation::ReplayState::capture(weapon.data()));
        if (batched) {
            // Another authority Read arrives before PredictTurn and still needs its original private clock.
            const auto next = history->restore(player, 68404);
            REQUIRE(next);
            next->restore(weapon.data());
            CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.10F) < 0.00001F);
            fc::write_field(weapon.data(), 0xB4, 0.35F);
            fc::write_field(weapon.data(), 0x180, std::int32_t{22});
            fc::write_field(weapon.data(), 0x188, 0.025F);
            fc::write_field(weapon.data(), 0x174, std::uint32_t{0x01000201});
            history->capture(player, 68404, hero_animation::ReplayState::capture(weapon.data()));

            // Prediction normally resumes at the latest read and advances its revised values.
            REQUIRE_FALSE(history->needs_rewind(player, 68404));
            history->capture(player, 68404, hero_animation::ReplayState::capture(weapon.data()));
            advance();
            history->advanced(player, 68404);
            CHECK(fc::read_field<std::int32_t>(weapon.data(), 0x180) == 22);
            CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x174) == 0x01000201);
            CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.075F) < 0.00001F);
            CHECK(std::abs(fc::read_field<float>(weapon.data(), 0xB4) - 0.40F) < 0.00001F);

            // Native prediction can restart before the latest read; the revised earlier seed owns that start.
            REQUIRE(history->needs_rewind(player, 68403));
            const auto earlier = history->restore(player, 68403);
            REQUIRE(earlier);
            earlier->restore(weapon.data());
            CHECK(std::abs(fc::read_field<float>(weapon.data(), 0xB4) - 0.10F) < 0.00001F);
            CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.05F) < 0.00001F);
        }
        for (std::int32_t turn = 68403; turn <= 68404; ++turn) {
            REQUIRE_FALSE(history->needs_rewind(player, turn));
            history->capture(player, turn, hero_animation::ReplayState::capture(weapon.data()));
            advance();
            history->advanced(player, turn);
        }
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.15F) < 0.00001F);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0xB4) - 0.20F) < 0.00001F);
        CHECK(fc::read_field<std::int32_t>(weapon.data(), 0x180) == 20);
        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x174) == 0);

        // A correction changes queue, combo and clocks together and owns the revised start boundary.
        const auto corrected_start = history->restore(player, 68404);
        REQUIRE(corrected_start);
        corrected_start->restore(weapon.data());
        CHECK(fc::read_field<std::int32_t>(weapon.data(), 0x180) == 20);
        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x174) == 0);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.10F) < 0.00001F);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0xB4) - 0.15F) < 0.00001F);
        fc::write_field(weapon.data(), 0x180, std::int32_t{22});
        fc::write_field(weapon.data(), 0x184, std::int32_t{20});
        fc::write_field(weapon.data(), 0x188, 0.025F);
        fc::write_field(weapon.data(), 0x130, 1.25F);
        fc::write_field(weapon.data(), 0x174, std::uint32_t{0x01000201});
        history->capture(player, 68404, hero_animation::ReplayState::capture(weapon.data()));
        REQUIRE_FALSE(history->needs_rewind(player, 68404));
        advance();
        history->advanced(player, 68404);
        REQUIRE_FALSE(history->needs_rewind(player, 68405));
        history->capture(player, 68405, hero_animation::ReplayState::capture(weapon.data()));
        advance();
        history->advanced(player, 68405);
        REQUIRE(history->needs_rewind(player, 68405));
        const auto revised = history->restore(player, 68405);
        REQUIRE(revised);
        revised->restore(weapon.data());
        CHECK(fc::read_field<std::int32_t>(weapon.data(), 0x180) == 22);
        CHECK(fc::read_field<std::int32_t>(weapon.data(), 0x184) == 20);
        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x174) == 0x01000201);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.075F) < 0.00001F);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x130) - 1.30F) < 0.00001F);
        history->capture(player, 68405, hero_animation::ReplayState::capture(weapon.data()));
        advance();
        history->advanced(player, 68405);
        CHECK(std::abs(fc::read_field<float>(weapon.data(), 0x188) - 0.125F) < 0.00001F);
    }
}

TEST_CASE("Completed native boundary survives consecutive reads with late notification and queue changes", "[plugins][hero_animation]") {
    for (const bool remote : {false, true}) {
        CAPTURE(remote);
        auto history = std::make_unique<hero_animation::ReplayHistory>();
        std::array<std::byte, 0x210> weapon{};
        auto player = identity(remote);
        player.weapon = reinterpret_cast<std::uintptr_t>(weapon.data());
        history->capture(player, 100, hero_animation::ReplayState::capture(weapon.data()));
        fc::write_field(weapon.data(), 0x188, 0.05F);
        fc::write_field(weapon.data(), 0x130, 7.8F);
        for (std::size_t row{}; row < 8; ++row) {
            fc::write_field(weapon.data(), 0x134 + row * 8, 7.8F + static_cast<float>(row));
            fc::write_field(weapon.data(), 0x138 + row * 8, static_cast<std::uint32_t>(row + 1));
        }
        fc::write_field(weapon.data(), 0x174, std::uint32_t{0x040103});
        history->advanced(player, 100);
        // Later native notification occurs after melee Update but before the full Predict return.
        fc::write_field(weapon.data(), 0x118, 0.125F);
        fc::write_field(weapon.data(), 0xAC, std::uint32_t{0x18});
        const auto completed = hero_animation::ReplayState::capture(weapon.data());
        REQUIRE(history->complete(player, 101, weapon.data()));
        const auto older = history->restore(player, 100);
        REQUIRE(older);
        older->restore(weapon.data());
        history->capture(player, 100, hero_animation::ReplayState::capture(weapon.data()));
        const auto newest = history->restore(player, 101);
        REQUIRE(newest);
        newest->restore(weapon.data());
        CHECK(hero_animation::ReplayState::capture(weapon.data()).melee == completed.melee);
        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0xAC) == 0x18);
        history->clear_weapon(player.weapon);
        CHECK_FALSE(history->complete(player, 101, weapon.data()));
    }
}

TEST_CASE("Replay requires exact turn tags and retires corrected suffixes and native lifetimes", "[plugins][hero_animation]") {
    auto history = std::make_unique<hero_animation::ReplayHistory>();
    auto player = identity();
    const hero_animation::ReplayState state{};
    for (std::int32_t turn = 100; turn <= 132; ++turn) {
        history->capture(player, turn, state);
    }
    CHECK_FALSE(history->restore(player, 100));
    CHECK_FALSE(history->restore(player, 132));
    history->capture(player, 200, state);
    history->capture(player, 201, state);
    REQUIRE(history->restore(player, 200));
    REQUIRE(history->restore(player, 201));
    REQUIRE(history->restore(player, 200));
    history->capture(player, 200, state);
    history->advanced(player, 200);
    CHECK_FALSE(history->restore(player, 201));
    history->capture(player, 300, state);
    SECTION("Native ClearInput or destruction retires old queue values") {
        history->clear_weapon(player.weapon);
    }
    SECTION("Owner replacement retires old queue values") {
        ++player.owner;
    }
    SECTION("A different local input slot retires the player identity") {
        player.local_player = 1;
    }
    SECTION("A new session retires old queue values") {
        history->clear_all();
    }
    CHECK_FALSE(history->restore(player, 300));
    auto remote = identity(true);
    remote.player_id = 8;
    remote.weapon = 9;
    history->capture(remote, 300, state);
    history->capture(player, 301, state);
    CHECK(history->restore(remote, 300).has_value());
    CHECK_FALSE(history->restore(remote, 301));
    history->capture(player, 400, state);
    history->capture(player, 450, state);
    CHECK_FALSE(history->restore(player, 400));
}

TEST_CASE("Detached native contacts keep victim membership and frontier age through nested replay", "[plugins][hero_animation]") {
    std::array<std::byte, 0x210> weapon{};
    std::array<std::byte, 0x38> first{};
    std::array<std::byte, 0x38> second{};
    fc::write_field(weapon.data(), 0x188, 0.25F);
    fc::write_field<void *>(weapon.data(), 0x1A8, first.data());
    fc::write_field(first.data(), 0, 0.05F);
    fc::write_field(first.data(), 4, std::uint32_t{55});
    fc::write_field(first.data(), 8, std::int32_t{1});
    fc::write_field(first.data(), 0xC, std::uint32_t{123});
    fc::write_field<void *>(first.data(), 0x2C, second.data());
    fc::write_field(second.data(), 0, 0.15F);
    fc::write_field<void *>(second.data(), 0x2C, nullptr);
    const auto membership = first;
    hero_animation::ContactScope outer;
    outer.begin(weapon.data(), nullptr);
    CHECK(fc::read_field<void *>(weapon.data(), 0x1A8) == nullptr);
    hero_animation::ContactScope inner;
    inner.begin(weapon.data(), &outer);
    auto history = std::make_unique<hero_animation::ReplayHistory>();
    const auto player = identity();
    history->capture(player, 100, {});
    history->clear_weapon(player.weapon);
    CHECK_FALSE(history->restore(player, 100));
    // ClearInput retires value history without disarming the independent live-contact owner.
    fc::write_field(weapon.data(), 0x188, 0.10F);
    inner.finish();
    CHECK(fc::read_field<void *>(weapon.data(), 0x1A8) == nullptr);
    outer.finish();
    REQUIRE(fc::read_field<void *>(weapon.data(), 0x1A8) == first.data());
    CHECK(std::abs((fc::read_field<float>(weapon.data(), 0x188) - fc::read_field<float>(first.data())) - 0.20F) < 0.00001F);
    CHECK(std::abs((fc::read_field<float>(weapon.data(), 0x188) - fc::read_field<float>(second.data())) - 0.10F) < 0.00001F);
    for (std::size_t offset = 4; offset < first.size(); ++offset) {
        CHECK(first[offset] == membership[offset]);
    }
    // Native frontier expiry still becomes eligible at age .3; the helper adds no retention or victim journal.
    fc::write_field(weapon.data(), 0x188, 0.21F);
    CHECK(fc::read_field<float>(weapon.data(), 0x188) >= fc::read_field<float>(first.data()) + 0.3F);
    fc::write_field<void *>(weapon.data(), 0x1A8, nullptr);
    // A native allocation for another use of the same attack remains a fresh record with fresh membership.
    fc::write_field(first.data(), 0, 0.21F);
    fc::write_field(first.data(), 8, std::int32_t{0});
    fc::write_field<void *>(weapon.data(), 0x1A8, first.data());
    CHECK(fc::read_field<std::uint32_t>(first.data(), 4) == 55);
    CHECK(fc::read_field<std::int32_t>(first.data(), 8) == 0);
}

TEST_CASE("Native destruction receives detached contacts once and later scopes cannot restore reused storage",
          "[plugins][hero_animation]") {
    std::array<std::byte, 0x210> weapon{};
    std::array<std::byte, 0x38> contact{};
    fc::write_field<void *>(weapon.data(), 0x1A8, contact.data());
    hero_animation::ContactScope outer;
    outer.begin(weapon.data(), nullptr);
    hero_animation::ContactScope inner;
    inner.begin(weapon.data(), &outer);
    inner.destroy(weapon.data());
    REQUIRE(fc::read_field<void *>(weapon.data(), 0x1A8) == contact.data());
    // Native destruction returns the exposed head to its pool and storage may then be reused.
    fc::write_field<void *>(weapon.data(), 0x1A8, nullptr);
    fc::write_field(weapon.data(), 0x188, 123.0F);
    inner.finish();
    outer.finish();
    CHECK(fc::read_field<void *>(weapon.data(), 0x1A8) == nullptr);
    CHECK(fc::read_field<float>(weapon.data(), 0x188) == 123.0F);
}

TEST_CASE("Animator callbacks resolve their separately allocated soldier", "[plugins][hero_animation]") {
    const auto soldier = std::make_unique<std::array<std::byte, 0x800>>();
    const auto replacement = std::make_unique<std::array<std::byte, 0x800>>();
    const auto animator = std::make_unique<NativeAnimator>();
    animator->soldier = soldier.get();
    CHECK(hero_animation::animator_soldier(animator.get()) == soldier.get());
    animator->soldier = replacement.get();
    CHECK(hero_animation::animator_soldier(animator.get()) == replacement.get());
}

TEST_CASE("Detached animators have no soldier identity", "[plugins][hero_animation]") {
    const auto animator = std::make_unique<NativeAnimator>();
    CHECK(hero_animation::animator_soldier(animator.get()) == nullptr);
}

TEST_CASE("Local authority confirms a presentation prefix and releases unmatched corrections", "[plugins][hero_animation]") {
    hero_animation::LocalHistory history;
    const auto player = identity();
    REQUIRE(history.observe_prediction(player, 0, 7, 100));
    REQUIRE(history.observe_prediction(player, 7, 8, 101));
    fc_plugins::hero_diagnostics::HistoryDecision evidence;
    CHECK(history.classify_authority(player, 8, 7, 100, &evidence) == hero_animation::AuthorityAction::Historical);
    CHECK(evidence.selected_turn == 100);
    CHECK(evidence.selected_prefix == 1);
    CHECK(history.classify_authority(player, 8, 16, 101) == hero_animation::AuthorityAction::Corrected);
    CHECK(history.snapshot(player).pending == 0);
}

TEST_CASE("Local history keeps distinct same-turn transitions and fails open at capacity", "[plugins][hero_animation]") {
    hero_animation::LocalHistory history;
    const auto player = identity();
    REQUIRE(history.observe_prediction(player, 0, 7, 100));
    REQUIRE(history.observe_prediction(player, 7, 0, 100));
    REQUIRE(history.observe_prediction(player, 0, 7, 100));
    CHECK(history.classify_authority(player, 7, 0, 100) == hero_animation::AuthorityAction::Corrected);
    history.clear_all();
    int state{};
    for (std::int32_t turn = 1; turn <= 32; ++turn) {
        const int next = state == 0 ? 7 : 0;
        REQUIRE(history.observe_prediction(player, state, next, turn));
        state = next;
    }
    CHECK_FALSE(history.observe_prediction(player, state, 7, 33));
    CHECK(history.classify_authority(player, 7, 0, 33) == hero_animation::AuthorityAction::Corrected);
}

TEST_CASE("Remote delayed idle has bounded tolerance and native completion ends the path", "[plugins][hero_animation]") {
    hero_animation::RemoteHistory history;
    const auto player = identity(true);
    REQUIRE(history.observe_prediction(player, 0, 7, 11));
    CHECK(history.classify_authority(player, 7, 0, 12) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(player, 7, 0, 12) == hero_animation::AuthorityAction::Historical);
    CHECK(history.classify_authority(player, 7, 0, 13) == hero_animation::AuthorityAction::Corrected);
    REQUIRE(history.observe_prediction(player, 0, 7, 14));
    REQUIRE(history.observe_prediction(player, 7, 0, 15));
    CHECK(history.classify_authority(player, 0, 0, 15) == hero_animation::AuthorityAction::Native);
    CHECK(history.snapshot(player).pending == 0);
}

TEST_CASE("Remote missing intermediate states cannot identify repeated combo occurrences", "[plugins][hero_animation]") {
    hero_animation::RemoteHistory history;
    const auto player = identity(true);
    REQUIRE(history.observe_prediction(player, 0, 7, 11));
    REQUIRE(history.observe_prediction(player, 7, 0, 12));
    REQUIRE(history.observe_prediction(player, 0, 7, 13));
    CHECK(history.classify_authority(player, 7, 7, 14) == hero_animation::AuthorityAction::Native);
    CHECK(history.snapshot(player).pending == 0);
    REQUIRE(history.observe_prediction(player, 7, 8, 15));
    CHECK(history.classify_authority(player, 8, 7, 16) == hero_animation::AuthorityAction::Corrected);
}

TEST_CASE("Authority confirmation and replay suppress only completed audio occurrences", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    auto player = identity();
    SECTION("Remote public-state history uses its own confirmation coordinate") {
        player = identity(true);
    }
    const hero_animation::PresentationEntry entry{.turn = 101, .order = 0, .source = 0, .target = 7};
    CHECK_FALSE(history.enter(player, entry, 110));
    CHECK_FALSE(history.audio_presented(player, entry, 2, 0, 123));
    history.present_audio(player, entry, 2, 0, 123);
    const auto confirmed = history.confirmed(player, 110, 7);
    REQUIRE(confirmed);
    CHECK(*confirmed == entry);
    CHECK_FALSE(history.confirmed(player, 101, 7));
    CHECK(history.audio_presented(player, *confirmed, 2, 0, 123));
    CHECK_FALSE(history.audio_presented(player, *confirmed, 2, 1, 123));
    CHECK_FALSE(history.audio_presented(player, *confirmed, 2, 0, 456));
    history.authority(player, hero_animation::AuthorityAction::Historical);
    CHECK(history.enter(player, entry, 110));
    CHECK(history.audio_presented(player, entry, 2, 0, 123));
    auto corrected = entry;
    corrected.target = 8;
    CHECK_FALSE(history.enter(player, corrected, 110));
    CHECK_FALSE(history.audio_presented(player, corrected, 2, 0, 123));
}

TEST_CASE("Installed and unchanged native lanes complete a pending combo request", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    const auto player = identity();
    const hero_animation::PresentationEntry entry{.turn = 100, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(history.enter(player, entry, 100));
    CHECK(history.animation(player, attack()).restart);
    std::uint8_t installed_lane = 1;
    SECTION("Upper installs and lower keeps its current clip") {}
    SECTION("Lower installs and upper keeps its current clip") {
        installed_lane = 2;
    }
    const auto unchanged_lane = static_cast<std::uint8_t>(3U ^ installed_lane);
    history.present_animation(player, unchanged_lane, false);
    history.authority(player, hero_animation::AuthorityAction::Historical);
    REQUIRE(history.enter(player, entry, 100));
    CHECK(history.animation(player, attack()).restart);
    history.present_animation(player, installed_lane, true);
    CHECK(history.animation(player, attack()).restart);
    // Only the qualified unchanged-clip return confirms this lane; an absent or failed selector cannot.
    history.present_animation(player, unchanged_lane, false);
    CHECK(history.animation(player, attack()).restart);
    history.present_animation(player, unchanged_lane, true);
    CHECK_FALSE(history.animation(player, attack()).restart);
}

TEST_CASE("Local older idle and exact confirmation preserve completed presentation", "[plugins][hero_animation]") {
    for (const std::uint8_t pending_lane : std::array<std::uint8_t, 3>{0, 1, 2}) {
        CAPTURE(pending_lane);
        hero_animation::LocalHistory history;
        hero_animation::PresentationHistory presentation;
        const auto player = identity();
        const hero_animation::PresentationEntry entry{.turn = 39240, .order = 0, .source = 0, .target = 20};
        const auto request = attack(20);
        REQUIRE(history.classify_authority(player, 0, 0, 39239) == hero_animation::AuthorityAction::Native);
        REQUIRE(history.observe_prediction(player, 0, 20, 39240));
        REQUIRE_FALSE(presentation.enter(player, entry, 39240));
        REQUIRE(presentation.animation(player, request) == request);
        for (const std::uint8_t lane : std::array<std::uint8_t, 2>{1, 2}) {
            presentation.present_animation(player, lane, lane != pending_lane);
        }

        fc_plugins::hero_diagnostics::HistoryDecision evidence;
        auto action = history.classify_authority(player, 20, 0, 39239, &evidence);
        REQUIRE(action == hero_animation::AuthorityAction::Historical);
        REQUIRE(evidence.selected_prefix == 0);
        presentation.authority(player, action);
        if (pending_lane == 0) {
            const auto retained = presentation.animation(player, {});
            CHECK(retained.primary == request.primary);
            CHECK_FALSE(retained.restart);
        }

        action = history.classify_authority(player, 20, 20, 39240, &evidence);
        REQUIRE(action == hero_animation::AuthorityAction::Native);
        REQUIRE(evidence.selected_prefix == 1);
        const auto confirmed = presentation.confirmed(player, evidence.selected_turn, 20);
        REQUIRE(confirmed == entry);
        presentation.authority(player, action, confirmed);
        // Native confirmation explicitly requests a restart even though this occurrence may already be displayed.
        CHECK(presentation.animation(player, request).restart == (pending_lane != 0));
        if (pending_lane != 0) {
            presentation.present_animation(player, pending_lane, true);
            CHECK_FALSE(presentation.animation(player, request).restart);
        }
    }
}

TEST_CASE("Remote exact confirmation preserves completed presentation and pending lanes", "[plugins][hero_animation]") {
    for (const std::uint8_t pending_lane : std::array<std::uint8_t, 3>{0, 1, 2}) {
        CAPTURE(pending_lane);
        hero_animation::RemoteHistory history;
        hero_animation::PresentationHistory presentation;
        const auto player = identity(true);
        const hero_animation::PresentationEntry entry{.turn = 39240, .order = 0, .source = 0, .target = 20};
        const auto request = attack(20);
        REQUIRE(history.observe_prediction(player, 0, 20, 39240));
        REQUIRE_FALSE(presentation.enter(player, entry, 39240));
        REQUIRE(presentation.animation(player, request) == request);
        for (const std::uint8_t lane : std::array<std::uint8_t, 2>{1, 2}) {
            presentation.present_animation(player, lane, lane != pending_lane);
        }

        fc_plugins::hero_diagnostics::HistoryDecision evidence;
        const auto action = history.classify_authority(player, 20, 20, 39241, &evidence);
        REQUIRE(action == hero_animation::AuthorityAction::Native);
        REQUIRE(evidence.selected_prefix == 1);
        const auto confirmed = presentation.confirmed(player, evidence.selected_turn, 20);
        REQUIRE(confirmed == entry);
        presentation.authority(player, action, confirmed);
        CHECK(presentation.animation(player, request).restart == (pending_lane != 0));
        if (pending_lane != 0) {
            presentation.present_animation(player, pending_lane, true);
            CHECK_FALSE(presentation.animation(player, request).restart);
        }
    }
}

TEST_CASE("Presentation lookup distinguishes missing unique and ambiguous native authority", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory presentation;
    const auto player = identity();
    std::uint32_t cardinality = 99;
    CHECK_FALSE(presentation.confirmed(player, 100, 7, &cardinality));
    CHECK(cardinality == 0);
    const hero_animation::PresentationEntry first{.turn = 100, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(presentation.enter(player, first, 100));
    CHECK(presentation.confirmed(player, 100, 7, &cardinality) == first);
    CHECK(cardinality == 1);
    REQUIRE_FALSE(presentation.enter(player, {.turn = 100, .order = 1, .source = 0, .target = 7}, 100));
    CHECK_FALSE(presentation.confirmed(player, 100, 7, &cardinality));
    CHECK(cardinality == 2);
}

TEST_CASE("Confirmed presentation releases corrections changed requests and new actions", "[plugins][hero_animation]") {
    hero_animation::LocalHistory history;
    hero_animation::PresentationHistory presentation;
    const auto player = identity();
    const hero_animation::PresentationEntry entry{.turn = 39240, .order = 0, .source = 0, .target = 20};
    const auto request = attack(20);
    REQUIRE(history.observe_prediction(player, 0, 20, 39240));
    REQUIRE_FALSE(presentation.enter(player, entry, 39240));
    REQUIRE(presentation.animation(player, request) == request);
    presentation.present_animation(player, 1, true);
    presentation.present_animation(player, 2, true);
    fc_plugins::hero_diagnostics::HistoryDecision evidence;
    const auto action = history.classify_authority(player, 20, 20, 39240, &evidence);
    REQUIRE(action == hero_animation::AuthorityAction::Native);
    const auto confirmed = presentation.confirmed(player, evidence.selected_turn, 20);
    REQUIRE(confirmed == entry);
    presentation.authority(player, action, confirmed);
    CHECK_FALSE(presentation.animation(player, request).restart);

    SECTION("Genuine authority correction releases the confirmed occurrence") {
        const auto correction = history.classify_authority(player, 20, 0, 39241);
        REQUIRE(correction == hero_animation::AuthorityAction::Corrected);
        presentation.authority(player, correction, confirmed);
        CHECK(presentation.animation(player, {}) == hero_animation::AnimationRequest{});
    }
    SECTION("Authority without a confirmed occurrence follows the native request") {
        presentation.authority(player, hero_animation::AuthorityAction::Native);
        CHECK(presentation.animation(player, request) == request);
    }
    SECTION("A changed native animation request remains eligible") {
        CHECK(presentation.animation(player, attack(21)) == attack(21));
    }
    SECTION("A new action using the same animation remains eligible") {
        REQUIRE(history.observe_prediction(player, 20, 21, 39241));
        REQUIRE_FALSE(presentation.enter(player, {.turn = 39241, .source = 20, .target = 21}, 39241));
        CHECK(presentation.animation(player, request) == request);
    }
}

TEST_CASE("Historical authority keeps a newer pending request eligible behind an installed animation", "[plugins][hero_animation]") {
    for (const bool remote : {false, true}) {
        for (const bool same_asset : {false, true}) {
            CAPTURE(remote, same_asset);
            hero_animation::PresentationHistory presentation;
            const auto player = identity(remote);
            const hero_animation::PresentationEntry first{.turn = 100, .order = 0, .source = 0, .target = 7};
            const hero_animation::PresentationEntry pending{.turn = 101, .order = 0, .source = 7, .target = 8};
            const auto request = attack(same_asset ? 7 : 8);
            REQUIRE_FALSE(presentation.enter(player, first, 100));
            presentation.animation(player, attack());
            presentation.present_animation(player, 1, true);
            presentation.present_animation(player, 2, true);
            REQUIRE_FALSE(presentation.enter(player, pending, 101));
            CHECK(presentation.animation(player, request) == request);

            hero_animation::AuthorityAction action;
            if (remote) {
                hero_animation::RemoteHistory history;
                REQUIRE(history.observe_prediction(player, 0, 7, 100));
                REQUIRE(history.observe_prediction(player, 7, 8, 101));
                action = history.classify_authority(player, 8, 7, 100);
            } else {
                hero_animation::LocalHistory history;
                REQUIRE(history.observe_prediction(player, 0, 7, 100));
                REQUIRE(history.observe_prediction(player, 7, 8, 101));
                action = history.classify_authority(player, 8, 7, 100);
            }
            REQUIRE(action == hero_animation::AuthorityAction::Historical);
            presentation.authority(player, action);
            REQUIRE(presentation.enter(player, pending, 101));
            CHECK(presentation.animation(player, request) == request);
            presentation.present_animation(player, 1, true);
            CHECK(presentation.animation(player, request) == request);
            presentation.present_animation(player, 2, true);
            const auto installed = presentation.animation(player, request);
            CHECK(installed.primary == request.primary);
            CHECK_FALSE(installed.restart);
        }
    }
}

TEST_CASE("Local and remote presentation retain confirmed prefixes and release new starts and ends", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    auto player = identity();
    SECTION("Remote player") {
        player = identity(true);
    }
    const hero_animation::PresentationEntry first{.turn = 100, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(history.enter(player, first, 100));
    CHECK(history.animation(player, attack()).restart);
    history.present_animation(player, 1, true);
    history.present_animation(player, 2, true);
    history.authority(player, hero_animation::AuthorityAction::Historical);
    const auto retained = history.animation(player, {});
    CHECK(retained.mode == 7);
    CHECK_FALSE(retained.restart);
    REQUIRE(history.enter(player, first, 100));
    CHECK_FALSE(history.animation(player, attack()).restart);
    const hero_animation::PresentationEntry end{.turn = 101, .order = 0, .source = 7, .target = 0};
    REQUIRE_FALSE(history.enter(player, end, 101));
    CHECK(history.animation(player, {}).mode == 0);
    const hero_animation::PresentationEntry second{.turn = 102, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(history.enter(player, second, 102));
    CHECK(history.animation(player, attack()).restart);
    history.authority(player, hero_animation::AuthorityAction::Corrected);
    CHECK(history.animation(player, {}).mode == 0);
}

TEST_CASE("Same-asset attacks and same-turn corrections remain distinct", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    const auto player = identity();
    const hero_animation::PresentationEntry first{.turn = 100, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(history.enter(player, first, 100));
    history.present_audio(player, first, 1, 0, 0);
    history.animation(player, attack());
    history.present_animation(player, 1, true);
    history.present_animation(player, 2, true);
    const hero_animation::PresentationEntry second{.turn = 100, .order = 1, .source = 7, .target = 7};
    REQUIRE_FALSE(history.enter(player, second, 100));
    CHECK_FALSE(history.audio_presented(player, second, 1, 0, 0));
    CHECK(history.animation(player, attack()).restart);
    CHECK_FALSE(history.confirmed(player, 100, 7));
    auto corrected = second;
    corrected.target = 8;
    REQUIRE_FALSE(history.enter(player, corrected, 100));
    CHECK(history.animation(player, attack()).restart);
}

TEST_CASE("Native lifetime replacement and history expiry restore native presentation", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    auto player = identity();
    const hero_animation::PresentationEntry entry{.turn = 100, .order = 0, .source = 0, .target = 7};
    REQUIRE_FALSE(history.enter(player, entry, 100));
    history.present_audio(player, entry, 1, 0, 0);
    CHECK(history.next_order(player, 100) == 0);
    CHECK(history.next_order(player, 100) == 1);
    history.rewind(player.weapon);
    CHECK(history.next_order(player, 100) == 0);
    SECTION("Destroy and reuse") {
        history.clear_weapon(player.weapon);
    }
    SECTION("Replacement owner") {
        ++player.owner;
    }
    SECTION("New session") {
        history.clear_all();
    }
    SECTION("Native input horizon") {
        REQUIRE_FALSE(history.enter(player, {.turn = 132, .source = 7, .target = 0}, 132));
    }
    CHECK_FALSE(history.audio_presented(player, entry, 1, 0, 0));
}

TEST_CASE("Jump replay removes resets at lane consumption and preserves pending selectors", "[plugins][hero_animation]") {
    hero_animation::JumpHistory history;
    const auto player = identity();
    REQUIRE_FALSE(history.already_presented(player, 100, 0, 1));
    CHECK(history.consume(player, 1, true) == 3);
    history.present(player, 1, 1, false);
    REQUIRE_FALSE(history.already_presented(player, 100, 0, 1));
    CHECK(history.consume(player, 1, true) == 3);
    history.present(player, 1, 1, true);
    REQUIRE_FALSE(history.already_presented(player, 100, 0, 1));
    CHECK(history.consume(player, 1, true) == 2);
    history.present(player, 2, 1, true);
    REQUIRE(history.already_presented(player, 100, 0, 1));
    CHECK(history.consume(player, 1, true) == 0);
    REQUIRE_FALSE(history.already_presented(player, 100, 1, 1));
    CHECK(history.consume(player, 1, true) == 3);
    REQUIRE_FALSE(history.already_presented(player, 100, 0, 2));
    CHECK(history.consume(player, 2, true) == 3);
    history.clear_weapon(player.weapon);
    CHECK_FALSE(history.already_presented(player, 100, 0, 1));
}

TEST_CASE("Authority occurrence matching uses the actual 60 TPS turn duration", "[plugins][hero_animation]") {
    hero_animation::PresentationHistory history;
    const auto player = identity(true);
    const float duration = 1.0F / 60.0F;
    const auto first = history.authority_entry(player, 71, 0, 10, 1, 0.4F - duration, true, duration);
    REQUIRE(first);
    const auto next = history.authority_entry(player, 72, 0, 10, 1, (0.4F - duration) + duration, true, duration);
    CHECK(next == first);
}

TEST_CASE("A remote preceding authority state preserves its future native occurrence", "[plugins][hero_animation]") {
    hero_animation::RemoteHistory history;
    const auto player = identity(true);
    REQUIRE(history.classify_authority(player, 12, 12, 106) == hero_animation::AuthorityAction::Native);
    REQUIRE(history.observe_prediction(player, 12, 13, 118));
    CHECK(history.classify_authority(player, 13, 12, 112) == hero_animation::AuthorityAction::Historical);
    CHECK(history.snapshot(player).pending == 1);
    CHECK(history.classify_authority(player, 13, 13, 118) == hero_animation::AuthorityAction::Native);
    CHECK(history.snapshot(player).pending == 0);
}
