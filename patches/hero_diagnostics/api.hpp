#pragma once

#include <fc/patch_api.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace fc_plugins::hero_diagnostics {

// Each capture level includes the practical observations provided by the lower levels.
enum class CaptureMode : std::uint8_t {
    Standard = 1,
    Combat = 2,
    Full = 3,
};

enum class MeleeUpdateMode : std::uint8_t {
    Native,
    // The behavior patch skips eligible client catch-up updates on update-only passes.
    SkipRedundantAuthority,
};

enum class NetworkStateMode : std::uint8_t {
    Native,
    // The behavior patch omits a historical state request already represented by prediction.
    SuppressHistorical,
};

enum class HistoryOperation : std::uint8_t {
    Prediction,
    Authority,
    Clear,
    Replay,
    BeginPrediction,
    FinishPrediction,
    BeginAuthority,
    AuthorityApplied
};
enum class HistoryReason : std::uint8_t {
    None,
    Matched,
    Corrected,
    Untracked,
    NotReady,
    ExternalEntry,
    ClearInput,
    Destroy,
    Session,
    CallFailed
};

// Copies the existing history's frontiers; the observer never alters or reconstructs its path.
struct HistorySnapshot {
    std::int32_t base_turn{-1};
    std::int32_t last_turn{-1};
    std::int8_t base_state{-1};
    std::int8_t last_state{-1};
    std::uint8_t pending{};
    std::uint8_t reserved{};
    std::int32_t last_stale_idle_turn{-1};
    std::uint8_t stale_idle_samples{};
    std::uint8_t previous_buttons{};
    std::int8_t replay_state{-1};
    std::uint8_t prediction_window{};
};

// Authority result 0/1/2 classifies native, historical or corrected presentation; native weapon setters still run.
struct HistoryDecision {
    HistorySnapshot before;
    HistorySnapshot after;
    std::int32_t turn{-1};
    std::int32_t source{-1};
    std::int32_t target{-1};
    HistoryOperation operation{};
    HistoryReason reason{};
    std::uint8_t result{};
    std::uint8_t remote{};
    std::int32_t expected{-1};
    std::int32_t selected_turn{-1};
    std::uint8_t branch{};
    std::uint8_t selected_prefix{};
};

enum class PhysicsOperation : std::uint8_t {
    AerialCompensation,
    ExhaustedRoll,
    JumpCost,
    WeaponSelection,
    // Words: format, received turn, weapon, presence bits; applied values: mode/input/combo/notified ages.
    WeaponPhase
};

// Values and words retain operation-specific native operands copied synchronously by the behavior owner.
struct PhysicsDecision {
    float values[6]{};
    std::uint32_t words[4]{};
    PhysicsOperation operation{};
    std::uint8_t reason{};
    std::uint8_t result{};
    std::uint8_t reserved{};
};

// Owner decisions report the branch where it occurs; values and words follow the operation's documented field order.
enum class CauseKind : std::uint8_t {
    Controller = 1,
    Trigger,
    TriggerReset,
    SprintRoute,
    Controls,
    Action,
    InstalledAnimation,
    Replay,
    // Values[0..2] retain native mode age, actual turn duration and native mode.
    PresentationLookup,
    Audio,
    AnimationRequest,
    AnimationLane,
    ContactIsolation,
    NativeBypass,
    JumpReset,
    AutomaticTransition,
    MovementLock,
    LiveGait,
    JumpClock,
    AmbientAssignment,
    // Words[0..2] mark selected cutoff source, precise timing and a winning queued timestamp.
    InputTiming,
    ConditionTime
};

