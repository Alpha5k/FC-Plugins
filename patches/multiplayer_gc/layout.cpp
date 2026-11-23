#include "layout.hpp"

#include <array>

namespace fc_plugins::multiplayer_gc {
namespace {

// RVAs come from the retail save/load callbacks at the preferred image base 0x00400000.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t campaign;
    std::uint32_t campaign_length;
    std::uint32_t mission;
    std::uint32_t mission_length;
    std::uint32_t campaign_save;
    std::uint32_t mission_save;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .campaign = 0x01B9C8D0,
                 .campaign_length = 0x01B9C7C8,
                 .mission = 0x01B9CCD0,
                 .mission_length = 0x01B9C7CC,
                 .campaign_save = 0x0018A1A0,
                 .mission_save = 0x0018A310},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .campaign = 0x01B9DD80,
                 .campaign_length = 0x01B9DC7C,
                 .mission = 0x01B9E188,
                 .mission_length = 0x01B9E180,
                 .campaign_save = 0x0018B150,
                 .mission_save = 0x0018B2C0},
};

// Lua registration tables identify SaveMissionSetup separately from the adjacent SaveCampaignState.
// Both selected saves push capacity, set EDX to storage and store SaveLuaItems' EAX length.
Layout make_layout(const ProfileSites &sites) {
    return {
        .campaign = {.rva = sites.campaign, .name = "compressed campaign state"},
        // Reset/IsMetagameStateSaved and LoadMetagameState use this same length as the saved-state flag.
        .campaign_length = {.rva = sites.campaign_length, .name = "campaign state length"},
        .mission = {.rva = sites.mission, .name = "compressed mission setup"},
        .mission_length = {.rva = sites.mission_length, .name = "mission setup length"},
        .campaign_save = {.rva = sites.campaign_save,
                          .name = "SaveMetagameState buffer capacity",
                          .evidence = fc::exact_bytes({0x68, 0x00, 0x02, 0x00, 0x00, 0xBA})},
        .mission_save = {.rva = sites.mission_save,
                         .name = "SaveMissionSetup buffer capacity",
                         .evidence = fc::exact_bytes({0x68, 0x00, 0x01, 0x00, 0x00, 0xBA})},
    };
}

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

