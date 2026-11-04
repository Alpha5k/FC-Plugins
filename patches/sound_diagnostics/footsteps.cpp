#include "handler.hpp"

#include <Windows.h>
#include <bit>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::sound_diagnostics {
namespace {

// Restores native thread error after passive observations.
struct ErrorGuard {
    DWORD error{GetLastError()};

    ~ErrorGuard() {
        SetLastError(error);
    }
};

// Copies borrowed x86 fields at qualified native boundaries; zero addresses produce an absent sample.
template <class T> T field(std::uint32_t address, std::ptrdiff_t offset = 0) noexcept {
    T value{};
    if (address) {
        std::memcpy(&value, reinterpret_cast<const std::byte *>(address) + offset, sizeof(value));
    }
    return value;
}

// Copies the actual scalar SSE operand without interpreting any higher lanes.
float scalar(const fc::CpuContext &cpu, std::size_t index) noexcept {
    return std::bit_cast<float>(static_cast<std::uint32_t>(cpu.xmm[index].low));
}

using HeightUpdate = void(__thiscall *)(void *, void *, const float *) noexcept;
// Native distance and sound selection consume an implicit XMM1 input; the bridge carries it explicitly.
using DistanceUpdate =
    fc::NativeCall<void(void *, float) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Xmm1>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using FootSound =
    fc::NativeCall<void(void *, float, std::uint32_t, const float *, const float *) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Xmm1, fc::abi::Stack<0>, fc::abi::Stack<4>, fc::abi::Stack<8>>,
                                fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

struct FootLayout {
    fc::FunctionLocation<HeightUpdate> height;
    fc::FunctionLocation<DistanceUpdate> distance;
    fc::FunctionLocation<FootSound> sound;
    std::array<fc::CodeLocation, 2> gates;
    std::array<fc::CodeLocation, 2> arms;
    std::array<fc::CodeLocation, 2> notifications;
    fc::CodeLocation writes;
    fc::CodeLocation speed;
    fc::CodeLocation choice;
    std::array<fc::CodeLocation, 4> plays;
};

// Encodes a qualified relative CALL, independent of the image's loaded base.
auto call_bytes(std::uint32_t site, std::uint32_t target) {
    const auto displacement = target - site - 5;
    return fc::exact_bytes({0xE8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8),
                            static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)});
}

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<FootLayout> select_layout(std::string_view profile, std::uint32_t game_play) {
    constexpr std::array kProfiles{std::string_view{"BF2.Steam.59EDE353"}, std::string_view{"BF2.GOG.59EDF52B"}};
    for (const auto row : kProfiles) {
        if (profile != row) {
            continue;
        }
        // Both builds share these instruction forms, operands and addresses; Play's target differs by profile.
        return FootLayout{
            .height = {.rva = 0xE8030,
                       .name = "soldier rendered foot heights",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
            .distance = {.rva = 0xE7D20,
                         .name = "soldier distance foot writer",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
            .sound = {.rva = 0xE7C00,
                      .name = "soldier native foot notification",
                      .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53})},
            .gates =
                {{{.rva = 0xE81B2, .name = "left foot height gate", .evidence = fc::exact_bytes({0x66, 0x89, 0x86, 0xD6, 0x09, 0, 0})},
                  {.rva = 0xE8372, .name = "right foot height gate", .evidence = fc::exact_bytes({0x0F, 0xB7, 0x86, 0xD6, 0x09, 0, 0})}}},
            .arms = {{{.rva = 0xE81D2, .name = "left foot arms", .evidence = fc::exact_bytes({0x66, 0x89, 0x86, 0xD6, 0x09, 0, 0})},
                      {.rva = 0xE8392, .name = "right foot arms", .evidence = fc::exact_bytes({0x66, 0x89, 0x86, 0xD6, 0x09, 0, 0})}}},
            .notifications = {{{.rva = 0xE8241, .name = "left foot notification", .evidence = call_bytes(0xE8241, 0xE7C00)},
                               {.rva = 0xE8401, .name = "right foot notification", .evidence = call_bytes(0xE8401, 0xE7C00)}}},
            // All native branches enter 8515 at its start; its single MOVSS displaces six bytes.
            .writes = {.rva = 0xE8515,
                       .name = "native final foot height writes",
                       .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x28})},
            .speed = {.rva = 0xE7C0A, .name = "native foot speed operands", .evidence = fc::exact_bytes({0x8B, 0x86, 0x50, 0x07, 0, 0})},
            .choice = {.rva = 0xE7C51,
                       .name = "native foot sound class result",
                       .evidence = fc::exact_bytes({0x8A, 0x8E, 0x42, 0x07, 0, 0})},
            .plays = {{{.rva = 0xE7C81, .name = "fast right foot Play", .evidence = call_bytes(0xE7C81, game_play)},
                       {.rva = 0xE7CAE, .name = "fast left foot Play", .evidence = call_bytes(0xE7CAE, game_play)},
                       {.rva = 0xE7CDD, .name = "slow right foot Play", .evidence = call_bytes(0xE7CDD, game_play)},
                       {.rva = 0xE7D06, .name = "slow left foot Play", .evidence = call_bytes(0xE7D06, game_play)}}}};
    }
    return std::nullopt;
}

} // namespace