// Selection stages 1..6 report local request, channel authority, packed sync/authority changed HUD projection and inventory contradiction.
struct SelectionDecision {
    std::int32_t update_turn{-1};
    std::int32_t prediction_turn{-1};
    std::int32_t acknowledged_turn{-1};
    std::int32_t request_turn{-1};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
    std::uint32_t actual_weapon{};
    std::uint32_t target_weapon{};
    std::int8_t local_player{-1};
    std::int8_t old_channel{-1};
    std::int8_t channel{-1};
    std::int8_t old_slot{-1};
    std::int8_t target_slot{-1};
    std::uint8_t stage{};
    std::uint8_t verdict{};
    std::uint8_t invalidated{};
    std::uint8_t mute{};
};

// Describes the copied selection outcome; pointers remain x86 identities only.
inline fc::RecordDescription fc_description(std::type_identity<SelectionDecision>) {
    return fc::describe<SelectionDecision>(
        "SelectionDecision.v1", &SelectionDecision::update_turn, &SelectionDecision::prediction_turn, &SelectionDecision::acknowledged_turn,
        &SelectionDecision::request_turn, &SelectionDecision::epoch, &SelectionDecision::sequence, &SelectionDecision::actual_weapon,
        &SelectionDecision::target_weapon, &SelectionDecision::local_player, &SelectionDecision::old_channel, &SelectionDecision::channel,
        &SelectionDecision::old_slot, &SelectionDecision::target_slot, &SelectionDecision::stage, &SelectionDecision::verdict,
        &SelectionDecision::invalidated, &SelectionDecision::mute);
}

// Copies a synchronous owner decision; each cause defines the meaning of its values, words and stage.
struct OwnerDecision {
    float values[8]{};
    std::uint32_t words[12]{};
    std::uint32_t occurrence{};
    CauseKind kind{};
    std::uint8_t stage{};
    std::uint8_t result{};
    std::uint8_t reason{};
};

// Describes a synchronous owner branch with fixed copied operands.
inline fc::RecordDescription fc_description(std::type_identity<OwnerDecision>) {
    return fc::describe<OwnerDecision>("OwnerDecision.v1", &OwnerDecision::values, &OwnerDecision::words, &OwnerDecision::occurrence,
                                       &OwnerDecision::kind, &OwnerDecision::stage, &OwnerDecision::result, &OwnerDecision::reason);
}

// Zero subject means no currently observed Hero call on this OS thread.
struct HeroScope {
    std::uint32_t subject{};
    std::uint32_t generation{};
    std::uint32_t weapon{};
    std::uint32_t read_scope{};
    std::uint32_t operation_scope{};
    std::int32_t prediction_turn{-1};
    std::int32_t update_turn{-1};
    std::int32_t acknowledged_turn{-1};
    std::uint8_t phase{};
    std::int32_t host_turn{-1};
};

// Describes the fixed native decision payload borrowed by an optional producer.
inline fc::RecordDescription fc_description(std::type_identity<HistorySnapshot>) {
    return fc::describe<HistorySnapshot>("HistorySnapshot.v1", &HistorySnapshot::base_turn, &HistorySnapshot::last_turn,
                                         &HistorySnapshot::base_state, &HistorySnapshot::last_state, &HistorySnapshot::pending,
                                         &HistorySnapshot::reserved, &HistorySnapshot::last_stale_idle_turn,
                                         &HistorySnapshot::stale_idle_samples, &HistorySnapshot::previous_buttons,
                                         &HistorySnapshot::replay_state, &HistorySnapshot::prediction_window);
}

// Describes the fixed native decision payload borrowed by an optional producer.
inline fc::RecordDescription fc_description(std::type_identity<HistoryDecision>) {
    return fc::describe<HistoryDecision>(
        "HistoryDecision.v1", &HistoryDecision::before, &HistoryDecision::after, &HistoryDecision::turn, &HistoryDecision::source,
        &HistoryDecision::target, &HistoryDecision::operation, &HistoryDecision::reason, &HistoryDecision::result, &HistoryDecision::remote,
        &HistoryDecision::expected, &HistoryDecision::selected_turn, &HistoryDecision::branch, &HistoryDecision::selected_prefix);
}

