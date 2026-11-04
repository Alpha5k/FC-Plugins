#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::hero_diagnostics {
namespace {

// Supplies the native role selected by the runtime variant.
template <fc::Role Role> struct RoleHandler : Handler {
    RoleHandler(fc::CreateContext, const Settings &settings) noexcept : Handler(settings, Role) {}
};

} // namespace

fc::Patch definition() {
    const auto schema = fc::settings<Settings>(
        {fc::setting("Capture", &Settings::capture,
                     "Standard records input use, authority, completed motion and body/camera output.\n"
                     "Combat adds action decisions and boundary detail. Full adds native operands and timing.")
             .choices({{"Standard", CaptureMode::Standard}, {"Combat", CaptureMode::Combat}, {"Full", CaptureMode::Full}})});

    return fc::Patch{.id = "HeroDiagnostics",
                     .description = "Records melee state, movement, animation, network activity, and input data for troubleshooting.",
                     .category = "Diagnostics",
                     .enabled = false,
                     .settings = schema,
                     .variants = {fc::variant<RoleHandler<fc::Role::Client>>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client),
                                  fc::variant<RoleHandler<fc::Role::Server>>({"BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::hero_diagnostics