thread_local FootInvocation *Handler::foot_{};
thread_local Handler::FootSoundInvocation *Handler::foot_sound_{};
thread_local std::array<FootHistory, 64> Handler::foot_histories_{};

FootHistory *Handler::foot_history(void *object) noexcept {
    if (!trace_.enabled() || !hero_ready_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    const auto soldier = reinterpret_cast<std::uint32_t>(object);
    const auto selected = static_cast<std::int8_t>(static_cast<std::uint8_t>(field<std::uint8_t>(soldier, 0x742) << 4)) >> 4;
    if (selected < 0) {
        return nullptr;
    }
    const auto weapon = field<std::uint32_t>(soldier, 0x720 + selected * 4);
    const auto scope = hero_.weapon_scope(reinterpret_cast<void *>(weapon));
    if (!scope.subject || scope.weapon != weapon) {
        return nullptr;
    }
    return select_foot_history(foot_histories_, field<std::int32_t>(soldier, 0x314), scope.subject, scope.generation, soldier, weapon);
}

std::uint8_t Handler::foot_maintenance_enabled() noexcept {
    return trace_.enabled() && hero_ready_.load(std::memory_order_acquire);
}

void Handler::foot_maintenance(void *soldier, const FootMaintenance *outcome) noexcept {
    if (outcome == nullptr || !foot_maintenance_enabled()) {
        return;
    }
    auto *history = foot_history(soldier);
    if (!history) {
        return;
    }
    const ErrorGuard guard;
    const auto scope = hero_.weapon_scope(reinterpret_cast<void *>(history->weapon));
    const bool changed =
        !history->maintenance_seen || history->maintenance_reason != outcome->reason || history->maintenance_support != outcome->support ||
        history->maintenance_flags != outcome->flags_after ||
        (outcome->reason == 3 && (!foot_baseline_matches(*history, {outcome->result_heights[0], outcome->result_heights[1]}) ||
                                  outcome->flags_before != outcome->flags_after));
    if (changed) {
        trace::FootMaintenanceRecord record{.header = header(soldier, trace::RecordKind::FootMaintenance),
                                            .subject = history->subject,
                                            .generation = history->generation,
                                            .weapon = history->weapon,
                                            .render_scope = scope.operation_scope,
                                            .read_scope = scope.read_scope,
                                            .host_turn = scope.host_turn,
                                            .prediction_turn = scope.prediction_turn,
                                            .update_turn = scope.update_turn,
                                            .acknowledged_turn = scope.acknowledged_turn,
                                            .pose = outcome->pose,
                                            .flags_before = outcome->flags_before,
                                            .flags_after = outcome->flags_after,
                                            .support = outcome->support,
                                            .reason = outcome->reason,
                                            .phase = scope.phase};
        std::memcpy(record.bones, outcome->bones, sizeof(record.bones));
        std::memcpy(record.old_heights, outcome->old_heights, sizeof(record.old_heights));
        std::memcpy(record.sampled_heights, outcome->sampled_heights, sizeof(record.sampled_heights));
        std::memcpy(record.result_heights, outcome->result_heights, sizeof(record.result_heights));
        std::memcpy(record.directions, outcome->directions, sizeof(record.directions));
        write(record);
    }
    history->maintenance_seen = true;
    history->maintenance_reason = outcome->reason;
    history->maintenance_support = outcome->support;
    history->maintenance_flags = outcome->flags_after;
    if (outcome->reason != 3) {
        return;
    }

    // The producer supplied the actual write; native observers may enter before or after this callback.
    if (foot_ && foot_->history == history && foot_->soldier == history->soldier) {
        std::memcpy(foot_->old_heights.data(), outcome->result_heights, sizeof(outcome->result_heights));
    }
    history->writer_scope = scope.operation_scope;
    std::memcpy(history->heights.data(), outcome->result_heights, sizeof(outcome->result_heights));
    history->flags = outcome->flags_after;
    history->writer_seen = true;
    history->writer_source = 2;
    history->writer_other_invocations = history->other_invocations;
    for (std::uint32_t foot{}; foot < 2; ++foot) {
        trace::FootGate gate{.pose = {.scope = scope.operation_scope, .pose = outcome->pose, .hash = foot == 0 ? 0x1AF77461U : 0x2F06FA86U},
                             .other_invocations = history->other_invocations,
                             .old_height = outcome->old_heights[foot],
                             .current_height = outcome->sampled_heights[foot],
                             .epsilon = 0.01F,
                             .descending_difference = outcome->old_heights[foot] - outcome->sampled_heights[foot],
                             .ascending_difference = outcome->sampled_heights[foot] - outcome->old_heights[foot],
                             .flags_before = outcome->flags_before,
                             .flags_after = outcome->flags_after,
                             .comparisons = outcome->directions[foot],
                             .source = 2};
        retain_foot_arm(*history, foot, gate, outcome->flags_after);
    }
}

void Handler::plan_footsteps(fc::Plan &plan, const Layout &layout) {
    const auto selected = select_layout(plan.profile(), layout.game_play.rva);
    if (!selected) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    plan.observe<FootInvocation>(selected->height,
                                 fc::before([this](void *soldier, void *pose, const float *, FootInvocation &invocation) noexcept {
                                     invocation.previous = foot_;
                                     foot_ = &invocation;
                                     invocation.history = foot_history(soldier);
                                     if (!invocation.history) {
                                         return;
                                     }
                                     const ErrorGuard guard;
                                     invocation.soldier = reinterpret_cast<std::uint32_t>(soldier);
                                     invocation.pose = reinterpret_cast<std::uint32_t>(pose);
                                     invocation.old_heights = field<std::array<float, 2>>(invocation.soldier, 0xA08);
                                 }),
                                 fc::after([this](void *soldier, void *, const float *, const FootInvocation &invocation) noexcept {
                                     if (invocation.history && invocation.writes_completed && foot_history(soldier) == invocation.history) {
                                         const ErrorGuard guard;
                                         auto &history = *invocation.history;
                                         history.writer_scope = invocation.gates[0].pose.scope;
                                         history.heights = field<std::array<float, 2>>(invocation.soldier, 0xA08);
                                         history.flags = field<std::uint16_t>(invocation.soldier, 0x9D6);
                                         history.writer_seen = true;
                                         history.writer_source = 1;
                                         history.writer_other_invocations = history.other_invocations;
                                     }
                                     foot_ = invocation.previous;
                                 }));
    plan.observe<FootInvocation>(selected->distance, fc::before([this](void *soldier, float, FootInvocation &invocation) noexcept {
                                     // Distance notifications must not consume a suspended height invocation's operands.
                                     invocation.previous = foot_;
                                     foot_ = &invocation;
                                     invocation.history = foot_history(soldier);
                                     invocation.soldier = reinterpret_cast<std::uint32_t>(soldier);
                                 }),
                                 fc::after([this](void *soldier, float, const FootInvocation &invocation) noexcept {
                                     if (invocation.history && foot_history(soldier) == invocation.history) {
                                         const ErrorGuard guard;
                                         ++invocation.history->other_invocations;
                                         invocation.history->other_scope =
                                             hero_.weapon_scope(reinterpret_cast<void *>(invocation.history->weapon)).operation_scope;
                                     }
                                     foot_ = invocation.previous;
                                 }));
    for (std::size_t foot{}; foot < 2; ++foot) {
        for (const auto &[site, operation] :
             {std::pair{selected->gates[foot], 1U}, std::pair{selected->arms[foot], 2U}, std::pair{selected->notifications[foot], 3U}}) {
            plan.observe_instruction(site, fc::before([this, foot, operation](const fc::CpuContext &cpu) noexcept {
                                         foot_instruction(cpu, static_cast<std::uint8_t>(operation), static_cast<std::uint32_t>(foot));
                                     }));
        }
    }
    plan.observe_instruction(selected->writes, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 foot_instruction(cpu, 4, 0);
                             }));
    plan.observe<FootSoundInvocation>(selected->sound,
                                      fc::before([this](const fc::NativeContext &native, void *soldier, float, std::uint32_t foot,
                                                        const float *, const float *, FootSoundInvocation &invocation) noexcept {
                                          invocation.previous = foot_sound_;
                                          invocation.previous_cause = scopes_.control_cause;
                                          foot_sound_ = &invocation;
                                          auto *history = foot_history(soldier);
                                          if (!history || foot > 1) {
                                              return;
                                          }
                                          const ErrorGuard guard;
                                          const auto scope = hero_.weapon_scope(reinterpret_cast<void *>(history->weapon));
                                          auto &record = invocation.record;
                                          record.header = header(soldier, trace::RecordKind::FootstepDecision);
                                          record.cause = next_control_cause_.fetch_add(1, std::memory_order_relaxed);
                                          record.caller = native.return_address();
                                          record.subject = history->subject;
                                          record.generation = history->generation;
                                          record.weapon = history->weapon;
                                          record.render_scope = scope.operation_scope;
                                          record.read_scope = scope.read_scope;
                                          record.host_turn = scope.host_turn;
                                          record.prediction_turn = scope.prediction_turn;
                                          record.update_turn = scope.update_turn;
                                          record.acknowledged_turn = scope.acknowledged_turn;
                                          record.phase = scope.phase;
                                          record.foot = static_cast<std::uint8_t>(foot);
                                          record.stage = 1;
                                          record.arm = history->arms[foot];
                                          record.writer_scope = history->writer_scope;
                                          std::memcpy(record.writer_heights, history->heights.data(), sizeof(record.writer_heights));
                                          record.writer_flags = history->flags;
                                          record.writer_seen = history->writer_seen;
                                          record.writer_source = history->writer_source;
                                          record.other_invocations = history->other_invocations;
                                          record.other_since_writer = history->other_invocations - history->writer_other_invocations;
                                          record.other_since_arm = history->other_invocations - record.arm.other_invocations;
                                          record.other_scope = history->other_scope;
                                          attach_foot_notification(foot_, history, reinterpret_cast<std::uint32_t>(soldier), foot, record);
                                          record.flags_after_notification = field<std::uint16_t>(history->soldier, 0x9D6);
                                          invocation.observed = true;
                                          scopes_.control_cause = record.cause;
                                          write(record);
                                      }),
                                      fc::after([this](void *soldier, float, std::uint32_t, const float *, const float *,
                                                       const FootSoundInvocation &invocation) noexcept {
                                          if (invocation.observed && trace_.enabled()) {
                                              const ErrorGuard guard;
                                              auto record = invocation.record;
                                              record.header = header(soldier, trace::RecordKind::FootstepDecision);
                                              record.flags_after_notification =
                                                  field<std::uint16_t>(reinterpret_cast<std::uint32_t>(soldier), 0x9D6);
                                              record.stage = 3;
                                              write(record);
                                          }
                                          foot_sound_ = invocation.previous;
                                          scopes_.control_cause = invocation.previous_cause;
                                      }));
    for (const auto &[site, operation] : {std::pair{selected->speed, 5U}, std::pair{selected->choice, 6U}}) {
        plan.observe_instruction(site, fc::before([this, operation](const fc::CpuContext &cpu) noexcept {
                                     foot_instruction(cpu, static_cast<std::uint8_t>(operation), 0);
                                 }));
    }
    for (std::size_t index{}; index < selected->plays.size(); ++index) {
        plan.observe_instruction(selected->plays[index], fc::before([this, index](const fc::CpuContext &cpu) noexcept {
                                     foot_instruction(cpu, 7, static_cast<std::uint32_t>(index));
                                 }));
    }
}

