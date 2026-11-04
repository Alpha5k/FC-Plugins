#include "patch.hpp"

#include "client/handler.hpp"
#include "server/handler.hpp"

#include <utility>

namespace fc_plugins::direct_transport {

fc::Patch definition(fc::Role role) {
    fc::Patch patch{
        .id = "DirectTransport",
        .description = "Restores direct UDP connections instead of using GalaxyPeer for game traffic.",
        .category = "Networking",
        .enabled = true,
    };

    if (role == fc::Role::Client) {
        patch.variants = {fc::variant<client::Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)};
    } else {
        // The dedicated GOG handler waits for a supported GalaxyPeer image to observe the public address used in Offers.
        patch.variants = {
            fc::variant<server::Handler>({fc::ImageRequirement{"game", {"BF2.GOG.59EDF52B"}},
                                          fc::ImageRequirement{"galaxy_peer", {"GalaxyPeer.59E6304A", "GalaxyPeer.5BBE22A6"}}},
                                         fc::Role::Server)
                .failure_policy(fc::FailurePolicy::Fatal)
                .settings(fc::settings<server::Settings>({fc::setting("Policy", &server::Settings::policy)
                                                              .environment("BF2_DIRECT_POLICY")
                                                              .choices({{"Disabled", Policy::Disabled},
                                                                        {"PreferDirect", Policy::PreferDirect},
                                                                        {"RequireDirectPatched", Policy::RequireDirectPatched},
                                                                        {"RequireDirectAll", Policy::RequireDirectAll}})})),
        };
    }

    return patch;
}

} // namespace fc_plugins::direct_transport
