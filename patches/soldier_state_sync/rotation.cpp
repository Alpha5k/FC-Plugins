#include "corrections.hpp"

#include "rotation.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fc_plugins::soldier_state_sync {
namespace {

struct ReturnedPair {
    fc::CodeLocation location;
    std::uint8_t sine_offset;
    std::uint8_t cosine_offset;
};

struct Layout {
    std::array<ReturnedPair, 3> pairs;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t ground;
    std::uint32_t air;
    std::uint32_t hover;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .ground = 0x000EEAD9, .air = 0x000EEEA0, .hover = 0x000EF2A9},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .ground = 0x000EEAD9, .air = 0x000EEEA0, .hover = 0x000EF2A9},
};

// Qualifies the first MOVSS after SinCos returns, before the caller removes its three arguments.
ReturnedPair returned_pair(std::uint32_t rva, std::string name, std::uint8_t sine_offset, std::uint8_t cosine_offset) {
    return {.location = {.rva = rva, .name = std::move(name), .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x4C, 0x24, sine_offset})},
            .sine_offset = sine_offset,
            .cosine_offset = cosine_offset};
}

// Steam calls 727C90 and GOG calls 728D60; each caller reads its own returned stack pair.
Layout make_layout(const ProfileSites &sites) {
    return {.pairs = {returned_pair(sites.ground, "Soldier ground rotation returned pair", 0x18, 0x1C),
                      returned_pair(sites.air, "Soldier air rotation returned pair", 0x28, 0x24),
                      returned_pair(sites.hover, "Soldier jet hover rotation returned pair", 0x20, 0x1C)}};
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

} // namespace

void GroundRotation::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    for (const auto &pair : layout->pairs) {
        // A four-byte LEA separates the sine load from MOVSS XMM2,[ESP+cosine_offset].
        plan.check(pair.location.rva + 10, 6, fc::exact_bytes({0xF3, 0x0F, 0x10, 0x54, 0x24, pair.cosine_offset}));
        plan.hook_instruction(pair.location,
                              [this, sine_offset = pair.sine_offset, cosine_offset = pair.cosine_offset](fc::CpuContext &context) noexcept {
                                  if (!applies()) {
                                      return;
                                  }

                                  const auto stack = fc::stack_pointer(context);
                                  float sine = fc::read_field<float>(stack, sine_offset);
                                  float cosine = fc::read_field<float>(stack, cosine_offset);
                                  normalize_rotation(sine, cosine);
                                  fc::write_field(stack, sine_offset, sine);
                                  fc::write_field(stack, cosine_offset, cosine);
                              });
    }
}

fc::Result<> GroundRotation::prepare(fc::PrepareContext &context) {
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    const auto feature = multiplayer_.feature({"FC.SoldierGroundRotationFix", fc::FeatureRole::Both});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    return {};
}

bool GroundRotation::applies() const noexcept {
    const auto state = game_.current();
    if (!state) {
        return false;
    }
    if (state->mode == fc::GameMode::Local || state->mode == fc::GameMode::Host || state->mode == fc::GameMode::Dedicated) {
        return true;
    }
    if (state->mode == fc::GameMode::Client) {
        const auto host = multiplayer_.host();
        return host.value != 0 && feature_.agreement(host) == fc::Agreement::Compatible;
    }
    return false;
}

} // namespace fc_plugins::soldier_state_sync
