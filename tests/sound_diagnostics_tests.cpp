#include "../patches/sound_diagnostics/aggregation.hpp"
#include "../patches/sound_diagnostics/footsteps.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

namespace sound = fc_plugins::sound_diagnostics::trace;

TEST_CASE("Sound ambient binding changes retain null properties and controller retirement") {
    const sound::AmbientBindingState live{.properties = 0x12345678,
                                          .controller_index = 196,
                                          .descriptor_type = 1,
                                          .controller_flags = 1,
                                          .state_flags = 4,
                                          .sound_masks = 0x10};
    auto after = live;
    CHECK_FALSE(sound::ambient_binding_changed(live, after));
    after.state_flags = 0x26;
    after.sound_masks = 0x20;
    CHECK_FALSE(sound::ambient_binding_changed(live, after));
    after.properties = 0;
    CHECK(sound::ambient_binding_changed(live, after));
    after = live;
    after.descriptor_type = 4;
    CHECK(sound::ambient_binding_changed(live, after));
    after = live;
    after.controller_index = 198;
    CHECK(sound::ambient_binding_changed(live, after));
    after = live;
    after.controller_flags = 0;
    CHECK(sound::ambient_binding_changed(live, after));
    after = {};
    CHECK_FALSE(sound::ambient_binding_changed({}, after));
    after.properties = live.properties;
    CHECK(sound::ambient_binding_changed({}, after));
}

TEST_CASE("Sound binding schema preserves saber prefixes and paired native identities") {
    CHECK(sound::kSchemaVersion == 11);
    CHECK(static_cast<std::uint16_t>(sound::RecordKind::UiQueued) == 44);
    CHECK(static_cast<std::uint16_t>(sound::RecordKind::AmbientBinding) == 45);
    CHECK(offsetof(sound::SaberDecisionRecord, ambient_wrapper) == 88);
    CHECK(sizeof(sound::SaberDecisionRecord) == 152);
    CHECK(sizeof(sound::AmbientBindingRecord) == 136);
    const sound::AmbientBindingRecord record{.header = {.kind = sound::RecordKind::AmbientBinding, .object = 0x1000},
                                             .caller = 0x6890F6,
                                             .controller = 0x10CC,
                                             .wrapper = 0x10D8,
                                             .read_scope = 73,
                                             .before = {.properties = 0x12345678, .controller_index = 196, .controller_flags = 1},
                                             .after = {.controller_index = 196, .controller_flags = 1},
                                             .boundary = sound::AmbientBoundary::Read};
    sound::AmbientBindingRecord decoded{};
    std::memcpy(&decoded, std::as_bytes(std::span{&record, 1}).data(), sizeof(decoded));
    CHECK(decoded.header.object == 0x1000);
    CHECK(decoded.caller == 0x6890F6);
    CHECK(decoded.controller == 0x10CC);
    CHECK(decoded.wrapper == 0x10D8);
    CHECK(decoded.read_scope == 73);
    CHECK(decoded.before.properties == 0x12345678);
    CHECK(decoded.after.properties == 0);
    CHECK(decoded.after.controller_flags == 1);
    CHECK(decoded.boundary == sound::AmbientBoundary::Read);
}

TEST_CASE("Sound gap closure distinguishes reused sources from native end and recovery") {
    const sound::PacketGapRecord gap{.generation = 41, .physical = 0x123400};
    CHECK(sound::packet_gap_end_stage(gap, 41, 0x123400, true) == 5);
    CHECK(sound::packet_gap_end_stage(gap, 42, 0x123400, true) == 4);
    CHECK(sound::packet_gap_end_stage(gap, 41, 0x123800, true) == 4);
    CHECK(sound::packet_gap_end_stage(gap, 41, 0x123400, false) == 2);
    CHECK(sound::packet_gap_end_stage(gap, 42, 0x123400, false) == 4);
    CHECK(sound::packet_gap_end_stage(gap, 0, 0x123400, true) == 0);
}