void Handler::foot_instruction(const fc::CpuContext &cpu, std::uint8_t operation, std::uint32_t foot) noexcept {
    if (!trace_.enabled()) {
        return;
    }
    if (operation >= 5) {
        if (!foot_sound_ || !foot_sound_->observed) {
            return;
        }
        const ErrorGuard guard;
        auto &record = foot_sound_->record;
        if (operation == 5) {
            if (cpu.esi != record.header.object) {
                return;
            }
            record.animator = field<std::uint32_t>(cpu.esi, 0x750);
            record.speed_class = field<std::int32_t>(record.animator, 0x1FF4);
            record.speed = scalar(cpu, 1);
            record.speed_threshold = field<float>(field<std::uint32_t>(cpu.esi, 0x440), 0x1024);
            record.speed_observed = 1;
        } else if (operation == 6) {
            if (cpu.esi != record.header.object) {
                return;
            }
            record.fast = static_cast<std::uint8_t>(cpu.ebx) != 0;
            record.sound_class = cpu.eax;
        } else {
            constexpr std::array<std::uint16_t, 4> branches{0x30, 0x28, 0x20, 0x18};
            record.branch = branches[foot];
            record.wrapper = cpu.ecx;
            auto emitted = record;
            emitted.header = header(reinterpret_cast<void *>(record.header.object), trace::RecordKind::FootstepDecision);
            emitted.stage = 2;
            write(emitted);
        }
        return;
    }
    if (!foot_ || !foot_->history || cpu.esi != foot_->soldier) {
        return;
    }
    const ErrorGuard guard;
    auto &invocation = *foot_;
    auto &gate = invocation.gates[foot];
    if (operation == 1) {
        auto &pose = gate.pose;
        const auto scope = hero_.weapon_scope(reinterpret_cast<void *>(invocation.history->weapon));
        pose.scope = scope.operation_scope;
        pose.animator = field<std::uint32_t>(cpu.esi, 0x750);
        pose.pose = invocation.pose;
        pose.hash = foot == 0 ? 0x1AF77461 : 0x2F06FA86;
        pose.upper_clip = field<std::uint32_t>(pose.animator, 0x15E4);
        pose.lower_clip = field<std::uint32_t>(pose.animator, 0x1F94);
        pose.phase[0] = field<float>(pose.animator, 0x1600);
        pose.phase[1] = field<float>(pose.animator, 0x1FB0);
        pose.blend[0] = field<float>(pose.animator, 0x1FE4);
        pose.blend[1] = field<float>(pose.animator, 0x1FE0);
        pose.soldier_state = field<std::int32_t>(cpu.esi, 0x744);
        pose.movement_flags = field<std::uint8_t>(cpu.esi, 0x471);
        pose.animation_flags = field<std::uint8_t>(pose.animator, 0x201B);
        pose.aim_flags = field<std::uint16_t>(pose.animator, 0x201A);
        // The gate also supplies the actual sampled height after the pose lookup.

        // Left uses XMM2/XMM3/XMM1; right uses XMM1/XMM3/XMM2 immediately before the native comparison.
        gate.observed = 1;
        gate.old_height = scalar(cpu, foot == 0 ? 2 : 1);
        gate.current_height = scalar(cpu, 3);
        gate.epsilon = scalar(cpu, foot == 0 ? 1 : 2);
        gate.descending_difference = scalar(cpu, 0);
        gate.ascending_difference = gate.current_height - gate.old_height;
        gate.property_object = field<std::uint32_t>(cpu.esi, 0x440);
        gate.threshold = field<float>(gate.property_object, 0x10F0);
        gate.flags_before = field<std::uint16_t>(cpu.esi, 0x9D6);
        gate.comparisons = foot_comparisons(gate.old_height, gate.current_height, gate.epsilon, gate.threshold);
    } else if (operation == 2) {
        auto armed = gate;
        armed.source = 1;
        armed.other_invocations = invocation.history->other_invocations;
        armed.flags_before = field<std::uint16_t>(cpu.esi, 0x9D6);
        retain_foot_arm(*invocation.history, foot, armed, static_cast<std::uint16_t>(cpu.eax));
    } else if (operation == 3) {
        // XMM0 is the actual property+10F0 operand that passed COMISS; native cleared the armed bit before this CALL.
        gate.threshold = scalar(cpu, 0);
        gate.flags_after = field<std::uint16_t>(cpu.esi, 0x9D6);
        gate.comparisons = foot_comparisons(gate.old_height, gate.current_height, gate.epsilon, gate.threshold);
        gate.observed = 1;
        invocation.pending_foot = foot;
        invocation.pending_site = image_base_ + (foot == 0 ? 0xE8241 : 0xE8401);
    } else {
        invocation.writes_completed = true;
    }
}

} // namespace fc_plugins::sound_diagnostics
