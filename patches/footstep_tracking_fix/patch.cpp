#include "patch.hpp"
#include "../sound_diagnostics/api.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::footstep_tracking_fix {
namespace {

// UpdateFootHeight uses ECX for Soldier and two callee-cleaned stack arguments: evaluated pose and transform.
using HeightUpdate = void(__thiscall *)(void *, void *, const float *) noexcept;
using FirstPerson = bool(__thiscall *)(void *) noexcept;
// The pose hash table lookup takes caller-cleaned stack arguments and returns a borrowed bone matrix.
using Joint = const void *(__cdecl *)(const void *, std::uint32_t, std::uint32_t) noexcept;

// Both qualified height routines use these Soldier fields, matrix height and direction threshold.
constexpr std::ptrdiff_t kSupportOffset = 0x9D4;
constexpr std::ptrdiff_t kTrackerOffset = 0x274;
constexpr std::ptrdiff_t kFlagsOffset = 0x9D6;
constexpr std::ptrdiff_t kHeightsOffset = 0xA08;
constexpr std::ptrdiff_t kMatrixHeightOffset = 0x34;
constexpr float kDirectionThreshold = 0.01F;
constexpr std::array<std::uint32_t, 2> kFootHashes{0x1AF77461, 0x2F06FA86};

struct Layout {
    fc::FunctionLocation<HeightUpdate> height;
    fc::FunctionLocation<FirstPerson> first_person;
    fc::FunctionLocation<Joint> joint;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t first_person;
    std::uint32_t joint;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .first_person = 0x0004E3C0, .joint = 0x00326E00},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .first_person = 0x0004E3A0, .joint = 0x00327ED0},
};

// Both UpdateFootHeight routines read support flag 0x02 and the tracker view before sampling pose+4 with these hashes.
Layout make_layout(const ProfileSites &sites) {
    return {
        .height = {.rva = 0x000E8030,
                   .name = "soldier rendered foot heights",
                   .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .first_person = {.rva = sites.first_person,
                         .name = "soldier tracker first person view",
                         .evidence = fc::exact_bytes({0x56, 0x57, 0x8B, 0xF9})},
        .joint = {.rva = sites.joint,
                  .name = "evaluated pose joint lookup",
                  .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C})},
    };
}

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

class Handler final {
  public:
    // Maintains existing foot state before native early returns, leaving eligible sound and effects to UpdateFootHeight.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        first_person_ = plan.function(layout->first_person);
        joint_ = plan.function(layout->joint);
        plan.observe(layout->height, fc::before([this](void *soldier, void *pose, const float *) noexcept {
                         maintain_heights(soldier, pose);
                     }));
    }

    // Connects the optional maintenance observation without making diagnostics an activation dependency.
    fc::Result<> prepare(fc::PrepareContext &context) {
        return context.on_available("Diagnostics.SoundDiagnostics", sound_diagnostics::api_contract(),
                                    [this](sound_diagnostics::SoundDiagnosticsApi api) noexcept {
                                        diagnostics_ = api;
                                        diagnostics_ready_.store(true, std::memory_order_release);
                                    });
    }

  private:
    // Preserves native thread errors while copying the completed owner's outcome.
    void record(void *soldier, const sound_diagnostics::FootMaintenance &outcome) const noexcept {
        if (diagnostics_ready_.load(std::memory_order_acquire)) {
            const auto win32 = GetLastError();
            const auto winsock = WSAGetLastError();
            diagnostics_.foot_maintenance(soldier, &outcome);
            WSASetLastError(winsock);
            SetLastError(win32);
        }
    }

    // Tests the optional recorder before sampling observation-only baseline fields.
    bool observes_maintenance() const noexcept {
        if (!diagnostics_ready_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto win32 = GetLastError();
        const auto winsock = WSAGetLastError();
        const bool enabled = diagnostics_.foot_maintenance_enabled();
        WSASetLastError(winsock);
        SetLastError(win32);
        return enabled;
    }

    // Uses only the supplied evaluated pose; intervals where Render supplies no pose have no new state.
    void maintain_heights(void *soldier, const void *pose) const noexcept {
        const bool observed = observes_maintenance();
        sound_diagnostics::FootMaintenance outcome{};
        outcome.support = fc::read_field<std::uint8_t>(soldier, kSupportOffset);
        if (observed) {
            outcome.pose = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pose));
            for (std::size_t foot = 0; foot < 2; ++foot) {
                outcome.old_heights[foot] = outcome.result_heights[foot] =
                    fc::read_field<float>(soldier, kHeightsOffset + static_cast<std::ptrdiff_t>(foot * sizeof(float)));
            }
            outcome.flags_before = outcome.flags_after = fc::read_field<std::uint16_t>(soldier, kFlagsOffset);
        }
        if ((outcome.support & 2U) != 0) {
            const auto tracker = fc::read_field<void *>(soldier, kTrackerOffset);
            if (!tracker) {
                outcome.reason = 1;
                if (observed) {
                    record(soldier, outcome);
                }
                return;
            }
            if (!first_person_.call(tracker).value()) {
                outcome.reason = 2;
                if (observed) {
                    record(soldier, outcome);
                }
                return;
            }
        }

        auto flags = static_cast<std::uint16_t>(fc::read_field<std::uint16_t>(soldier, kFlagsOffset) & ~12U);
        for (std::size_t foot = 0; foot < kFootHashes.size(); ++foot) {
            const auto matrix = joint_.call(static_cast<const std::byte *>(pose) + 4, 0x100, kFootHashes[foot]).value();
            const float height = fc::read_field<float>(matrix, kMatrixHeightOffset);
            const auto offset = kHeightsOffset + static_cast<std::ptrdiff_t>(foot * sizeof(float));
            const float previous = fc::read_field<float>(soldier, offset);
            outcome.bones[foot] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(matrix));
            outcome.sampled_heights[foot] = height;
            outcome.result_heights[foot] = height;
            const auto armed = static_cast<std::uint16_t>(1U << foot);

            // Native descent arms the foot and ascent disarms it; ineligible poses emit no notification or effect.
            if (previous - height > kDirectionThreshold) {
                flags |= armed;
                outcome.directions[foot] = 1;
            } else if (height - previous > kDirectionThreshold) {
                flags &= static_cast<std::uint16_t>(~armed);
                outcome.directions[foot] = 2;
            }
            fc::write_field(soldier, offset, height);
        }
        fc::write_field(soldier, kFlagsOffset, flags);
        outcome.flags_after = flags;
        outcome.reason = 3;
        if (observed) {
            record(soldier, outcome);
        }
    }

    sound_diagnostics::SoundDiagnosticsApi diagnostics_;
    std::atomic_bool diagnostics_ready_{};
    fc::NativeFunction<FirstPerson> first_person_;
    fc::NativeFunction<Joint> joint_;
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "FootstepTrackingFix",
                     .description = "Prevents duplicated footstep sounds particularly when landing.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::footstep_tracking_fix
