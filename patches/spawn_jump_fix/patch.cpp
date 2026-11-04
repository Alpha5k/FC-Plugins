#include "patch.hpp"

#include <fc/native.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::spawn_jump_fix {
namespace {

// Marks warmup units for removal while retaining their Character records.
using VanishAllPlayers = void(__cdecl *)() noexcept;

// IsPlaying takes two cdecl stack arguments; the character lookup takes the player ID in ECX.
using IsPlaying = bool(__cdecl *)(int player, bool next_state) noexcept;
using CharacterByPlayer = std::byte *(__fastcall *)(int player) noexcept;

constexpr std::size_t kTeamCount = 8;
constexpr int kMaximumPlayers = 64;

// SpawnManager arrays have one entry per team; all delay and timer values are float seconds.
constexpr std::size_t kCycleDelayOffset = 0x5C;
// Delay between eligible slots within a team wave.
constexpr std::size_t kSlotDelayOffset = 0x7C;
// Remaining time before each team's next cycle.
constexpr std::size_t kCycleTimerOffset = 0x9C;
// Signed 32-bit cycle counters, advanced when the team's cycle timer expires.
constexpr std::size_t kCycleIndexOffset = 0xBC;
// Remaining delay before a character may use the current wave.
constexpr std::size_t kSlotTimerOffset = 0xDC;

// Native team index on Character.
constexpr std::size_t kCharacterTeamOffset = 0x134;
// Earliest signed team cycle in which Character may spawn.
constexpr std::size_t kCharacterSpawnCycleOffset = 0x15C;

// Groups warmup removal, player lookup, and spawn-manager locations.
struct Layout {
    fc::FunctionLocation<IsPlaying> is_playing;
    fc::FunctionLocation<CharacterByPlayer> character_by_player;
    fc::CodeLocation manager_reference;
    fc::CodeLocation players_reference;
    fc::DataLocation<std::byte *const> spawn_manager;
    fc::DataLocation<const int> maximum_players;
    fc::CodeLocation warmup_branch;
    fc::CallLocation<VanishAllPlayers> vanish_players;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .is_playing = {.rva = 0x001C4530, .name = "NetGame::IsPlaying"},
        .character_by_player = {.rva = 0x00029430, .name = "Character::GetByPlayer"},
        .manager_reference = {.rva = 0x0018D61E, .name = "spawn manager load", .evidence = fc::exact_bytes({0x8B, 0x0D})},
        .players_reference = {.rva = 0x001C50EB, .name = "player limit comparison", .evidence = fc::exact_bytes({0x3B, 0x0D})},
        .spawn_manager = {.rva = 0x01AB0FE8},
        .maximum_players = {.rva = 0x01A64408},
        .warmup_branch = {.rva = 0x001C4F35, .name = "warmup timer branch", .evidence = fc::exact_bytes({0x7E, 0x1D})},
        .vanish_players = {.rva = 0x001C4F37,
                           .name = "warmup player removal",
                           .evidence = fc::direct_call_to(fc::Location{.image = "BF2.GOG.59EDF52B", .rva = 0x001C50D0})},
    };
}

// Restores each team's timers and active players' first spawn cycle after warmup removal.
class Handler {
  public:
    // Ends warmup at timer equality and intercepts only its player-removal call.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        is_playing_ = plan.function(layout->is_playing);

        character_by_player_ = plan.function(layout->character_by_player);

        // Check the instructions and their relocated references to SpawnManager and the player-slot upper bound.
        plan.check(layout->manager_reference, 2);
        plan.check(plan.at(layout->manager_reference.rva + 2, "spawn manager reference"), 4,
                   fc::points_to(plan.at(layout->spawn_manager.rva)));
        plan.check(layout->players_reference, 2);
        plan.check(plan.at(layout->players_reference.rva + 2, "player limit reference"), 4,
                   fc::points_to(plan.at(layout->maximum_players.rva)));

        spawn_manager_ = plan.access(layout->spawn_manager);
        maximum_players_ = plan.access(layout->maximum_players);

        // UpdatePreGame truncates elapsed seconds to an integer. Changing JLE to JL ends warmup on
        // equality with the limit, rather than waiting until the next integer second.
        plan.write(layout->warmup_branch, fc::byte_array<0x7C, 0x1D>());

        // The callback resets waves after removal marking and before the caller clears its pregame flag.
        vanish_all_players_ = plan.hook_call(layout->vanish_players, [this]() noexcept {
            if (vanish_all_players_.call()) {
                finish_warmup();
            }
        });
    }

  private:
    // Re-arms team timers, then queues each active character for its team's next wave.
    void finish_warmup() const noexcept {
        const auto manager = spawn_manager_.read();
        if (!manager || !*manager) {
            return;
        }

        auto *spawn_manager = *manager;

        // Warmup advances cycle indices on its short timer; restart the configured cycle and slot delays.
        for (std::size_t team = 0; team < kTeamCount; ++team) {
            const auto offset = team * sizeof(float);
            fc::write_field(spawn_manager, kCycleTimerOffset + offset, fc::read_field<float>(spawn_manager, kCycleDelayOffset + offset));
            fc::write_field(spawn_manager, kSlotTimerOffset + offset, fc::read_field<float>(spawn_manager, kSlotDelayOffset + offset));
        }

        const auto maximum_players = maximum_players_.read();
        if (!maximum_players) {
            return;
        }

        // The bound is highest occupied slot plus one, so IsPlaying skips holes in the player table.
        const int player_count = std::clamp(*maximum_players, 0, kMaximumPlayers);
        for (int player = 0; player < player_count; ++player) {
            if (!is_playing_.call(player, false).value_or(false)) {
                continue;
            }

            auto *character = character_by_player_.call(player).value_or(nullptr);
            if (!character) {
                continue;
            }

            const int team = fc::read_field<int>(character, kCharacterTeamOffset);
            if (team < 0 || team >= static_cast<int>(kTeamCount)) {
                continue;
            }

            // Character::Update waits for this cycle and the slot timer. Queue current playing characters,
            // including those already waiting without a unit, for the next cycle.
            const auto offset = static_cast<std::size_t>(team) * sizeof(std::int32_t);
            const auto cycle = fc::read_field<std::int32_t>(spawn_manager, kCycleIndexOffset + offset);
            fc::write_field(character, kCharacterSpawnCycleOffset, cycle + 1);
        }
    }

    fc::NativeFunction<VanishAllPlayers> vanish_all_players_;
    fc::NativeFunction<IsPlaying> is_playing_;
    fc::NativeFunction<CharacterByPlayer> character_by_player_;

    // Pointer slot reread at warmup end; EnterClient/LeaveClient select the current manager.
    fc::Memory<std::byte *const> spawn_manager_;
    fc::Memory<const int> maximum_players_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "SpawnJumpFix",
                     .description = "Fixes the bug where players can spawn right as warmup timer ends and skip the first wave.",
                     .category = "Server",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::spawn_jump_fix
