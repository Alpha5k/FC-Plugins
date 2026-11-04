#include "handler.hpp"

namespace fc_plugins::multiplayer_gc {

fc::Result<> Handler::bind_chat(fc::lua::Bindings &bindings) {
    if (auto result = bindings.bind("ScriptCB_SetMetagameChatActive",
                                    [this](fc::lua::Frame &frame) {
                                        const auto active = frame.argument(0).as_bool();
                                        if (!active) {
                                            return_result(frame, std::unexpected(active.error()));
                                        } else if (*active) {
                                            return_result(frame, shell_.open_chat());
                                        } else {
                                            shell_.close_chat();
                                            frame.return_values({frame.value(true)});
                                        }
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_UpdateMetagameChat",
                                    [this](fc::lua::Frame &frame) {
                                        const auto dt = frame.argument(0).as_number();
                                        if (dt) {
                                            shell_.update_chat(static_cast<float>(*dt));
                                        }
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_CloseMetagameChatInput",
                                    [this](fc::lua::Frame &) {
                                        shell_.close_editor();
                                    });
        !result) {
        return result;
    }
    if (auto result = bindings.bind("ScriptCB_GetMetagameInput",
                                    [this](fc::lua::Frame &frame) {
                                        const auto input = shell_.input();
                                        if (!input) {
                                            frame.return_values({frame.nil()});
                                            return;
                                        }
                                        auto table = frame.table();
                                        table.set("player_list", frame.value(input->player_list));
                                        table.set("consumed", frame.value(input->consumed));
                                        table.set("chat_open", frame.value(input->chat_open));
                                        frame.return_values({table});
                                    });
        !result) {
        return result;
    }
    return {};
}

} // namespace fc_plugins::multiplayer_gc
