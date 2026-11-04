#include "layout.hpp"

#include <array>
#include <string>
#include <string_view>

namespace fc_plugins::voice_diagnostics {
namespace {

// Stores function and call-site RVAs for each supported retail executable.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t receive_packet;
    std::uint32_t read_header;
    std::uint32_t read_header_call;
    std::uint32_t receive_audio;
    std::uint32_t receive_audio_call;
    std::uint32_t relay_audio;
    std::uint32_t get_method_type;
    std::uint32_t relay_method_type_call;
    std::uint32_t get_num_nodes;
    std::uint32_t relay_node_count_call;
    std::uint32_t get_receive_node;
    std::uint32_t relay_recipient_call;
    std::uint32_t send_relay_packet;
    std::uint32_t send_relay_call;
    std::uint32_t send_voice_packet;
    std::uint32_t decoder_receive_packet;
    std::uint32_t update_latency;
    std::uint32_t admitted_identity;
    std::uint32_t decode_stream;
    std::uint32_t node_close;
    std::uint32_t queue_retired;
    std::uint32_t flush_packet;
};

constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .receive_packet = {0x0013E300},
        .read_header = {0x00272920},
        .read_header_call = {0x0027404D},
        .receive_audio = {0x00273F50},
        .receive_audio_call = {0x00274122},
        .relay_audio = {0x002744D0},
        .get_method_type = {0x002745F0},
        .relay_method_type_call = {0x002744ED},
        .get_num_nodes = {0x002749B0},
        .relay_node_count_call = {0x002744FC},
        .get_receive_node = {0x00272870},
        .relay_recipient_call = {0x00274515},
        .send_relay_packet = {0x002743B0},
        .send_relay_call = {0x00274539},
        .send_voice_packet = {0x0013E1F0},
        .decoder_receive_packet = {0x0013CFF0},
        .update_latency = {0x00273080},
        .admitted_identity = {0x00274118},
        .decode_stream = {0x00273C10},
        .node_close = {0x00272D70},
        .queue_retired = {0x002738BD},
        .flush_packet = {0x00273A50},
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .receive_packet = {0x0013F050},
        .read_header = {0x002739C0},
        .read_header_call = {0x002750ED},
        .receive_audio = {0x00274FF0},
        .receive_audio_call = {0x002751C2},
        .relay_audio = {0x00275570},
        .get_method_type = {0x00275690},
        .relay_method_type_call = {0x0027558D},
        .get_num_nodes = {0x00275A50},
        .relay_node_count_call = {0x0027559C},
        .get_receive_node = {0x00273910},
        .relay_recipient_call = {0x002755B5},
        .send_relay_packet = {0x00275450},
        .send_relay_call = {0x002755D9},
        .send_voice_packet = {0x0013EF40},
        .decoder_receive_packet = {0x0013DD40},
        .update_latency = {0x00274120},
        .admitted_identity = {0x002751B8},
        .decode_stream = {0x00274CB0},
        .node_close = {0x00273E10},
        .queue_retired = {0x0027495D},
        .flush_packet = {0x00274AF0},
    },
};

// Associates each retail RVA with its native function or expected call target.
Layout make_layout(const ProfileSites &sites) {
    return {
        .receive_packet = {.rva = sites.receive_packet, .name = "VoiceChat::NetPacketAudio::Receive"},
        .read_header_call =
            {
                .rva = sites.read_header_call,
                .name = "incoming voice header read",
                .evidence =
                    fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.read_header, "VoiceChat::Node::Header::Read"}),
            },
        .receive_audio_call =
            {
                .rva = sites.receive_audio_call,
                .name = "incoming voice payload delivery",
                .evidence = fc::direct_call_to(
                    fc::Location{std::string{sites.profile}, sites.receive_audio, "VoiceChat::Node::ReceiveAudioPacketAllLocalNodes"}),
            },
        .relay_audio = {.rva = sites.relay_audio, .name = "VoiceChat::Node::RelayAudioPacket"},
        .relay_method_type_call =
            {
                .rva = sites.relay_method_type_call,
                .name = "relay transport method query",
                .evidence = fc::direct_call_to(
                    fc::Location{std::string{sites.profile}, sites.get_method_type, "VoiceChat::Transport::Method::GetType"}),
            },
        .relay_node_count_call =
            {
                .rva = sites.relay_node_count_call,
                .name = "relay node count query",
                .evidence =
                    fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.get_num_nodes, "VoiceChat::Transport::GetNumNodes"}),
            },
        .relay_recipient_call =
            {
                .rva = sites.relay_recipient_call,
                .name = "relay recipient lookup",
                .evidence = fc::direct_call_to(
                    fc::Location{std::string{sites.profile}, sites.get_receive_node, "VoiceChat::Node::Header::GetReceiveNode"}),
            },
        .send_relay_call =
            {
                .rva = sites.send_relay_call,
                .name = "relay voice packet send",
                .evidence = fc::direct_call_to(
                    fc::Location{std::string{sites.profile}, sites.send_relay_packet, "VoiceChat::Node::SendAudioPacket"}),
            },
        .send_voice_packet = {.rva = sites.send_voice_packet, .name = "GameVoiceChat::SendVoice"},
        .decoder_receive_packet = {.rva = sites.decoder_receive_packet, .name = "GameVoiceChat::Decoder::ReceivePacket"},
        .update_latency = {.rva = sites.update_latency, .name = "voice target queue depth in packets"},
        // Both retail frames retain the parsed sequence at EBP-0xC; the following instructions load sender and descriptor.
        .admitted_identity = {.rva = sites.admitted_identity,
                              .name = "admitted voice frame identity",
                              .evidence = fc::exact_bytes({0x53, 0x8A, 0x5D, 0xFE, 0x8D, 0x55, 0xD8})},
        .decode_stream = {.rva = sites.decode_stream,
                          .name = "VoiceChat::Node::DecodeStream",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56})},
        // ECX=node, two byte-valued stack arguments, RET 8; queues remain live until this function runs.
        .node_close = {.rva = sites.node_close,
                       .name = "VoiceChat::Node::Close",
                       .evidence = fc::exact_bytes({0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x10})},
        // Common retirement path before clearing the owner/size and advancing the read index; no payload bytes are observed.
        .queue_retired = {.rva = sites.queue_retired,
                          .name = "native voice queue slot retirement",
                          .evidence = fc::exact_bytes({0xC7, 0x47, 0x08, 0x00, 0x00, 0x00, 0x00})},
        // ECX=node, sender queue and marker on the stack, AL=DecodeStream result after the pending run was cleared; RET 8.
        .flush_packet = {.rva = sites.flush_packet,
                         .name = "VoiceChat::Node::FlushPacket",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
    };
}

} // namespace

std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (profile == sites.profile) {
            return make_layout(sites);
        }
    }

    return std::nullopt;
}

} // namespace fc_plugins::voice_diagnostics