TEST_CASE("Sound assignment identifies empty fire properties separately from other property wrappers") {
    CHECK(sound::property_wrapper_offset(0x5000, 0x5120) == 0x120);
    CHECK(sound::property_wrapper_offset(0x5000, 0x5138) == 0x138);
    CHECK(sound::property_wrapper_offset(0x5000, 0x5154) == 0x154);
    CHECK(sound::property_wrapper_offset(0x5000, 0x5130) == 0x130);
    CHECK(sound::property_wrapper_offset(0x5000, 0x5121) == 0);
    CHECK(sound::property_wrapper_offset(0, 0x120) == 0);
    CHECK(static_cast<std::uint16_t>(sound::RecordKind::BindingMutation) == 46);
    CHECK(static_cast<std::uint16_t>(sound::RecordKind::NativeProducer) == 47);
    CHECK(sizeof(sound::BindingMutationRecord) == 168);
    CHECK(sizeof(sound::ProducerRecord) == 232);
    CHECK(offsetof(sound::PacketRecord, position) == 64);
    CHECK(sizeof(sound::PacketRecord) == 80);
    CHECK(offsetof(sound::SourceEndRecord, data) == 80);
    CHECK(sizeof(sound::SourceEndRecord) == 120);
}

TEST_CASE("Footstep lineage retains the first arming episode and detects changed height baselines") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    diagnostics::FootHistory history{.subject = 10, .generation = 1};
    sound::FootGate gate{.pose = {.scope = 750334, .hash = 0x1AF77461},
                         .other_invocations = 7,
                         .old_height = 0.05F,
                         .current_height = 0.02F,
                         .epsilon = 0.01F,
                         .threshold = 0.04F,
                         .flags_before = 0xA004};
    diagnostics::retain_foot_arm(history, 0, gate, 0xA005);
    CHECK(history.arms[0].observed == 1);
    CHECK(history.arms[0].pose.scope == 750334);
    CHECK(history.arms[0].flags_after == 0xA005);
    CHECK(history.arms[0].other_invocations == 7);
    gate.pose.scope = 750437;
    gate.flags_before = 0xA005;
    diagnostics::retain_foot_arm(history, 0, gate, 0xA005);
    CHECK(history.arms[0].pose.scope == 750334);
    CHECK(history.arms[1].observed == 0);
    gate.flags_before = 0xA001;
    diagnostics::retain_foot_arm(history, 1, gate, 0xA003);
    CHECK(history.arms[1].pose.scope == 750437);
    CHECK(diagnostics::foot_comparisons(0.02F, 0.04F, 0.01F, 0.03F) == 6);
    CHECK(diagnostics::foot_comparisons(0.04F, 0.02F, 0.01F, 0.03F) == 1);
    CHECK(diagnostics::foot_comparisons(0.02F, 0.03F, 0.01F, 0.02F) == 0);
    CHECK_FALSE(diagnostics::foot_baseline_matches(history, {0.02F, 0.04F}));
    history.heights = {0.02F, 0.04F};
    history.writer_seen = true;
    CHECK(diagnostics::foot_baseline_matches(history, {0.02F, 0.04F}));
    CHECK_FALSE(diagnostics::foot_baseline_matches(history, {0.02F, 0.05F}));
    history.heights = {0.0F, 0.04F};
    CHECK_FALSE(diagnostics::foot_baseline_matches(history, {-0.0F, 0.04F}));
    CHECK(static_cast<std::uint16_t>(sound::RecordKind::FootstepDecision) == 50);
}

TEST_CASE("Footstep histories retain subjects with colliding diagnostic IDs on different native players") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    std::array<diagnostics::FootHistory, 64> histories{};
    auto *first = diagnostics::select_foot_history(histories, 0, 1, 1, 0x1000, 0x2000);
    REQUIRE(first != nullptr);
    first->arms[0].pose.scope = 750334;
    auto *second = diagnostics::select_foot_history(histories, 1, 65, 1, 0x3000, 0x4000);
    REQUIRE(second != nullptr);
    second->arms[0].pose.scope = 750437;
    CHECK(first->arms[0].pose.scope == 750334);
    CHECK(second->arms[0].pose.scope == 750437);
    CHECK(diagnostics::select_foot_history(histories, 0, 1, 1, 0x1000, 0x2000) == first);
    CHECK(first->arms[0].pose.scope == 750334);
    CHECK(diagnostics::select_foot_history(histories, -1, 1, 1, 0x1000, 0x2000) == nullptr);
    CHECK(diagnostics::select_foot_history(histories, 64, 1, 1, 0x1000, 0x2000) == nullptr);
    CHECK(diagnostics::select_foot_history(histories, 0, 66, 2, 0x1000, 0x2000) == first);
    CHECK(first->arms[0].pose.scope == 0);
    CHECK(second->arms[0].pose.scope == 750437);
}

