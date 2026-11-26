#include "handler.hpp"

#include <cmath>
#include <utility>

namespace fc_plugins::multiplayer_gc {
namespace {

// Reads IDs and native item indices without truncating fractional Lua numbers.
std::optional<std::uint32_t> integer(fc::lua::Frame &frame, std::size_t index) {
    const auto value = frame.argument(index).as_number();
    if (!value || !(*value >= 0 && *value <= UINT32_MAX) || std::trunc(*value) != *value) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

fc::Result<BattleRoles> battle_roles(fc::lua::Value table) {
    if (table.type() != fc::lua::Type::Table || table.has_metatable()) {
        return std::unexpected(fc::Error{"Supply a plain table of battle roles keyed by native player slot."});
    }
    BattleRoles roles;
    const auto result = table.for_each([&](fc::lua::Value key, fc::lua::Value value) {
        const auto slot = key.as_number();
        const auto role = value.as_number();
        return slot && role && roles.set(*slot, *role);
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!*result) {
        return std::unexpected(fc::Error{"Battle roles must use integer slots 0 to 63 and role 1 or 2."});
    }
    return roles;
}

} // namespace

void Handler::update(const fc::UpdateContext &) noexcept {
    session_.update();
}

fc::Result<> Handler::bind_campaign(fc::lua::Bindings &bindings) {
    if (auto result = bindings.bind("ScriptCB_SetMetagameTurnTimer",
                                    [this](fc::lua::Frame &frame) {
                                        const auto minutes = integer(frame, 0);
                                        if (!minutes || *minutes > 10) {
                                            return_result(frame,
                                                          std::unexpected(fc::Error{"Choose Off or a Turn Timer from 1 to 10 minutes."}));
                                            return;
                                        }
                                        session_.set_turn_timer(static_cast<std::uint8_t>(*minutes));
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_GetMetagameTurnTimer",
                                    [this](fc::lua::Frame &frame) {
                                        frame.return_values({frame.value(session_.turn_timer())});
                                    });
        !result) {
        return result;
    }
    // Stock metagame entry sets the host's idle timer to zero; keep the ordinary multiplayer statistics interval.
    if (auto result = bindings.bind({"ifs_teamstats", "Enter"}, fc::lua::BindingMode::Wrap,
                                    [this](fc::lua::Frame &frame) {
                                        const auto result = frame.original();
                                        if (!result) {
                                            frame.error(result.error().message);
                                            return;
                                        }
                                        if (transition_.host_mission()) {
                                            auto screen = frame.argument(0);
                                            screen.set("fCurIdleTime", screen.get("fMAX_IDLE_TIME"));
                                        }
                                        frame.return_values(result->values());
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind(
            "ScriptCB_SetMetagameDecision",
            [this](fc::lua::Frame &frame) {
                const auto faction = integer(frame, 0);
                const auto turn = integer(frame, 1);
                const auto screen = frame.argument(2).as_string();
                const auto card = integer(frame, 3);
                const auto phase = frame.argument(4).as_string();
                const auto source = frame.argument(5).as_string();
                const auto remaining = integer(frame, 6);
                const auto bonus_mask = integer(frame, 7);
                const auto timer_active = frame.argument(8).as_bool();
                if (frame.argument_count() != 9 || !timer_active || !faction || *faction < 1 || *faction > 2 || !turn || !screen || !card ||
                    !phase || !valid_presentation_phase(*phase) || !source || !remaining || !bonus_mask || *bonus_mask > 3) {
                    return_result(frame,
                                  std::unexpected(fc::Error{"Supply the current decision, pending purchase and fleet presentation."}));
                    return;
                }
                const auto state = capture();
                if (!state) {
                    return_result(frame, std::unexpected(state.error()));
                    return;
                }
                session_.set_decision({.turn = *turn,
                                       .faction = static_cast<std::uint8_t>(*faction),
                                       .screen = std::string{*screen},
                                       .pending_card = *card,
                                       .presentation_phase = std::string{*phase},
                                       .presentation_source = std::string{*source},
                                       .presentation_remaining_ms = *remaining,
                                       .bonus_pending_mask = static_cast<std::uint8_t>(*bonus_mask),
                                       .strategic_timer_active = *timer_active},
                                      *state);
                frame.return_values({frame.value(true)});
            });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_SubmitMetagameAction",
                                    [this](fc::lua::Frame &frame) {
                                        const auto decision = integer(frame, 0);
                                        const auto name = frame.argument(1).as_string();
                                        const auto first = frame.argument(2).as_string();
                                        const auto second = frame.argument(3).as_string();
                                        const auto index = integer(frame, 4);
                                        const auto confirm = frame.argument(5).as_bool();
                                        if (frame.argument_count() != 6 || !decision || !name || !first || !second || !index || !confirm) {
                                            return_result(frame, std::unexpected(
                                                                     fc::Error{"Supply the campaign decision and the action's choices."}));
                                            return;
                                        }
                                        return_result(frame, session_.request_action({.decision = *decision,
                                                                                      .name = std::string{*name},
                                                                                      .first = std::string{*first},
                                                                                      .second = std::string{*second},
                                                                                      .index = *index,
                                                                                      .confirm = *confirm}));
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_CompleteMetagameAction",
                                    [this](fc::lua::Frame &frame) {
                                        const auto token = integer(frame, 0);
                                        const auto accepted = frame.argument(1).as_bool();
                                        const auto message = frame.argument(2).as_string();
                                        if (!token || !accepted || !message) {
                                            return_result(frame, std::unexpected(fc::Error{"Supply the completed action and its result."}));
                                            return;
                                        }
                                        session_.action_result(*token, *accepted, std::string{*message});
                                        frame.return_values({frame.value(true)});
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind(
            "ScriptCB_PrepareMetagameBattle",
            [this](fc::lua::Frame &frame) {
                if (frame.argument_count() != 0) {
                    return_result(frame, std::unexpected(fc::Error{"Battle preparation takes the already saved campaign state."}));
                    return;
                }
                const auto state = capture();
                if (!state) {
                    return_result(frame, std::unexpected(state.error()));
                    return;
                }
                return_result(frame, session_.prepare_battle({.state = *state}));
            });
        !result) {
        return result;
    }
    return bindings.bind("ScriptCB_SetMetagameBattleReady", [this](fc::lua::Frame &frame) {
        const auto arguments = frame.argument_count();
        const auto id = integer(frame, 0);
        const auto first_side = frame.argument(1).as_string();
        const auto second_side = frame.argument(2).as_string();
        if ((arguments != 3 && arguments != 4) || !first_side || first_side->empty() || !second_side || second_side->empty()) {
            return_result(frame, std::unexpected(fc::Error{"Supply the applied battle setup and loaded faction side codes."}));
            return;
        }
        if (!id || !pending_setup_ || pending_setup_->id != *id || (arguments == 4 && session_.current_battle_setup() != *id)) {
            return_result(frame, std::unexpected(fc::Error{"This battle setup is no longer current."}));
            return;
        }
        std::optional<BattleRoles> roles;
        if (arguments == 4) {
            const auto parsed = battle_roles(frame.argument(3));
            if (!parsed) {
                return_result(frame, std::unexpected(parsed.error()));
                return;
            }
            roles = *parsed;
        }
        if (auto result = transition_.prepare(*id, *first_side, *second_side, roles); !result) {
            return_result(frame, result);
            return;
        }
        session_.battle_ready(*id);
        pending_setup_.reset();
        frame.return_values({frame.value(true)});
    });
}

} // namespace fc_plugins::multiplayer_gc
