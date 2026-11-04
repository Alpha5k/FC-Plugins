#include "handler.hpp"

#include <cmath>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fc_plugins::multiplayer_gc {
namespace {

// Copies both Lua byte strings before their frame expires.
fc::Result<State> state_arguments(fc::lua::Frame &frame) {
    if (frame.argument_count() != 2) {
        return std::unexpected(fc::Error{"Supply the captured campaign and mission strings."});
    }
    const auto campaign = frame.argument(0).as_string();
    const auto mission = frame.argument(1).as_string();
    if (!campaign || !mission) {
        return std::unexpected(fc::Error{"Campaign and mission data must be strings."});
    }
    return State{std::string{*campaign}, std::string{*mission}};
}

// Copies supplied browser fields and retains fields omitted by an options-only update.
fc::Result<Summary> summary_arguments(fc::lua::Value table, Summary summary = {}) {
    if (table.type() != fc::lua::Type::Table) {
        return std::unexpected(fc::Error{"Supply a campaign summary table."});
    }
    for (const auto &[name, target] :
         std::array{std::pair{"campaign", &summary.campaign}, std::pair{"era", &summary.era}, std::pair{"factions", &summary.factions},
                    std::pair{"victoryCondition", &summary.victory_condition}, std::pair{"difficulty", &summary.difficulty},
                    std::pair{"map", &summary.map}}) {
        const auto field = table.get(name);
        if (field.type() == fc::lua::Type::Nil) {
            continue;
        }
        const auto value = field.as_string();
        const auto number = field.as_number();
        if (value) {
            *target = *value;
        } else if (std::string_view{name} == "campaign" && number) {
            *target = std::format("{:g}", *number);
        } else {
            return std::unexpected(fc::Error{"Campaign summary fields must be strings."});
        }
    }
    const auto turn = table.get("turn");
    if (turn.type() != fc::lua::Type::Nil) {
        const auto value = turn.as_number();
        if (!value || !(*value >= 0 && *value <= UINT32_MAX) || std::trunc(*value) != *value) {
            return std::unexpected(fc::Error{"The campaign turn must be a whole number."});
        }
        summary.turn = static_cast<std::uint32_t>(*value);
    }
    const auto damage = table.get("teamDamage");
    if (damage.type() != fc::lua::Type::Nil) {
        const auto value = damage.as_bool();
        if (!value) {
            return std::unexpected(fc::Error{"Team Damage must be On or Off."});
        }
        summary.team_damage = *value;
    }
    const auto maximum = table.get("maximum");
    if (maximum.type() != fc::lua::Type::Nil) {
        const auto value = maximum.as_number();
        if (!value || !(*value >= 2 && *value <= 16) || std::trunc(*value) != *value) {
            return std::unexpected(fc::Error{"Max Players must be a whole number from 2 to 16."});
        }
        summary.maximum = static_cast<std::uint8_t>(*value);
    }
    return summary;
}

} // namespace

// Returns true on success, or nil and the operation's error.
void Handler::return_result(fc::lua::Frame &frame, const fc::Result<> &result) {
    if (result) {
        frame.return_values({frame.value(true)});
    } else {
        frame.return_values({frame.nil(), frame.value(result.error().message)});
    }
}

