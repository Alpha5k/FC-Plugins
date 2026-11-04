#include "patch.hpp"

#include "../hero_diagnostics/api.hpp"

#include <fc/native.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::infinite_sprint_patch {
namespace {

struct EntitySoldier;
struct Controllable;

// Both native methods take their object in ECX and return success in AL.
using RollUsingEnergy = bool(__thiscall *)(EntitySoldier *) noexcept;
using EndSprint = bool(__thiscall *)(Controllable *) noexcept;

// Embedded Controllable base within EntitySoldier.
constexpr std::size_t kControllableOffset = 0x240;
// EnergyBar flags word at EntitySoldier + 0xA10 + 8.
constexpr std::size_t kEnergyFlagsOffset = 0xA18;
// Byte offset of the virtual EndSprint entry in Controllable's vtable.
constexpr std::size_t kEndSprintVtableOffset = 0xB0;
// EnergyBar's overheat bit, cleared only after energy reaches its recovery threshold.
constexpr std::uint32_t kEnergyOverheated = 1;

// Locates the sprint caller that needs its roll transition completed.
struct Layout {
    fc::CallLocation<RollUsingEnergy> sprint_roll;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return Layout{
        .sprint_roll = {.rva = 0x000EB146,
                        .name = "sprint roll request",
                        .evidence = fc::direct_call_to(fc::Location{.image = "BF2.GOG.59EDF52B", .rva = 0x000EDD30})},
    };
}

// Completes the native sprint transition after the one roll caller that exits early.
class Handler {
  public:
    // Replaces the sprint state's direct CALL to RollUsingEnergy.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        roll_ = plan.hook_call(layout->sprint_roll, [this](EntitySoldier *soldier) noexcept {
            return roll_using_energy(soldier);
        });
    }

    // Optional reporting copies the branch operands after the native roll result is known.
    fc::Result<> prepare(fc::PrepareContext &context) {
        return context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                    [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                        diagnostics_ = api;
                                        diagnostics_ready_.store(true, std::memory_order_release);
                                    });
    }

  private:
    // Keeps the roll result and ends sprint only when a failed roll leaves EnergyBar overheated.
    bool roll_using_energy(EntitySoldier *soldier) const noexcept {
        const auto rolled = roll_.call(soldier);
        if (!rolled || *rolled) {
            if (diagnostics_ready_.load(std::memory_order_acquire)) {
                const hero_diagnostics::PhysicsDecision decision{.operation = hero_diagnostics::PhysicsOperation::ExhaustedRoll,
                                                                 .reason = static_cast<std::uint8_t>(rolled ? 3 : 2),
                                                                 .result = static_cast<std::uint8_t>(rolled.value_or(false))};
                diagnostics_.physics_decision(soldier, 1, &decision);
            }
            return rolled.value_or(false);
        }

        // The sprint caller exits immediately after a roll request, skipping its ordinary overheat exit.
        // A refused roll can refund energy while leaving overheat set, so inspect the flag after the call.
        const auto energy_flags = fc::read_field<std::uint32_t>(soldier, kEnergyFlagsOffset);
        bool ended{};
        if ((energy_flags & kEnergyOverheated) != 0) {
            // EntitySoldier::Update uses this same Controllable subobject and virtual EndSprint slot.
            auto *controllable_bytes = reinterpret_cast<std::byte *>(soldier) + kControllableOffset;
            auto *controllable = reinterpret_cast<Controllable *>(controllable_bytes);
            const auto *vtable = fc::read_field<const std::byte *>(controllable);

            const auto end_sprint = fc::read_field<EndSprint>(vtable, kEndSprintVtableOffset);
            ended = end_sprint(controllable);
        }

        if (diagnostics_ready_.load(std::memory_order_acquire)) {
            const hero_diagnostics::PhysicsDecision decision{.words = {energy_flags},
                                                             .operation = hero_diagnostics::PhysicsOperation::ExhaustedRoll,
                                                             .reason = static_cast<std::uint8_t>((energy_flags & kEnergyOverheated) != 0),
                                                             .result = static_cast<std::uint8_t>(ended)};
            diagnostics_.physics_decision(soldier, 1, &decision);
        }

        return false;
    }

    fc::NativeFunction<RollUsingEnergy> roll_;
    hero_diagnostics::HeroDiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "InfiniteSprintPatch",
                     .description =
                         "Fixes the infinite sprint bug where players can glitch their stamina bar to run without losing stamina.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::infinite_sprint_patch
