#include "patch.hpp"

#include "../hero_diagnostics/api.hpp"

#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace fc_plugins::aerial_gravity {
namespace {

// Native locations used to change melee gravity compensation.
struct Layout {
    fc::DataLocation<const float> stock_gravity;
    fc::DataLocation<const float> client_delta;
    fc::DataLocation<const float> simulation_delta;
    // Unaligned absolute-address operands checked separately from the MULSS opcodes.
    std::uint32_t gravity_operand;
    std::uint32_t client_delta_operand;
    fc::CodeLocation gravity_multiply;
    fc::CodeLocation elapsed_time_multiply;
    fc::CodeLocation prediction_inputs;
};

// RVAs of the melee velocity function and its gravity and time-step values.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t velocity;
    std::uint32_t gravity;
    std::uint32_t delta;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353", .velocity = 0x00288C30, .gravity = 0x003B22C4, .delta = 0x01A56054},
    ProfileSites{.profile = "BF2.GOG.59EDF52B", .velocity = 0x00289CB0, .gravity = 0x003B323C, .delta = 0x01A574EC},
};

// Describes the gravity values and instructions used by the selected build.
Layout make_layout(const ProfileSites &sites) {
    // MULSS uses unaligned absolute operands; planning checks their addresses separately from these opcodes.
    const auto multiply = fc::masked_bytes({0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
    return {
        .stock_gravity = {.rva = sites.gravity, .name = "stock melee gravity compensation"},
        .client_delta = {.rva = sites.delta + 4, .name = "client prediction interval in seconds"},
        .simulation_delta = {.rva = sites.delta, .name = "current simulation step in seconds"},
        .gravity_operand = sites.velocity + 0xFE,
        .client_delta_operand = sites.velocity + 0x106,
        .gravity_multiply = {.rva = sites.velocity + 0xFA, .name = "aerial gravity multiply", .evidence = multiply},
        .elapsed_time_multiply = {.rva = sites.velocity + 0x102, .name = "aerial elapsed time multiply", .evidence = multiply},
        // Combo factor one skips this adjustment; direct vertical-velocity assignments use another branch.
        .prediction_inputs = {.rva = sites.velocity + 0xF2, .name = "aerial prediction inputs"},
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

// Selects melee combo gravity compensation for local play, player hosts, dedicated servers and compatible joining clients.
class Handler final {
  public:
    // Redirects the melee prediction operands to storage updated immediately before their native reads.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        stock_gravity_memory_ = plan.access(layout->stock_gravity);
        client_delta_memory_ = plan.access(layout->client_delta);
        simulation_delta_memory_ = plan.access(layout->simulation_delta);

        const auto values = plan.allocate<float>(std::array{18.0F, 0.0F});
        values_memory_ = plan.access<float>(values);

        // The absolute operands are unaligned; verify their relocated addresses with byte reads.
        plan.check(layout->gravity_operand, 4, fc::points_to(plan.at(layout->stock_gravity.rva)));
        plan.check(layout->client_delta_operand, 4, fc::points_to(plan.at(layout->client_delta.rva)));

        plan.operand(layout->gravity_multiply, 1, values.address());
        plan.operand(layout->elapsed_time_multiply, 1, values.address(1));

        plan.observe_instruction(layout->prediction_inputs, fc::before([this](const fc::CpuContext &context) noexcept {
                                     update_inputs(context);
                                 }));
    }

    // Resolves native views and registers both feature roles so player hosts and dedicated servers advertise their aerial physics.
    fc::Result<> prepare(fc::PrepareContext &context) {
        const auto gravity = stock_gravity_memory_.view();
        if (!gravity) {
            return std::unexpected(gravity.error());
        }

        const auto client = client_delta_memory_.view();
        if (!client) {
            return std::unexpected(client.error());
        }

        const auto simulation = simulation_delta_memory_.view();
        if (!simulation) {
            return std::unexpected(simulation.error());
        }

        const auto values = values_memory_.view();
        if (!values) {
            return std::unexpected(values.error());
        }

        stock_gravity_ = gravity->data();
        client_delta_ = client->data();
        simulation_delta_ = simulation->data();
        values_ = values->data();

        game_ = context.game();
        multiplayer_ = context.multiplayer();

        const auto feature = multiplayer_.feature({"FC.AerialGravity", fc::FeatureRole::Both});
        if (!feature) {
            return std::unexpected(feature.error());
        }

        feature_ = *feature;

        return context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                    [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                        diagnostics_ = api;
                                        diagnostics_ready_.store(true, std::memory_order_release);
                                    });
    }

  private:
    // Uses corrected compensation locally and on hosts. Joining clients require a compatible current host;
    // missing game state or agreement selects the stock gravity and client interval.
    void update_inputs(const fc::CpuContext &context) noexcept {
        const auto state = game_.current();
        bool corrected = false;

        if (state) {
            // Local and hosted games own their physics, including LAN games without feature negotiation.
            corrected = state->mode == fc::GameMode::Local || state->mode == fc::GameMode::Host || state->mode == fc::GameMode::Dedicated;

            if (state->mode == fc::GameMode::Client) {
                // Joining LAN clients have no feature agreement and keep the stock operands.
                const auto host = multiplayer_.host();
                corrected = host.value != 0 && feature_.agreement(host) == fc::Agreement::Compatible;
            }
        }

        // Native code subtracts (combo factor - 1) * gravity * seconds from vertical velocity.
        // The 0.75 tuning scales this compensation; ordinary downward gravity still runs in soldier movement.
        values_[0] = *stock_gravity_ * (corrected ? 0.75F : 1.0F);
        values_[1] = corrected ? *simulation_delta_ : *client_delta_;

        if (diagnostics_ready_.load(std::memory_order_acquire)) {
            const hero_diagnostics::PhysicsDecision decision{
                .values = {std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[1].low)), *stock_gravity_, values_[0],
                           *simulation_delta_, *client_delta_, values_[1]},
                .words = {state ? static_cast<std::uint32_t>(state->mode) : 0xFFFFFFFFU},
                .operation = hero_diagnostics::PhysicsOperation::AerialCompensation,
                .result = static_cast<std::uint8_t>(corrected)};
            diagnostics_.physics_decision(reinterpret_cast<void *>(context.esi), 0, &decision);
        }
    }

    fc::Memory<const float> stock_gravity_memory_;
    fc::Memory<const float> client_delta_memory_;
    fc::Memory<const float> simulation_delta_memory_;
    fc::Memory<float> values_memory_;

    const float *stock_gravity_{};
    const float *client_delta_{};
    const float *simulation_delta_{};
    // FC-owned operands read by the two MULSS instructions immediately after update_inputs.
    float *values_{};

    fc::Game game_;

    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    hero_diagnostics::HeroDiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};
};

} // namespace

fc::Patch definition(fc::Role role) {
    fc::Patch patch{
        .id = "AerialGravity",
        .description = "Fixes hero aerial gravity to be the correct speed in both singleplayer and multiplayer.",
        .category = "Multiplayer",
        .enabled = true,
    };

    if (role == fc::Role::Client) {
        patch.variants.push_back(fc::variant<Handler>({"BF2.Steam.59EDE353"}, fc::Role::Client));
    }

    patch.variants.push_back(fc::variant<Handler>({"BF2.GOG.59EDF52B"}, role));

    return patch;
}

} // namespace fc_plugins::aerial_gravity
