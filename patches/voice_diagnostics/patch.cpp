#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::voice_diagnostics {

namespace {

// Supplies immutable settings and the launch role to plan-time observation selection.
template <fc::Role Role> struct RoleHandler : Handler {
    RoleHandler(fc::CreateContext, const Settings &settings) noexcept : Handler(settings, Role) {}
};

} // namespace

fc::Patch definition() {
    const auto schema = fc::settings<Settings>({fc::setting("Capture", &Settings::capture,
                                                            "Standard records voice delivery, routing and HUD activity.\n"
                                                            "Full adds individual queue entries and text drawing.")
                                                    .choices({{"Standard", CaptureMode::Standard}, {"Full", CaptureMode::Full}})});
    return fc::Patch{.id = "VoiceDiagnostics",
                     .description = "Records native voice routing, codec and jitter decisions for troubleshooting.",
                     .category = "Diagnostics",
                     .enabled = false,
                     .settings = schema,
                     .variants = {fc::variant<RoleHandler<fc::Role::Client>>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client),
                                  fc::variant<RoleHandler<fc::Role::Server>>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::voice_diagnostics