TEST_CASE("Footstep notifications copy current height operands once despite relocated callers") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    diagnostics::FootHistory history{.soldier = 0x1000, .heights = {0.02F, 0.04F}, .writer_seen = true};
    constexpr std::array sites{0x4E8241U, 0x4E8401U};
    constexpr std::array callers{0x31AB767U, 0x31ABA5BU};
    for (std::uint32_t foot = 0; foot < 2; ++foot) {
        diagnostics::FootInvocation invocation{.history = &history,
                                               .soldier = history.soldier,
                                               .old_heights = history.heights,
                                               .pending_site = sites[foot],
                                               .pending_foot = foot};
        invocation.gates[0].current_height = 0.03F;
        invocation.gates[1].current_height = 0.06F;
        auto &gate = invocation.gates[foot];
        gate.pose.scope = 750437;
        gate.old_height = history.heights[foot];
        gate.epsilon = 0.01F;
        gate.threshold = 0.17F;
        gate.flags_after = 0xA000;
        gate.observed = 1;
        sound::FootstepRecord record{.caller = callers[foot], .writer_scope = 750334, .writer_flags = 0xA001};
        record.arm.pose.scope = 750300;
        diagnostics::attach_foot_notification(&invocation, &history, history.soldier, foot, record);
        CHECK(record.caller == callers[foot]);
        CHECK(record.site == sites[foot]);
        CHECK(record.gate_observed == 1);
        CHECK(record.current.pose.scope == 750437);
        CHECK(record.current.old_height == history.heights[foot]);
        CHECK(record.current.current_height == gate.current_height);
        CHECK(record.current.epsilon == 0.01F);
        CHECK(record.current.threshold == 0.17F);
        CHECK(record.current.flags_after == 0xA000);
        CHECK(record.old_heights[0] == 0.02F);
        CHECK(record.old_heights[1] == 0.04F);
        CHECK(record.sampled_heights[0] == 0.03F);
        CHECK(record.sampled_heights[1] == 0.06F);
        CHECK(record.baseline_matches == 1);
        CHECK(record.arm.pose.scope == 750300);
        CHECK(record.writer_scope == 750334);
        CHECK(record.writer_flags == 0xA001);

        sound::FootstepRecord nested_sound{};
        diagnostics::attach_foot_notification(&invocation, &history, history.soldier, foot, nested_sound);
        CHECK(nested_sound.site == 0);
        CHECK(nested_sound.gate_observed == 0);
    }
    CHECK(sizeof(sound::FootstepRecord) == 336);
    CHECK(sound::kSchemaVersion == 11);
}

TEST_CASE("Footstep notifications require the pending invocation history soldier and foot") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    diagnostics::FootHistory history{.soldier = 0x1000};
    diagnostics::FootHistory other_history{.soldier = history.soldier};
    diagnostics::FootInvocation invocation{.history = &history, .soldier = history.soldier, .pending_site = 0x4E8241};
    invocation.gates[0].observed = 1;
    auto *active = &invocation;
    const auto *sound_history = &history;
    auto soldier = history.soldier;
    std::uint32_t foot{};
    SECTION("Another history cannot consume the notification") {
        sound_history = &other_history;
    }
    SECTION("Another soldier cannot consume the notification") {
        soldier = 0x2000;
    }
    SECTION("The other foot cannot consume the notification") {
        foot = 1;
    }
    SECTION("An absent producer cannot consume the notification") {
        active = nullptr;
    }
    sound::FootstepRecord unrelated{};
    diagnostics::attach_foot_notification(active, sound_history, soldier, foot, unrelated);
    CHECK(unrelated.site == 0);
    CHECK(unrelated.gate_observed == 0);
    sound::FootstepRecord matching{};
    diagnostics::attach_foot_notification(&invocation, &history, history.soldier, 0, matching);
    CHECK(matching.site == 0x4E8241);
    CHECK(matching.gate_observed == 1);
}

