#include "layout.hpp"

namespace fc_plugins::soldier_state_sync {

std::optional<Layout> select_layout(std::string_view profile) {
    const bool steam = profile == "BF2.Steam.59EDE353";
    if (!steam && profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }
    // Steam and GOG use the same instruction spans, with independently qualified entry locations.
    const auto entry = fc::exact_bytes({0x55, 0x8B, 0xEC});
    const auto group = steam ? 0x001B2F90U : 0x001B3F40U;
    return Layout{
        .send_update = {.rva = steam ? 0x001BAE80U : 0x001BBE30U, .name = "SendUpdate", .evidence = entry},
        .write_update = {.rva = steam ? 0x001BB100U : 0x001BC0B0U, .name = "WriteUpdate", .evidence = entry},
        .read_update = {.rva = steam ? 0x001BB5B0U : 0x001BC560U, .name = "ReadUpdate", .evidence = entry},
        .send_group = {.rva = steam ? 0x001B2900U : 0x001B38A0U, .name = "NetComm::Send", .evidence = entry},
        .get_group = {.rva = group, .name = "GetGroupFromPacket", .evidence = entry},
        .receive_packet = {.rva = steam ? 0x001B29C0U : 0x001B3960U,
                           .name = "NetComm::Recv(type, type)",
                           .evidence = fc::exact_bytes({0x56, 0x57, 0x8B, 0xF9})},
        .get_packet = {.rva = steam ? 0x001D8F00U : 0x001D9FB0U, .name = "NetPacket::Get"},
        .free_packet = {.rva = steam ? 0x001D8E90U : 0x001D9F40U, .name = "NetPacket::Free"},
        .remove_packet = {.rva = steam ? 0x001D9500U : 0x001DA5B0U, .name = "NetPktGroup::Remove"},
        .connect = {.rva = steam ? 0x001B2CA0U : 0x001B3C40U,
                    .name = "NetComm::Connect",
                    .evidence = fc::exact_bytes({0x8B, 0x02, 0x8D, 0x0C, 0x49})},
        .disconnect = {.rva = steam ? 0x001B2AF0U : 0x001B3A90U,
                       .name = "NetComm::Disconnect",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
        .endpoints = {.rva = steam ? 0x01ACC0C0U : 0x01ACD570U, .name = "native endpoint records and delayed outgoing queues"},
        .free_group = {.rva = steam ? 0x001D9250U : 0x001DA300U, .name = "NetPktGroup::Free"},
        .write_bits = {.rva = steam ? 0x001D9690U : 0x001DA740U, .name = "NetPktGroup::WriteBits", .evidence = entry},
        .read_bits = {.rva = steam ? 0x001D9710U : 0x001DA7C0U, .name = "NetPktGroup::ReadBits", .evidence = entry},
        .write_velocity = {.rva = steam ? 0x001E9835U : 0x001EA8D5U,
                           .name = "Soldier::WriteObject velocity",
                           .evidence = fc::exact_bytes({0xE8, 0x56, 0xC4, 0, 0})},
        .read_velocity = {.rva = steam ? 0x001EA2EBU : 0x001EB38BU,
                          .name = "Soldier::ReadObject velocity",
                          .evidence = fc::exact_bytes({0xE8, 0xB0, 0xBA, 0, 0})},
        // Soldier+74C is always serialized; only these bit-count arguments change.
        .write_state_clock = {.rva = steam ? 0x001E98CDU : 0x001EA96DU,
                              .name = "Soldier::WriteObject state clock",
                              .evidence = fc::exact_bytes({0xE8, static_cast<std::uint8_t>(steam ? 0xBE : 0xCE), 0xFE, 0xFE, 0xFF})},
        .read_state_clock = {.rva = steam ? 0x001EA595U : 0x001EB635U,
                             .name = "Soldier::ReadObject state clock",
                             .evidence = fc::exact_bytes({0xE8, static_cast<std::uint8_t>(steam ? 0xC6 : 0xD6), 0xF2, 0xFE, 0xFF})},
        // Shared wrappers adapt ECX/EDX to the Trigger codecs; native callers retain their selection conditions.
        .write_trigger = {.rva = steam ? 0x001F61A0U : 0x001F7240U,
                          .name = "WriteTrigger",
                          .evidence = fc::exact_bytes({0x51, 0x8B, 0xCA})},
        .read_trigger = {.rva = steam ? 0x001F61B0U : 0x001F7250U, .name = "ReadTrigger", .evidence = fc::exact_bytes({0x51, 0x8B, 0xCA})},
        .write_melee = {.rva = steam ? 0x001E8A60U : 0x001E9B00U, .name = "WeaponMelee::Write", .evidence = entry},
        // Complete codecs finish native selection callbacks before the appended active phase.
        .write_soldier = {.rva = steam ? 0x001E9660U : 0x001EA700U, .name = "Soldier::WriteObject", .evidence = entry},
        .read_soldier = {.rva = steam ? 0x001EA140U : 0x001EB1E0U, .name = "Soldier::ReadObject", .evidence = entry},
        .melee_vtable = {.rva = steam ? 0x003B1578U : 0x003B24F0U, .name = "WeaponMelee vtable"},
        .update_turn = {.rva = steam ? 0x01BA65B0U : 0x01BA7A64U, .name = "received client update turn"},
        // Weapon::Write/Read includes EnergyBar only when its class heat capacity is positive.
        .write_heat = {.rva = steam ? 0x001E837FU : 0x001E941FU,
                       .name = "Weapon::Write conditional heat",
                       .evidence = fc::exact_bytes({0xE8, 0x4C, 0xFC, 0xFF, 0xFF})},
        .read_heat = {.rva = steam ? 0x001E857EU : 0x001E961EU,
                      .name = "Weapon::Read conditional heat",
                      .evidence = fc::exact_bytes({0xE8, 0xAD, 0xFA, 0xFF, 0xFF})},
        // Only Soldier+A10 binds the wider scalar; other EnergyBar callers keep their native codec.
        .write_energy = {.rva = steam ? 0x001E9844U : 0x001EA8E4U,
                         .name = "Soldier::WriteObject EnergyBar",
                         .evidence = fc::exact_bytes({0xE8, 0x87, 0xE7, 0xFF, 0xFF})},
        .read_energy = {.rva = steam ? 0x001EA32BU : 0x001EB3CBU,
                        .name = "Soldier::ReadObject EnergyBar",
                        .evidence = fc::exact_bytes({0xE8, 0, 0xDD, 0xFF, 0xFF})},
        // Both range branches have just pushed 12. Keep their flags and native overheat operations.
        .energy_write_count = {.rva = steam ? 0x001E7FFAU : 0x001E909AU,
                               .name = "EnergyBar writer scalar count",
                               .evidence = fc::exact_bytes({0x74, 0x15})},
        .energy_read_count = {.rva = steam ? 0x001E804FU : 0x001E90EFU,
                              .name = "EnergyBar reader scalar count",
                              .evidence = fc::exact_bytes({0x0F, 0x95, 0xC0})},
        // EDI/EDX retain the two query types; EAX is the actual queued packet type before this JZ.
        .receive_match = {.rva = steam ? 0x001B29DFU : 0x001B397FU,
                          .name = "NetComm::Recv second type match",
                          .evidence = fc::exact_bytes({0x74, 0x13})},
        .receive_type = {.rva = steam ? 0x001B32D4U : 0x001B4284U,
                         .name = "native game packet type gate",
                         .evidence = fc::exact_bytes({0x0F, 0x83, 0x39, 0x01, 0, 0})},
        // Broadcast receive paths already reserve the native game-header prefix.
        .receive_prefix = {{
            {.compare = {.rva = steam ? 0x00217FD1U : 0x00219041U,
                         .name = "socket packet prefix comparison",
                         .evidence = fc::masked_bytes({0x80, 0x3D, 0, 0, 0, 0, 0x1E}, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF})},
             .branch = {.rva = steam ? 0x00217FD8U : 0x00219048U,
                        .name = "socket packet prefix selection",
                        .evidence = fc::exact_bytes({0x73, 0x55})},
             .type = {.rva = steam ? 0x01BAB8E0U : 0x01BACD90U, .name = "socket receive packet type"}},
            {.compare = {.rva = steam ? 0x00218328U : 0x00219398U,
                         .name = "Galaxy packet prefix comparison",
                         .evidence = fc::masked_bytes({0x80, 0x3D, 0, 0, 0, 0, 0x1E}, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF})},
             .branch = {.rva = steam ? 0x0021832FU : 0x0021939FU,
                        .name = "Galaxy packet prefix selection",
                        .evidence = fc::exact_bytes({0x73, 0x5B})},
             .type = {.rva = steam ? 0x01BAB0D0U : 0x01BAC580U, .name = "Galaxy receive packet type"}},
        }},

        .send_fragment = {.rva = steam ? 0x001B2958U : 0x001B38F8U,
                          .name = "native fragment send",
                          .evidence = fc::exact_bytes({0xE8, 0x53, 0xFF, 0xFF, 0xFF})},
        .single_fragment = {.rva = group + 0x22, .name = "single fragment index", .evidence = fc::exact_bytes({0x80, 0x7B, 0x10, 0})},
        .group_identity = {.rva = group + 0x6F, .name = "pending group identity", .evidence = fc::exact_bytes({0x8A, 0x40, 0x0F})},
        .next_group = {.rva = group + 0x77, .name = "scan next pending group", .evidence = fc::exact_bytes({0x83, 0xC6, 0x1C})},
        // Each MOVZX/CMP span ends before its JZ/JNZ. Neither branch enters the other displaced span.
        .order = {{{.rva = group + 0x12C,
                    .name = "fragment ordering first index",
                    .evidence = fc::exact_bytes({0x0F, 0xB6, 0x48, 0x10, 0x3B, 0xCF})},
                   {.rva = group + 0x143,
                    .name = "fragment ordering next index",
                    .evidence = fc::exact_bytes({0x0F, 0xB6, 0x48, 0x10, 0x3B, 0xCF})}}},
    };
}

} // namespace fc_plugins::soldier_state_sync
