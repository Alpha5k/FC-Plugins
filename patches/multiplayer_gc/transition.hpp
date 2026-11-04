#pragma once

#include "protocol.hpp"

#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace fc_plugins::multiplayer_gc {

// Copies campaign assignments and admission policy without retaining transport or Lua objects.
struct TransitionSession {
    bool active{};
    bool host{};
    bool admission_closed{};
    std::vector<Player> players;
};

using TransitionDelete = fc::NativeCall<void *(void *, std::uint32_t) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using TransitionInit = fc::NativeCall<void(void *, bool) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using TransitionCommCreate = fc::NativeCall<void(bool) noexcept, fc::abi::Ecx>;
using TransitionConnected = fc::NativeCall<bool(std::uint32_t) noexcept, fc::abi::Ecx>;
using TransitionTime = fc::NativeCall<float() noexcept, fc::abi::X86<fc::abi::Args<>, fc::abi::Result<fc::abi::Xmm0>>>;
using TransitionDrop = fc::NativeCall<void(void *, std::uint32_t) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using TransitionReset = fc::NativeCall<void(void *) noexcept, fc::abi::Ecx>;
using TransitionTranslate = fc::NativeCall<std::uint32_t(const char *) noexcept, fc::abi::Ecx>;
using TransitionCharacter = fc::NativeCall<bool(void *, std::uint32_t) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using TransitionSpawnTeam = fc::NativeCall<void(void *, std::uint32_t) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using TransitionLocalSlot = std::int32_t(__cdecl *)(std::uint32_t) noexcept;
using TransitionCreate = void(__cdecl *)(bool) noexcept;
using TransitionAllocate = void *(__cdecl *)(std::uint32_t) noexcept;
using TransitionBan = bool(__cdecl *)(std::uint32_t, void *) noexcept;
using TransitionTeam = void(__cdecl *)(std::uint32_t, std::uint32_t) noexcept;
using TransitionPostload = std::uint32_t(__cdecl *)() noexcept;
using TransitionNextMission = bool(__cdecl *)() noexcept;

// Continues the established native lobby across normal mission cleanup and Runtime-heap rebuilding.
class Transition {
  public:
    // Qualifies the native calls and reserves only the successful GC transition changes.
    void plan(fc::Plan &plan);
    // Publishes control-thread state; native calls remain on their original owner thread.
    void configure(TransitionSession session);
    // Seeds changed accepted shell assignments once per campaign update on the Lua owner thread.
    void update() noexcept;
    // Arms one battle after its exact native mission setup has been applied on the Lua owner thread.
    fc::Result<> prepare(std::string_view faction_one, std::string_view faction_two);
    // Restores normal quit/loss behavior before an explicit campaign leave.
    void disarm() noexcept;
    // Reports actual successful driver continuation before the new shell's first Lua Enter.
    bool shell() const noexcept;
    // Reports successful native shell launch, including mission loading and statistics.
    bool mission() const noexcept;
    // Runs on the Lua owner thread and identifies the active captured GC host battle.
    bool host_mission() const noexcept;
    // Reports the stored native failure without servicing players or changing admission.
    fc::Result<> check() const;

  private:
    enum class Stage : std::uint8_t {
        Idle,
        Prepared,
        Mission,
        Returning,
        Shell
    };
    enum class Failure : std::uint8_t {
        None,
        Allocate,
        TeamSides
    };

    // Rejects still-owned transfer/browser state before copying an established native driver.
    bool established(void *driver, bool host) const noexcept;
    // Copies an established prepared driver before destruction; returns whether this call produced the snapshot.
    bool capture(void *driver, bool host) noexcept;
    // Allocates on the rebuilt Runtime heap and attaches the established driver before shell Lua starts.
    void resume() noexcept;
    // Matches accepted factions to the mission's actual teams before native player integration.
    void resolve_teams() noexcept;
    // Uses the original host drop operation when preparation closes admission to unassigned visitors.
    void remove_unassigned() noexcept;
    // Returns an accepted player's mission team while the native mission mapping is valid.
    std::uint32_t assigned_team(std::int32_t slot) const noexcept;
    // Returns the accepted campaign faction only while the native shell is active.
    std::uint32_t shell_team(std::int32_t slot) const noexcept;
    // Identifies stages with an armed native continuation, including preparation before capture.
    bool continuing() const noexcept;

    std::atomic<std::shared_ptr<const TransitionSession>> session_;
    std::atomic<Stage> stage_{};
    std::atomic<Failure> failure_{};
    // Create preservation is synchronous; the saved driver kind outlives the destroyed shell allocation.
    bool preserving_{}, saved_host_{};
    // Shell-thread ownership: the last seeded immutable view is forgotten after heap replacement.
    std::shared_ptr<const TransitionSession> shell_session_;
    std::array<std::byte, 0x1590> driver_{};
    std::array<std::uint32_t, 2> faction_sides_{}, faction_teams_{};
    fc::Memory<void *> host_, join_, teams_;
    fc::Memory<void *> postload_host_, postload_join_;
    fc::Memory<std::uint32_t> setup_mode_;
    fc::Memory<bool> team_selected_;
    fc::Memory<std::uint8_t> shell_state_;
    fc::NativeFunction<TransitionDelete> host_delete_, join_delete_;
    fc::NativeFunction<TransitionCreate> create_;
    fc::NativeFunction<TransitionInit> player_init_, host_init_;
    fc::NativeFunction<TransitionCommCreate> comm_create_;
    fc::NativeFunction<TransitionAllocate> allocate_;
    fc::NativeFunction<TransitionConnected> connected_;
    fc::NativeFunction<TransitionLocalSlot> local_slot_;
    fc::NativeFunction<TransitionTime> time_;
    fc::NativeFunction<TransitionDrop> drop_;
    fc::NativeFunction<TransitionReset> reset_player_;
    fc::NativeFunction<TransitionBan> ban_;
    fc::NativeFunction<TransitionTeam> set_team_;
    fc::NativeFunction<TransitionPostload> postload_;
    fc::NativeFunction<TransitionNextMission> next_mission_;
    fc::NativeFunction<TransitionTranslate> translate_;
    fc::NativeFunction<TransitionCharacter> change_team_;
    fc::NativeFunction<TransitionSpawnTeam> spawn_team_;
};

} // namespace fc_plugins::multiplayer_gc
