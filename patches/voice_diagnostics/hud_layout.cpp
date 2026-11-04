#include "hud_layout.hpp"

namespace fc_plugins::voice_diagnostics {
namespace {

struct ProfileSites {
    std::string_view profile;
    std::uint32_t status, bind, update, add, lines, node, group_draw, text_draw, glyph_submit, glyph_target, render_gate;
    std::uint32_t local_index, spectator_index, menu, count, sending, permission;
    std::array<std::uint32_t, static_cast<std::size_t>(HudGlobal::Count)> globals;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .status = 0x251D40,
                 .bind = 0x2514F0,
                 .update = 0x251610,
                 .add = 0x251630,
                 .lines = 0x2517F0,
                 .node = 0x272FD0,
                 .group_draw = 0x2D6B20,
                 .text_draw = 0x2D8150,
                 .glyph_submit = 0x2DBA8C,
                 .glyph_target = 0x2B88F0,
                 .render_gate = 0x2BD0AD,
                 .local_index = 0x1B73B0,
                 .spectator_index = 0x1C3950,
                 .menu = 0x130ED0,
                 .count = 0x2749B0,
                 .sending = 0x274C40,
                 .permission = 0x274E70,
                 .globals = {0x1AAFD58, 0x1A30334, 0x1A30330, 0x1AAFB41, 0x1AAD47B, 0x3DE9C2, 0x4F6DB8, 0x3AF3FC, 0x4F6D9B, 0x4F6DF0}},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .status = 0x252DE0,
                 .bind = 0x252590,
                 .update = 0x2526B0,
                 .add = 0x2526D0,
                 .lines = 0x252890,
                 .node = 0x274070,
                 .group_draw = 0x2D7BC0,
                 .text_draw = 0x2D91F0,
                 .glyph_submit = 0x2DCB2C,
                 .glyph_target = 0x2B9980,
                 .render_gate = 0x2BE13D,
                 .local_index = 0x1B8360,
                 .spectator_index = 0x1C48E0,
                 .menu = 0x131C20,
                 .count = 0x275A50,
                 .sending = 0x275CE0,
                 .permission = 0x275F10,
                 .globals = {0x1AB11FC, 0x1A317D4, 0x1A317D0, 0x1AB0FF9, 0x1AAE932, 0x3DF9C2, 0x4F8258, 0x3B0374, 0x4F823B, 0x4F8290}}};

// Calls retain the exact retail target as evidence; instruction probes preserve all game registers.
HudLayout make_layout(const ProfileSites &s) {
    const auto call = [&](std::uint32_t target) {
        return fc::direct_call_to(fc::Location{std::string{s.profile}, target});
    };
    return {.create = {.rva = s.bind - 0x1B0, .name = "VoiceChatGroup constructor"},
            .status_update = {.rva = s.status, .name = "StatusDisplay::Update"},
            .bind = {.rva = s.bind, .name = "VoiceChatGroup::Bind"},
            .update = {.rva = s.update, .name = "VoiceChatGroup::Update"},
            .add_players = {.rva = s.add, .name = "VoiceChatGroup::AddPlayers"},
            .node_update = {.rva = s.node, .name = "VoiceChat::Node::Update outgoing audio"},
            .group_draw = {.rva = s.group_draw, .name = "RedInterfaceGroup::Draw"},
            .text_draw = {.rva = s.text_draw, .name = "RedInterfaceText::Draw"},
            .status_local = {.rva = s.status + 0xB, .name = "HUD local player", .evidence = call(s.local_index)},
            .status_spectated = {.rva = s.status + 0x13, .name = "HUD spectated player", .evidence = call(s.spectator_index)},
            .hud_local = {.rva = s.add + 0x29, .name = "voice HUD local player", .evidence = call(s.local_index)},
            .menu_hidden = {.rva = s.status + 0x82, .name = "HUD menu visibility", .evidence = call(s.menu)},
            .node_count = {.rva = s.add + 0x15, .name = "voice HUD node count", .evidence = call(s.count)},
            .sending = {.rva = s.add + 0xD2, .name = "voice HUD speaking query", .evidence = call(s.sending)},
            .permission = {.rva = s.add + 0xE8, .name = "voice HUD permission query", .evidence = call(s.permission)},
            // EDI is the validated local/spectated character at this branch.
            .character = {.rva = s.status + 0x5F, .name = "HUD character and visibility", .evidence = fc::exact_bytes({0x80, 0x3D})},
            // XMM1=voice clock, XMM0=last receive time, EDI=row, EDX=player.
            .timestamps = {.rva = s.lines + 0x79, .name = "voice HUD fade inputs", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x5C, 0xC8})},
            // Submission observes prepared glyphs; font inventory and mapping are not recorded.
            .glyph_submit = {.rva = s.glyph_submit, .name = "voice HUD glyph submission", .evidence = call(s.glyph_target)},
            .render_gate = {.rva = s.render_gate, .name = "interface render suppression", .evidence = fc::exact_bytes({0x80, 0x3D})},
            .globals = s.globals};
}

} // namespace

std::optional<HudLayout> select_hud_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (profile == sites.profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

} // namespace fc_plugins::voice_diagnostics
