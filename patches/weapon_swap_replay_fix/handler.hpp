#pragma once

#include "layout.hpp"
#include "packed_ledger.hpp"
#include "../hero_diagnostics/api.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fc_plugins::weapon_swap_replay_fix {

// Mutes repeated weapon change sounds and shows predicted weapons through HUD/render read results.
class Handler final {
  public:
    // Verifies the game layout and retains all selection, presentation and input hooks.
    void plan(fc::Plan &plan);

    // Binds planned native storage before callbacks can observe prediction state.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // Records a predicted channel swap and tells the following Select whether to mute its change sound.
    void handle_local_selection(void *adjusted_soldier, int channel, int old_index, int target_index) noexcept;

    // Records active-channel selection before native overwrites the source byte and calls Select.
    void handle_packed_sync(void *adjusted_soldier, int target_channel) noexcept;

    // Assigns a local soldier to its input slot and validates its retained inventory before recording selection.
    bool track_local_soldier(void *soldier, int local_player) noexcept;

    // Classifies a server channel selection as pending, settled or corrective and arms the next Select's sound decision.
    void handle_authority_selection(void *soldier, int channel, int current_index, int server_index) noexcept;

    // Classifies a received packed transition using its old channel and slot.
    void handle_packed_authority(void *soldier, int old_channel, int old_index, std::uint8_t server_packed) noexcept;

    // Links packed history to a predicted channel request with the same slot and weapon pointer.
    bool track_matching_projection(void *soldier, int local_player, int channel, int index, void *weapon,
                                   const TurnFrontiers &frontiers) noexcept;

    // Returns the predicted channel weapon while every recorded inventory pointer still matches.
    void *resolve_hud_weapon(void *controllable, int channel, void *actual_weapon) noexcept;

    // One operation uses these channel slots and active byte, all validated against the same current inventory.
    struct PresentationView {
        void *soldier{};
        std::array<void *, kWeaponSlotCount> weapons{};
        std::array<std::int8_t, kWeaponChannelCount> slots{};
        std::uint8_t packed{};
    };

    struct RenderInvocation {
        RenderInvocation *previous{};
        std::optional<PresentationView> view;
    };

    // Builds a coherent channel/active selection view, invalidating both histories on inventory contradiction.
    std::optional<PresentationView> presentation_view(void *soldier) noexcept;

    // Returns the current invocation's captured view while client prediction and local ownership remain eligible.
    const PresentationView *render_view(const fc::CpuContext &context) noexcept;

    // Invalidates local histories, input provenance and any active presentation view for this native soldier.
    void invalidate_soldier(void *soldier) noexcept;

    // Clears both weapon histories and pending intent when their shared inventory ownership contradicts native state.
    void invalidate_player(int local_player) noexcept;

    // Checks client lifecycle before consuming an intent that mutes only the base change sound.
    void suppress_duplicate_select(fc::CpuContext &context) noexcept;

    // Tests locally owned held input only during active network prediction.
    bool suppress_held_switch(void *controllable, int channel, std::uint32_t direction) const noexcept;

    // Compares adjacent prediction samples only after their turns and native player identity were recorded.
    bool held_switch_replay(int local_player, int player_id, int channel, std::uint32_t trigger, std::uint32_t direction) const noexcept;

    // Returns the native local input slot, or -1 for a remote player or failed lookup.
    int local_player_index(void *soldier) const noexcept;

    // Finds the local player whose retained soldier identity matches this object.
    int tracked_local_player(void *soldier) const noexcept;

    // Tests whether this local input slot still owns the retained soldier.
    bool is_tracked_local_soldier(void *soldier, int local_player) const noexcept;

    // Requires joining Client mode, the native client world and valid prediction counters.
    bool network_prediction_active() const noexcept;

    // Clears ended/changed sessions and native counter resets; world switches only suspend reconciliation.
    // Advances history only with valid client-world prediction frontiers.
    void observe_lifecycle() noexcept;

    // Enables presentation hooks while either ledger retains weapon selection history.
    void update_presentation_activity() noexcept;

    // Drops both ledgers, retained soldier identities and the pending selection intent.
    void clear_prediction_state() noexcept;

    // Drops all prediction state and forgets the native network lifecycle.
    void clear_tracking() noexcept;

    // Snapshots the native update, prediction and acknowledgement counters.
    TurnFrontiers frontiers() const noexcept;

    // Copies one computed selection decision with the current exact frontiers; no native helpers are called.
    void selection_decision(void *soldier, hero_diagnostics::SelectionDecision decision) const noexcept;

    // Checks every retained channel inventory pointer against the current soldier.
    bool validate_snapshot(const EpochSnapshot &snapshot) const noexcept;

    // Requires a recorded predicted weapon and checks every recorded weapon pointer against the inventory.
    bool validate_packed_snapshot(const PackedSnapshot &snapshot) const noexcept;

    // Checks both histories before recording or muting; a contradiction invalidates their shared ownership.
    bool validate_player_history(void *soldier, int local_player) noexcept;

    // Adjusts a borrowed Controllable subobject back to its EntitySoldier owner.
    static void *soldier_from_controllable(void *controllable) noexcept;

    // Returns the four-byte native weapon pointer for a valid inventory slot.
    static void *weapon_at(void *soldier, int index) noexcept;

    fc::NativeFunction<GetJoystickIndex> get_joystick_index_;
    fc::NativeFunction<GetLocalPlayer> get_local_player_;

    fc::Memory<const std::int32_t> update_turn_memory_;
    fc::Memory<const std::int32_t> prediction_turn_memory_;
    fc::Memory<const std::int32_t> acknowledged_turn_memory_;

    fc::Memory<const std::uint8_t> network_enabled_memory_;
    fc::Memory<const std::uint8_t> network_client_active_memory_;

    fc::Memory<const std::int32_t> client_turn_memory_;
    fc::Memory<const LocalMoveHistory> local_move_history_memory_;

    fc::Memory<const std::byte> primary_switch_call_memory_;
    fc::Memory<const std::byte> secondary_switch_call_memory_;

    fc::Game game_;

    // The game thread compares this session identity before retaining any soldier or weapon pointer.
    fc::SessionId session_{};

    const std::int32_t *update_turn_{};
    const std::int32_t *prediction_turn_{};
    const std::int32_t *acknowledged_turn_{};

    const std::uint8_t *network_enabled_{};
    const std::uint8_t *network_client_active_{};

    const std::int32_t *client_turn_{};
    const LocalMoveHistory *local_move_history_{};

    // Return addresses identify the two PlayerController::Update calls to Controllable::SetSwitch.
    std::uintptr_t primary_switch_return_{};
    std::uintptr_t secondary_switch_return_{};

    LocalSwapLedger ledger_;
    PackedSwapLedger packed_ledger_;
    // Borrowed soldier pointers recorded by manual or automatic local selection callbacks.
    std::array<void *, kLocalPlayerCount> local_soldiers_{};

    NetworkLifecycle lifecycle_;
    bool presentation_active_{};

    // Selection callbacks and their immediate Weapon::Select calls share one serial game-thread intent.
    SelectIntent select_intent_;
    hero_diagnostics::HeroDiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};
    RecordedMoves recorded_moves_;
    // Native observer storage owns each invocation; this link restores the enclosing render view after nested calls.
    RenderInvocation *render_invocation_{};
};

} // namespace fc_plugins::weapon_swap_replay_fix
