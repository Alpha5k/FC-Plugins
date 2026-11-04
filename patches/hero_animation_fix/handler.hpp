#pragma once

#include "api.hpp"
#include "history.hpp"
#include "jump_history.hpp"
#include "layout.hpp"
#include "presentation_history.hpp"
#include "replay_history.hpp"
#include "sound_state.hpp"
#include "../hero_diagnostics/api.hpp"

#include <fc/game.hpp>
#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

namespace fc_plugins::hero_animation_fix {

// Reconciles presentation while native weapon authority, simulation and state entries execute normally.
class Handler final {
  public:
    // Reserves qualified gameplay observers and presentation hooks for this exact profile.
    void plan(fc::Plan &plan);
    // Binds native counters and optional passive diagnostics before callbacks run.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    using DiagnosticsApi = hero_diagnostics::HeroDiagnosticsApi;

    struct UpdateWindow {
        void *weapon{};
        UpdateWindow *previous{};
        HeroIdentity identity{};
        std::optional<PresentationEntry> entry;
        std::uint32_t audio_order{};
        bool frontier{};
        // This remains active through ClearInput even when presentation identity is retired.
        bool historical{};
    };

    struct ReadWindow {
        void *weapon{};
        ReadWindow *previous{};
        HeroIdentity identity{};
    };

    struct AuthorityWindow {
        void *weapon{};
        AuthorityWindow *previous{};
        HeroIdentity identity{};
        std::optional<PresentationEntry> entry;
        std::uint32_t audio_order{};
    };

    struct SimulationWindow {
        SimulationWindow *previous{};
        void *controllable{};
        std::int32_t turn{-1};
    };

    struct JumpInvocation {
        HeroIdentity identity;
        std::int32_t turn{-1};
        std::uint32_t order{};
        std::int32_t action{};
        float timer{};
        bool historical{};
    };

    struct GaitReadWindow {
        GaitReadWindow *previous{};
        void *soldier{};
    };

    struct GaitWindow {
        GaitWindow *previous{};
        HeroIdentity identity;
        LiveGaitState live;
        float delta{};
        bool reading{};
    };

    struct AnimationWindow {
        AnimationWindow *previous{};
        void *animator{};
        std::int32_t action{};
        std::uint8_t reset_lanes{3};
    };