void Handler::plan(fc::Plan &input) {
    auto plan = input.for_image("game");
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Absolute operands must refer to the selected buffers and length fields after image relocation.
    const auto qualify_save = [&plan](const fc::CodeLocation &save, std::uint32_t buffer, std::uint32_t length) {
        plan.check(save.rva, 6, save.evidence);
        plan.check(save.rva + 6, 4, fc::points_to(plan.at(buffer)));
        plan.check(save.rva + 15, 1, fc::exact_bytes({0xA3}));
        plan.check(save.rva + 16, 4, fc::points_to(plan.at(length)));
    };
    qualify_save(layout->campaign_save, layout->campaign.rva, layout->campaign_length.rva);
    qualify_save(layout->mission_save, layout->mission.rva, layout->mission_length.rva);
    campaign_ = plan.access(layout->campaign);
    campaign_length_ = plan.access(layout->campaign_length);
    mission_ = plan.access(layout->mission);
    mission_length_ = plan.access(layout->mission_length);
    shell_.plan(plan);
    transition_.plan(plan);
    discovery_.plan(input);
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    auto bindings = fc::lua::get(context);
    if (!bindings) {
        return std::unexpected(bindings.error());
    }
    if (auto result = shell_.prepare(); !result) {
        return result;
    }
    if (auto result = session_.prepare(context, shell_, transition_); !result) {
        return result;
    }
    if (auto result = bind_session(*bindings); !result) {
        return result;
    }

    if (auto result = bind_campaign(*bindings); !result) {
        return result;
    }
    if (auto result = bind_browser(*bindings); !result) {
        return result;
    }
    if (auto result = bind_chat(*bindings); !result) {
        return result;
    }

    if (auto result = bindings->initialize([this](fc::lua::Frame &) {
            shell_.reset_chat();
            pending_setup_.reset();
        });
        !result) {
        return result;
    }

    // Each command runs synchronously on the invoking Lua VM's owning thread.
    if (auto result = bindings->bind("ScriptCB_RestoreMetagameState",
                                     [this](fc::lua::Frame &frame) {
                                         const auto state = state_arguments(frame);
                                         return_result(frame, state ? restore(*state) : fc::Result<>{std::unexpected(state.error())});
                                     });
        !result) {
        return result;
    }
    return {};
}

fc::Result<Handler::Storage> Handler::storage() const {
    auto campaign = campaign_.view();
    if (!campaign) {
        return std::unexpected(campaign.error());
    }
    auto campaign_length = campaign_length_.view();
    if (!campaign_length) {
        return std::unexpected(campaign_length.error());
    }
    auto mission = mission_.view();
    if (!mission) {
        return std::unexpected(mission.error());
    }
    auto mission_length = mission_length_.view();
    if (!mission_length) {
        return std::unexpected(mission_length.error());
    }
    return Storage{*campaign, *campaign_length, *mission, *mission_length};
}

fc::Result<State> Handler::capture() const {
    const auto native = storage();
    if (!native) {
        return std::unexpected(native.error());
    }
    return capture_state(native->campaign, native->campaign_length[0], native->mission, native->mission_length[0]);
}

fc::Result<> Handler::restore(const State &state) {
    const auto native = storage();
    if (!native) {
        return std::unexpected(native.error());
    }
    return install_state(native->campaign, native->campaign_length[0], native->mission, native->mission_length[0], state);
}

