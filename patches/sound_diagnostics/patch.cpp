#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::sound_diagnostics {

namespace {

// Supplies immutable settings and the launch role to plan-time observation selection.
template <fc::Role Role> struct RoleHandler : Handler {
    RoleHandler(fc::CreateContext, const Settings &settings) noexcept : Handler(settings, Role) {}
};

} // namespace

fc::Patch definition() {
    const auto schema = fc::settings<Settings>({fc::setting("Capture", &Settings::capture,
                                                            "Standard records sound causes, playback and output progress.\n"
                                                            "Full adds individual service iterations and detailed output timing.")
                                                    .choices({{"Standard", CaptureMode::Standard}, {"Full", CaptureMode::Full}})});
    return fc::Patch{
        .id = "SoundDiagnostics",
        .description = "Records native sound lifetimes, output service and device results for troubleshooting.",
        .category = "Diagnostics",
        .enabled = false,
        .settings = schema,
        .variants = {fc::variant<RoleHandler<fc::Role::Client>>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::sound_diagnostics