TEST_CASE("Nested foot height invocations retain their own notification operands") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    diagnostics::FootHistory history{.soldier = 0x1000};
    diagnostics::FootInvocation outer{.history = &history, .soldier = history.soldier, .pending_site = 0x4E8241};
    outer.gates[0].pose.scope = 10;
    outer.gates[0].current_height = 0.03F;
    outer.gates[0].observed = 1;
    auto *active = &outer;
    diagnostics::FootInvocation inner{
        .previous = active, .history = &history, .soldier = history.soldier, .pending_site = 0x4E8401, .pending_foot = 1};
    inner.gates[1].pose.scope = 11;
    inner.gates[1].current_height = 0.06F;
    inner.gates[1].observed = 1;
    active = &inner;
    sound::FootstepRecord inner_record{};
    diagnostics::attach_foot_notification(active, &history, history.soldier, 1, inner_record);
    CHECK(inner_record.site == 0x4E8401);
    CHECK(inner_record.current.pose.scope == 11);
    CHECK(inner_record.current.current_height == 0.06F);
    active = active->previous;
    sound::FootstepRecord outer_record{};
    diagnostics::attach_foot_notification(active, &history, history.soldier, 0, outer_record);
    CHECK(outer_record.site == 0x4E8241);
    CHECK(outer_record.current.pose.scope == 10);
    CHECK(outer_record.current.current_height == 0.03F);
}

TEST_CASE("Nested distance notifications cannot inherit a height invocation's operands") {
    namespace diagnostics = fc_plugins::sound_diagnostics;
    diagnostics::FootHistory history{.soldier = 0x1000};
    diagnostics::FootInvocation height{.history = &history, .soldier = history.soldier, .pending_site = 0x4E8241};
    height.gates[0].observed = 1;
    auto *active = &height;
    diagnostics::FootInvocation distance{.previous = active, .history = &history, .soldier = history.soldier};
    SECTION("A selected distance invocation shadows the enclosing height") {}
    SECTION("An unselected distance invocation shadows the enclosing height") {
        distance.history = nullptr;
    }
    active = &distance;
    sound::FootstepRecord distance_record{};
    diagnostics::attach_foot_notification(active, &history, history.soldier, 0, distance_record);
    CHECK(distance_record.site == 0);
    CHECK(distance_record.gate_observed == 0);
    active = active->previous;
    sound::FootstepRecord height_record{};
    diagnostics::attach_foot_notification(active, &history, history.soldier, 0, height_record);
    CHECK(height_record.site == 0x4E8241);
    CHECK(height_record.gate_observed == 1);
}

TEST_CASE("Repeated sound refusals preserve owner occurrence and native controller differences") {
    const sound::RequestRecord first{.header = {.kind = sound::RecordKind::Request, .source = sound::SourceKind::Game, .object = 0x1000},
                                     .request = 1,
                                     .caller = 0x2345,
                                     .descriptor = 0x4000,
                                     .subject = 1,
                                     .operation_scope = 8,
                                     .rejection = 1,
                                     .completed = 1};
    auto next = first;
    next.request = 2;
    next.entry_ticks = 100;
    next.end_ticks = 110;
    next.position[0] = 10.0F;
    CHECK(sound::same_refusal(first, next));
    next.operation_scope = 9;
    CHECK_FALSE(sound::same_refusal(first, next));
    next = first;
    next.ui_callback = 42;
    CHECK_FALSE(sound::same_refusal(first, next));
    next = first;
    next.starts = 1;
    CHECK_FALSE(sound::same_refusal(first, next));
    next = first;
    next.rejection = 2;
    CHECK_FALSE(sound::same_refusal(first, next));
    next = first;
    next.stop_argument = 1;
    CHECK_FALSE(sound::same_refusal(first, next));
}

TEST_CASE("Idle sound renderer runs retain device state failures and actual work") {
    const sound::RendererUpdateRecord first{.header = {.kind = sound::RecordKind::RendererUpdate, .object = 0x1000},
                                            .start_ticks = 100,
                                            .duration_ticks = 2,
                                            .position_observed = 1,
                                            .device = 0x2000,
                                            .input_node = 0x3000};
    auto next = first;
    next.start_ticks = 500;
    next.duration_ticks = 10;
    next.owner_iteration = 80;
    CHECK(sound::same_idle_renderer(first, next));
    next.position_result = -1;
    CHECK_FALSE(sound::same_idle_renderer(first, next));
    next = first;
    next.gain_current[0] = 1;
    CHECK_FALSE(sound::same_idle_renderer(first, next));
    next = first;
    next.raw_cursors_observed = 1;
    CHECK_FALSE(sound::same_idle_renderer(first, next));
    next = first;
    next.written_bytes = 8;
    CHECK_FALSE(sound::same_idle_renderer(first, next));
    next = first;
    next.position_after = 1;
    CHECK_FALSE(sound::same_idle_renderer(first, next));
}