fc::Result<> Handler::bind_session(fc::lua::Bindings &bindings) {
    if (auto result = bindings.bind(
            "ScriptCB_BeginMPMetagame",
            [this](fc::lua::Frame &frame) {
                const auto cooperative = frame.argument(0).as_bool();
                const auto faction = frame.argument(1).as_number();
                const auto summary = frame.argument(2);
                if (!cooperative || !faction || (*faction != 1 && *faction != 2) || summary.type() != fc::lua::Type::Table) {
                    return_result(frame, std::unexpected(fc::Error{"Supply campaign mode, host faction and campaign summary."}));
                    return;
                }
                const auto fields = summary_arguments(summary);
                if (!fields || fields->campaign.empty() || fields->era.empty() || !fields->maximum ||
                    (*cooperative && fields->maximum > 8)) {
                    return_result(frame, std::unexpected(fc::Error{"The campaign summary is incomplete."}));
                    return;
                }
                const auto state = capture();
                if (!state) {
                    return_result(frame, std::unexpected(state.error()));
                    return;
                }
                return_result(frame, session_.host_campaign(*state, *cooperative, static_cast<std::uint8_t>(*faction), *fields));
            });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_SetMetagameSummary",
                                    [this](fc::lua::Frame &frame) {
                                        const auto summary = summary_arguments(frame.argument(0), session_.summary());
                                        if (!summary) {
                                            return_result(frame, std::unexpected(summary.error()));
                                            return;
                                        }
                                        session_.update_summary(*summary);
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_SetMPMetagameReady",
                                    [this](fc::lua::Frame &frame) {
                                        session_.shell_ready();
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind(
            "ScriptCB_SetMPMetagameTeam",
            [this](fc::lua::Frame &frame) {
                const auto faction = frame.argument(0).as_number();
                return_result(frame, faction && (*faction == 1 || *faction == 2)
                                         ? session_.choose(static_cast<std::uint8_t>(*faction))
                                         : fc::Result<>{std::unexpected(fc::Error{"Choose one of this campaign's factions."})});
            });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_EndMPMetagame",
                                    [this](fc::lua::Frame &frame) {
                                        transition_.disarm();
                                        shell_.close_chat();
                                        session_.leave();
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    return bindings.bind("ScriptCB_UpdateMPMetagame", [this](fc::lua::Frame &frame) {
        poll(frame);
    });
}

void Handler::poll(fc::lua::Frame &frame) {
    const auto peek = frame.argument_count() && frame.argument(0).as_bool().value_or(false);
    if (!peek) {
        shell_.capture_names();
        transition_.update();
    }
    auto state = session_.poll(peek);
    state.returned = state.returned || transition_.shell();
    if (auto result = transition_.check(); !result) {
        state.phase = "error";
        state.message = result.error().message;
    }
    auto table = frame.table();
    table.set("phase", frame.value(state.phase));
    table.set("host", frame.value(state.host));
    table.set("cooperative", frame.value(state.snapshot.cooperative));
    table.set("hostFaction", frame.value(state.snapshot.host_faction));
    table.set("localFaction", frame.value(state.snapshot.local_faction));
    auto players = frame.table();
    for (std::size_t i = 0; i < state.snapshot.players.size(); ++i) {
        const auto &player = state.snapshot.players[i];
        auto row = frame.table();
        row.set("name", frame.value(player.name));
        row.set("slot", frame.value(player.slot));
        row.set("faction", frame.value(player.faction));
        row.set("host", frame.value(player.host));
        players.set(i + 1, row);
    }
    table.set("participants", players);
    if (!state.message.empty()) {
        table.set("message", frame.value(state.message));
    }
    if (state.pending) {
        table.set("campaign", frame.value(state.pending->campaign));
        table.set("mission", frame.value(state.pending->mission));
    }
    table.set("localSlot", frame.value(state.local_slot));
    table.set("decisionId", frame.value(state.decision.id));
    table.set("decisionFaction", frame.value(state.decision.faction));
    table.set("decisionScreen", frame.value(state.decision.screen));
    table.set("pendingCard", frame.value(state.decision.pending_card));
    table.set("presentationPhase", frame.value(state.decision.presentation_phase));
    table.set("presentationSource", frame.value(state.decision.presentation_source));
    table.set("presentationRemainingMs", frame.value(state.decision.presentation_remaining_ms));
    table.set("timerEnabled", frame.value(state.decision.timer_enabled));
    table.set("timerRemainingMs", frame.value(state.decision.timer_remaining_ms));
    table.set("timerExpired", frame.value(state.timer_expired));
    table.set("bonusPendingMask", frame.value(state.decision.bonus_pending_mask));
    table.set("bonusRemainingMs", frame.value(state.decision.bonus_remaining_ms));
    table.set("bonusExpired", frame.value(state.bonus_expired));
    auto commanders = frame.table();
    commanders.set(1, frame.value(state.decision.commanders[0]));
    commanders.set(2, frame.value(state.decision.commanders[1]));
    table.set("commanders", commanders);
    table.set("returned", frame.value(state.returned));
    table.set("launch", frame.value(state.launch));
    table.set("actionDone", frame.value(state.action_done));
    table.set("actionAccepted", frame.value(state.action_accepted));
    table.set("actionMessage", frame.value(state.action_message));
    if (state.action) {
        auto action = frame.table();
        action.set("token", frame.value(state.action->token));
        action.set("id", frame.value(state.action->decision));
        action.set("faction", frame.value(state.action->faction));
        action.set("name", frame.value(state.action->name));
        action.set("first", frame.value(state.action->first));
        action.set("second", frame.value(state.action->second));
        action.set("index", frame.value(state.action->index));
        action.set("confirm", frame.value(state.action->confirm));
        table.set("action", action);
    }
    if (state.setup) {
        if (!peek) {
            pending_setup_ = state.setup;
        }
        table.set("battleSetup", frame.value(true));
        table.set("setupId", frame.value(state.setup->id));
        table.set("campaign", frame.value(state.setup->state.campaign));
        table.set("mission", frame.value(state.setup->state.mission));
    }
    frame.return_values({table});
}

} // namespace fc_plugins::multiplayer_gc