std::optional<ShellLayout> select_shell_layout(std::string_view profile) {
    struct ShellProfile {
        std::string_view profile;
        ShellLayout layout;
    };

    // Preferred-base VAs from the separately inspected retail functions, expressed here as RVAs.
    static const std::array kShellProfiles{
        ShellProfile{.profile = "BF2.Steam.59EDE353",
                     .layout = {.write_move = 0x1E5BB0,
                                .read_move = 0x1E5D10,
                                .write_update = 0x1E5FC0,
                                .read_update = 0x1E6690,
                                .queue_chat = 0x1D13C0,
                                .send_server = 0x1D1820,
                                .send_client = 0x1D19B0,
                                .display = 0x1D1570,
                                .player_name = 0x1C36A0,
                                .local_slot = 0x1B73B0,
                                .read_bytes = 0x1D9AE0,
                                .resolve_packet = 0x1D8F00,
                                .destination = 0x1BA8778,
                                .coverage = 0x1BA6598,
                                .host_queue = 0x1ACB25C,
                                .remove_node = 0x1D3AE0,
                                .free_record = 0x2C3560,
                                .client_due = 0x1DFFB7,
                                .client_queue_count = 0x1AE7624,
                                .host_send_updates = 0x1DCF90,
                                .publication_hidden = 0x1D726C,
                                .publication_evidence = fc::exact_bytes({0xE8, 0x3F, 0x7C, 0xF5, 0xFF}),
                                .search_filter = 0x1D5EDB,
                                .reader_cancelled = 0x1D58E7,
                                .reader_count = 0x1D59A2,
                                .reader_remove_advance = 0x1DEDB4,
                                .input_manager_reference = 0x17F77C,
                                .input_manager = 0x1ABDC50,
                                .input_getter = 0x12A1C0,
                                .input_flags_read = 0x17F789,
                                .browser_map_gate = 0x1DD7AB,
                                .browser_resort = 0x194300,
                                .browser_sort = 0x3E7378,
                                .browser_direction = 0x3E69CA,
                                .browser_select = 0x1DDB40,
                                .browser_sort_reference = 0x193B80,
                                .browser_direction_reference = 0x194823,
                                .chat = {.input_process = 0x15750,
                                         .input_gate = 0x152B1,
                                         .shell_gate = 0x15991,
                                         .shell_state = 0x3E8007,
                                         .submit = 0x15B4D,
                                         .keyboard_event = 0x2EFEB0,
                                         .keyboard_update = 0x2C6590,
                                         .key_map = 0x387F00,
                                         .event_key_map = 0x560E10,
                                         .display_name = 0x1D1691,
                                         .display_team = 0x1D1742,
                                         .copy_name = 0x3273D0,
                                         .strip_create = 0x546B0,
                                         .strip_init = 0x54850,
                                         .strip_activate = 0x54AC0,
                                         .strip_delete = 0x547A0,
                                         .strip_hide = 0x54AD0,
                                         .strip_global = 0x1A30340,
                                         .strip_position = 0x2C0850,
                                         .hud_update = 0x1658E0,
                                         .hud_group_reference = 0x1655D8,
                                         .hud_group = 0x1A573C8,
                                         .messages_create = 0x1A38E0,
                                         .messages_destroy = 0x1A3920,
                                         .messages_global = 0x1A579BC,
                                         .message_event = 0x1637E0}}},
        ShellProfile{.profile = "BF2.GOG.59EDF52B",
                     .layout = {.write_move = 0x1E6C50,
                                .read_move = 0x1E6DB0,
                                .write_update = 0x1E7060,
                                .read_update = 0x1E7730,
                                .queue_chat = 0x1D2340,
                                .send_server = 0x1D27A0,
                                .send_client = 0x1D2930,
                                .display = 0x1D24F0,
                                .player_name = 0x1C4630,
                                .local_slot = 0x1B8360,
                                .read_bytes = 0x1DAB90,
                                .resolve_packet = 0x1D9FB0,
                                .destination = 0x1BA9C2C,
                                .coverage = 0x1BA7A50,
                                .host_queue = 0x1ACCE5C,
                                .remove_node = 0x1D4A60,
                                .free_record = 0x2C45F0,
                                .client_due = 0x1E1067,
                                .client_queue_count = 0x1AE69DC,
                                .host_send_updates = 0x1DE040,
                                .publication_hidden = 0x1D831C,
                                .publication_evidence = fc::exact_bytes({0xE8, 0xEF, 0x63, 0xF5, 0xFF}),
                                .search_filter = 0x1D6EAB,
                                .reader_cancelled = 0x1D68B7,
                                .reader_count = 0x1D6972,
                                .reader_remove_advance = 0x1DFE64,
                                .input_manager_reference = 0x18050C,
                                .input_manager = 0x1ABF100,
                                .input_getter = 0x12A1C0,
                                .input_flags_read = 0x180519,
                                .browser_map_gate = 0x1DE85B,
                                .browser_resort = 0x1952B0,
                                .browser_sort = 0x3E8378,
                                .browser_direction = 0x3E79CA,
                                .browser_select = 0x1DEBF0,
                                .browser_sort_reference = 0x194B20,
                                .browser_direction_reference = 0x1957D3,
                                .chat = {.input_process = 0x15710,
                                         .input_gate = 0x15271,
                                         .shell_gate = 0x15951,
                                         .shell_state = 0x3E9007,
                                         .submit = 0x15B0D,
                                         .keyboard_event = 0x2F0F70,
                                         .keyboard_update = 0x2C7630,
                                         .key_map = 0x388EB0,
                                         .event_key_map = 0x5622B0,
                                         .display_name = 0x1D2611,
                                         .display_team = 0x1D26C2,
                                         .copy_name = 0x3284A0,
                                         .strip_create = 0x54690,
                                         .strip_init = 0x54830,
                                         .strip_activate = 0x54AA0,
                                         .strip_delete = 0x54780,
                                         .strip_hide = 0x54AB0,
                                         .strip_global = 0x1A317E0,
                                         .strip_position = 0x2C18E0,
                                         .hud_update = 0x166660,
                                         .hud_group_reference = 0x166358,
                                         .hud_group = 0x1A58878,
                                         .messages_create = 0x1A4890,
                                         .messages_destroy = 0x1A48D0,
                                         .messages_global = 0x1A58E6C,
                                         .message_event = 0x164560}}},
    };
    for (const auto &sites : kShellProfiles) {
        if (sites.profile == profile) {
            return sites.layout;
        }
    }
    return std::nullopt;
}

} // namespace fc_plugins::multiplayer_gc
