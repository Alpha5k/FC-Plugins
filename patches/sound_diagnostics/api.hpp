#pragma once

#include <fc/patch_api.hpp>
#include <cstdint>
#include <string_view>

namespace fc_plugins::sound_diagnostics {

// The continuity owner copies its actual sampled heights and flags; direction bits mark descent/ascent per foot.
struct FootMaintenance {
    std::uint32_t pose{};
    std::uint32_t bones[2]{};
    float old_heights[2]{};
    float sampled_heights[2]{};
    float result_heights[2]{};
    std::uint16_t flags_before{};
    std::uint16_t flags_after{};
    std::uint8_t support{};
    std::uint8_t directions[2]{};
    // 1 missing tracker, 2 eligible third-person interval, 3 maintained ineligible interval.
    std::uint8_t reason{};
};

// Describes the copied owner outcome, independent of native observer callback order.
inline fc::RecordDescription fc_description(std::type_identity<FootMaintenance>) {
    return fc::describe<FootMaintenance>("FootMaintenance.v1", &FootMaintenance::pose, &FootMaintenance::bones,
                                         &FootMaintenance::old_heights, &FootMaintenance::sampled_heights, &FootMaintenance::result_heights,
                                         &FootMaintenance::flags_before, &FootMaintenance::flags_after, &FootMaintenance::support,
                                         &FootMaintenance::directions, &FootMaintenance::reason);
}

// Lets sound patches report their completed foot-height maintenance to diagnostics.
struct SoundDiagnosticsApi {
    static constexpr std::string_view id = "SoundDiagnosticsApi.v3";
    // Tests capture/association availability before the producer constructs optional observation fields.
    fc::Function<std::uint8_t() noexcept> foot_maintenance_enabled;
    // Reports completed continuity maintenance on the native owner thread; payload and Soldier are borrowed.
    fc::Function<void(void *soldier, const FootMaintenance *outcome) noexcept> foot_maintenance;
};

// Describes the optional API for foot-height observations.
inline fc::ApiContract<SoundDiagnosticsApi> api_contract() {
    return fc::api<SoundDiagnosticsApi>(SoundDiagnosticsApi::id, &SoundDiagnosticsApi::foot_maintenance_enabled,
                                        &SoundDiagnosticsApi::foot_maintenance);
}

} // namespace fc_plugins::sound_diagnostics