    // Observes native gait, accepted jump results and completed upper/lower animation selection.
    void plan_jump_presentation(fc::Plan &plan, const Layout &layout);
    // Captures native start boundaries before controller and scheduled work can mutate melee values.
    void before_predict_turn() noexcept;
    // Saves the completed native turn after scheduled work and before the optional fallback update.
    void after_predict_turn() noexcept;
    // Captures compatible authority only after the packet owner restores selection, native phase and exact ages.
    void received(void *soldier, std::int32_t turn) noexcept;
    // Returns the installed synchronous precise-queue provider, when available.
    const melee_timing::QueueTimingApi *queue_timing() const noexcept;
    // Reports the actual owner branch with copied operands.
    void owner_decision(void *weapon, const hero_diagnostics::OwnerDecision &decision) noexcept;
    // Records save/restore provenance and the existing queue at that exact decision.
    void replay_sample(void *weapon, const HeroIdentity &identity, std::int32_t turn, std::uint8_t stage, bool result) noexcept;
    // Copies the lock operands at each checkpoint restoration, without observing every simulation frame.
    void lock_decision(void *weapon, std::int32_t turn, std::uint8_t stage, bool restored) noexcept;
    // Copies baseline/classification immediately around an isolated native classifier call.
    void gait_decision(const GaitWindow &window, std::uint8_t stage, bool restored) noexcept;
    // Tests the independent synchronous historical simulation window, including nested native resets.
    bool historical_update(void *weapon) const noexcept;
    // Resolves the selected ready melee weapon from a complete borrowed soldier object.
    HeroIdentity soldier_identity(void *soldier) noexcept;
    // Publishes the optional diagnostics table once from the control thread.
    void connect_diagnostics(DiagnosticsApi diagnostics) noexcept;
    const DiagnosticsApi *diagnostics() const noexcept;
    // Records an entry before native mutation; external entries release presentation history.
    void observe_enter_state(void *weapon, std::int32_t state) noexcept;
    // Captures borrowed native identity and separately reports selected/alive readiness.
    HeroIdentity capture_identity(void *weapon, bool &ready) const noexcept;
    static void *selected_weapon(const HeroIdentity &identity) noexcept;
    // Uses FC session lifetime and native client-world flags without resetting history on host-world switches.
    bool network_prediction_active() noexcept;
    // Retires every presentation occurrence and invalidates active calls for this native lifetime.
    void clear_weapon(std::uintptr_t weapon,
                      hero_diagnostics::HistoryReason reason = hero_diagnostics::HistoryReason::ExternalEntry) noexcept;
    // Retires every session occurrence and invalidates active calls.
    void clear_tracking() noexcept;
    // Suppresses only a completed pure audio leaf associated with the exact current entry.
    bool suppress_audio(void *weapon, std::uint8_t kind, std::uint32_t &order, std::uint64_t variant = 0) noexcept;
    // Reports the actual audio lookup and native completion, including missing occurrence tokens.
    void audio_decision(void *weapon, std::uint8_t kind, std::uint32_t order, std::uint64_t variant, std::uint8_t stage,
                        bool result) noexcept;
    // Reports contact ownership and the summary computed by its existing translation traversal.
    void contact_decision(void *weapon, const ContactScope &contacts, std::uint8_t stage) noexcept;
    // Copies requested/effective animation arguments and owner tokens at retention or lane consumption.
    void animation_decision(const HeroIdentity &identity, const AnimationRequest &request, std::uint8_t stage, std::uint8_t lane = 0,
                            bool consumed = false) noexcept;
    // Records successful native audio completion within the current entry.
    void record_audio(void *weapon, std::uint8_t kind, std::uint32_t order, std::uint64_t variant = 0) noexcept;

    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::NativeFunction<UpdateFunction> update_;
    fc::NativeFunction<ReadFunction> read_;
    fc::NativeFunction<SetNetworkStateFunction> set_network_state_;
    fc::NativeFunction<AnimatorStateFunction> animator_state_;
    fc::NativeFunction<FireAudioFunction> fire_audio_;
    fc::NativeFunction<OrdnanceRenderFunction> ordnance_render_;
    std::array<fc::NativeFunction<ComboAudioFunction>, 2> combo_audio_;
    fc::Memory<const float> turn_duration_memory_;
    const float *turn_duration_{};
    fc::Memory<const std::int32_t> prediction_turn_memory_;
    const std::int32_t *prediction_turn_{};
    fc::Memory<const std::int32_t> acknowledged_turn_memory_;
    const std::int32_t *acknowledged_turn_{};
    fc::Memory<const std::int32_t> update_turn_memory_;
    const std::int32_t *update_turn_{};
    fc::Memory<const std::uint8_t> local_prediction_pass_memory_;
    const std::uint8_t *local_prediction_pass_{};
    fc::Memory<const std::uint8_t> network_enabled_memory_;
    const std::uint8_t *network_enabled_{};
    fc::Memory<const std::uint8_t> network_client_active_memory_;
    const std::uint8_t *network_client_active_{};
    fc::Memory<const std::byte> melee_vtable_memory_;
    const std::byte *melee_vtable_{};
    fc::Memory<const std::byte> collision_return_memory_;
    const std::byte *collision_return_{};
    fc::Memory<const std::byte> effects_timer_memory_;
    const std::byte *effects_timer_{};

    // Native callbacks are synchronous; stack-owned records preserve nested call identity.
    UpdateWindow *update_window_{};
    AuthorityWindow *authority_window_{};
    ReadWindow *read_window_{};
    ContactScope *contact_scope_{};
    SimulationWindow *simulation_window_{};
    AnimationWindow *animation_window_{};
    GaitReadWindow *gait_read_window_{};
    GaitWindow *gait_window_{};
    LocalHistory local_history_;
    RemoteHistory remote_history_;
    PresentationHistory presentation_history_;
    JumpHistory jump_history_;
    ReplayHistory replay_history_;
    std::int32_t observed_prediction_turn_{-1};
    bool fallback_pending_{};
    melee_timing::QueueTimingApi queue_timing_;
    std::atomic_bool queue_timing_ready_{};
    DiagnosticsApi diagnostics_{};
    hero_diagnostics::CaptureMode diagnostic_capture_{hero_diagnostics::CaptureMode::Standard};
    std::atomic_bool diagnostics_ready_{};
    fc::Game game_;
    fc::SessionId session_{};
};

} // namespace fc_plugins::hero_animation_fix