// Describes the fixed native decision payload borrowed by an optional producer.
inline fc::RecordDescription fc_description(std::type_identity<PhysicsDecision>) {
    return fc::describe<PhysicsDecision>("PhysicsDecision.v1", &PhysicsDecision::values, &PhysicsDecision::words,
                                         &PhysicsDecision::operation, &PhysicsDecision::reason, &PhysicsDecision::result,
                                         &PhysicsDecision::reserved);
}

// Describes the scope copied across the optional patch API.
inline fc::RecordDescription fc_description(std::type_identity<HeroScope>) {
    return fc::describe<HeroScope>("HeroScope.v1", &HeroScope::subject, &HeroScope::generation, &HeroScope::weapon, &HeroScope::read_scope,
                                   &HeroScope::operation_scope, &HeroScope::prediction_turn, &HeroScope::update_turn,
                                   &HeroScope::acknowledged_turn, &HeroScope::phase, &HeroScope::host_turn);
}

// Reports decisions synchronously inside the corresponding observed call on the same OS thread; weapon pointers are borrowed.
// Copied tables remain valid while installed. Callers preserve ambient Win32 and Winsock errors around every method.
struct HeroDiagnosticsApi {
    static constexpr std::string_view id = "HeroDiagnosticsApi.v5";

    // Reports requested and effective update durations in seconds; weapon remains owned by the game.
    fc::Function<void(void *weapon, float delta, float effective_delta, MeleeUpdateMode mode) noexcept> update_decision;

    // Reports the requested combo ID, setter flag (zero or one), and apply/suppress decision.
    // Updates only the current topmost call when its weapon matches.
    fc::Function<void(void *weapon, std::int32_t state, std::uint32_t flag, NetworkStateMode mode) noexcept> network_state_decision;

    // Reports the actual existing history classification and its observed before/after frontiers.
    fc::Function<void(void *weapon, const HistoryDecision *decision) noexcept> history_decision;

    // Reports a branch from its owner, identifying either a weapon or a complete soldier object.
    fc::Function<void(void *object, std::uint8_t soldier, const PhysicsDecision *decision) noexcept> physics_decision;

    // Copies the active call's existing identity and turn context without binding native objects.
    fc::Function<HeroScope() noexcept> current_scope;

    // Looks up an already-bound weapon on its native owner thread; unknown pointers and other threads return empty.
    // Active invocation context is copied only when its identity matches; the input pointer is never dereferenced or bound.
    fc::Function<HeroScope(void *weapon) noexcept> weapon_scope;

    // Reports a computed selection outcome for a borrowed complete Soldier, including ordinary infantry.
    fc::Function<void(void *soldier, const SelectionDecision *decision) noexcept> selection_decision;

    // Reports an owner decision; unbound or other-thread weapons retain subject zero and explicit object identity.
    fc::Function<void(void *weapon, const OwnerDecision *decision) noexcept> owner_decision;

    // Returns the immutable capture level. Owners copy it when acquiring the API to avoid per-event policy calls.
    fc::Function<CaptureMode() noexcept> capture_mode;
};

// Describes all required synchronous methods for exact runtime compatibility checks.
inline fc::ApiContract<HeroDiagnosticsApi> api_contract() {
    return fc::api<HeroDiagnosticsApi>(HeroDiagnosticsApi::id, &HeroDiagnosticsApi::update_decision,
                                       &HeroDiagnosticsApi::network_state_decision, &HeroDiagnosticsApi::history_decision,
                                       &HeroDiagnosticsApi::physics_decision, &HeroDiagnosticsApi::current_scope,
                                       &HeroDiagnosticsApi::weapon_scope, &HeroDiagnosticsApi::selection_decision,
                                       &HeroDiagnosticsApi::owner_decision, &HeroDiagnosticsApi::capture_mode);
}

} // namespace fc_plugins::hero_diagnostics
