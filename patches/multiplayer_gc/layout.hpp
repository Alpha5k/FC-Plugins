#pragma once

#include "state.hpp"

#include <fc/native.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::multiplayer_gc {

// Describes the retail compressed buffers, uint32 lengths and their native save instructions.
struct Layout {
    fc::DataLocation<char, kCampaignCapacity> campaign;
    fc::DataLocation<std::uint32_t> campaign_length;
    fc::DataLocation<char, kMissionCapacity> mission;
    fc::DataLocation<std::uint32_t> mission_length;
    fc::CodeLocation campaign_save;
    fc::CodeLocation mission_save;
};

// Original chat editor, message events and state-owned HUD, qualified for each retail image.
struct ChatLayout {
    std::uint32_t input_process, input_gate, shell_gate, shell_state, submit;
    std::uint32_t keyboard_event, keyboard_update, key_map, event_key_map;
    std::uint32_t display_name, display_team, copy_name;
    std::uint32_t strip_create, strip_init, strip_activate, strip_delete, strip_hide, strip_global, strip_position;
    std::uint32_t hud_update, hud_group_reference, hud_group;
    std::uint32_t messages_create, messages_destroy, messages_global, message_event;
};

// Retail shell serializer entries, native chat calls and their shared recipient fields.
struct ShellLayout {
    std::uint32_t write_move, read_move, write_update, read_update;
    std::uint32_t queue_chat, send_server, send_client, display, player_name, local_slot;
    std::uint32_t read_bytes, resolve_packet, destination, coverage;
    std::uint32_t host_queue, remove_node, free_record;
    std::uint32_t client_due, client_queue_count, host_send_updates;
    std::uint32_t publication_hidden;
    fc::Evidence publication_evidence;
    std::uint32_t search_filter, reader_cancelled, reader_count;
    std::uint32_t reader_remove_advance;
    std::uint32_t input_manager_reference, input_manager, input_getter, input_flags_read;
    std::uint32_t browser_map_gate, browser_resort, browser_sort, browser_direction;
    std::uint32_t browser_select, browser_sort_reference, browser_direction_reference;
    ChatLayout chat;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<ShellLayout> select_shell_layout(std::string_view profile);

} // namespace fc_plugins::multiplayer_gc
