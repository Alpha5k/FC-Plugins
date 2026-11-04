#include "patch.hpp"

#include "handler.hpp"

namespace fc_plugins::spectator_camera_smoothing {

fc::Patch definition() {
    return fc::Patch{.id = "SpectatorCameraSmoothing",
                     .description = "Smooth spectator camera movement and interpolated objects with two additional turns of delay.",
                     .category = "Multiplayer",
                     .enabled = false,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::spectator_camera_smoothing
