#include "handler.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace fc_plugins::multiplayer_gc {

fc::Result<> Handler::bind_browser(fc::lua::Bindings &bindings) {
    if (auto result = bindings.bind("ScriptCB_SetMetagameBrowser",
                                    [this](fc::lua::Frame &frame) {
                                        const auto active = frame.argument(0).as_bool();
                                        if (!active) {
                                            return_result(frame, std::unexpected(active.error()));
                                            return;
                                        }
                                        discovery_.select(*active);
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_SetMetagameFilters",
                                    [this](fc::lua::Frame &frame) {
                                        const auto map = frame.argument(0).as_string();
                                        const auto mode = frame.argument(1).as_string();
                                        const auto era = frame.argument(2).as_string();
                                        if (frame.argument_count() != 3 || !map || !mode || !era) {
                                            return_result(frame, std::unexpected(fc::Error{"Supply map, mode and era filter strings."}));
                                            return;
                                        }
                                        discovery_.filters({std::string{*map}, std::string{*mode}, std::string{*era}});
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_GetMetagameSessionInfo",
                                    [this](fc::lua::Frame &frame) {
                                        std::optional<std::int32_t> index;
                                        if (frame.argument_count() != 0) {
                                            const auto row = frame.argument(0).as_number();
                                            // ScriptCB_GetSessionList writes a one-based native ordinal in sessionIdx.
                                            if (!row || !(*row >= 1 && *row <= 2000) || std::trunc(*row) != *row) {
                                                frame.return_values({frame.nil()});
                                                return;
                                            }
                                            index = static_cast<std::int32_t>(*row) - 1;
                                        }
                                        const auto info = discovery_.session_info(index);
                                        if (!info) {
                                            frame.return_values({frame.nil()});
                                            return;
                                        }
                                        auto table = frame.table();
                                        table.set("map", frame.value(info->map));
                                        table.set("mode", frame.value(info->cooperative ? "Co-op" : "Versus"));
                                        table.set("era", frame.value(info->era));
                                        table.set("campaign", frame.value(info->campaign));
                                        table.set("turn", frame.value(info->turn));
                                        table.set("hostFaction", frame.value(info->host_faction));
                                        table.set("count", frame.value(info->count));
                                        table.set("maximum", frame.value(info->maximum));
                                        table.set("factions", frame.value(info->factions));
                                        table.set("victoryCondition", frame.value(info->victory_condition));
                                        table.set("difficulty", frame.value(info->difficulty));
                                        table.set("teamDamage", frame.value(info->team_damage));
                                        table.set("joinable", frame.value(info->joinable));
                                        frame.return_values({table});
                                    });
        !result) {
        return result;
    }
    return {};
}

} // namespace fc_plugins::multiplayer_gc
