#include "patch.hpp"
#include "handler.hpp"

namespace fc_plugins::voice_chat {
namespace {

// Adds volume and speaker HUD corrections for normal game launches, including player hosts.
class ClientHandler final : public Handler {
  public:
    using Handler::Handler;

    void plan(fc::Plan &plan) {
        Handler::plan(plan, true);
    }
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "VoiceChat",
                     .description = "Restores working in-game voice chat using the Opus codec. Requires accompanying mod.",
                     .category = "Social",
                     .enabled = true,
                     .depends_on = {"core.lua_bindings"},
                     .variants = {fc::variant<ClientHandler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client),
                                  fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Server)}};
}

} // namespace fc_plugins::voice_chat
