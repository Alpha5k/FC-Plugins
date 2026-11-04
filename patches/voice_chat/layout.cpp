#include "layout.hpp"

#include <array>
#include <string>
#include <string_view>

namespace fc_plugins::voice_chat {
namespace {

// Retail shifts the Q15 product by 16, halving capture audio before encoding.
constexpr auto kHalvedCaptureGain = fc::byte_array<0x0F, 0xAC, 0xD9, 0x10>();
// The decoder remains in ESI while its flags byte at +0x281 is read.
constexpr auto kPlaybackRendererReady = fc::byte_array<0x8A, 0x86, 0x81, 0x02, 0x00, 0x00>();

// Stores voice operation RVAs for one executable; make_layout adds shared instruction evidence and derived locations.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t calculate_samples;
    std::uint32_t packet_duration;
    std::uint32_t encoder_packet_duration;
    std::uint32_t encoder_sample_rate;
    std::uint32_t decoder_sample_rate;
    std::uint32_t vad_sample_rate;
    std::uint32_t vad_release;
    std::uint32_t vad_frequency_filter;
    std::uint32_t vad_power_result;
    std::uint32_t capture_source_open;
    std::uint32_t voice_capture_open;
    std::uint32_t aux_renderer_open;
    std::uint32_t voice_playback_open;
    std::uint32_t capture_ring_size;
    std::uint32_t agc_get_packet;
    std::uint32_t encoder_set_gain;
    std::uint32_t decoder_open;
    std::array<std::uint32_t, 5> playback_gain_shifts;
    std::uint32_t gsm_decode;
    std::uint32_t gsm_encode;
    std::uint32_t gsm_init;
    std::uint32_t encoder_close;
    std::uint32_t decoder_close;
    std::uint32_t send_audio;
    std::uint32_t send_network_dispatch;
    std::uint32_t local_echo_enable;
    std::uint32_t local_echo_enabled;
    std::uint32_t local_echo_store;
    std::uint32_t default_transport_method;
    std::uint32_t relay_audio;
    std::uint32_t get_relay_recipient;
    std::uint32_t relay_recipient_call;
    std::uint32_t recipient_count_start;
    std::uint32_t send_possible;
    std::uint32_t receive_possible;
    std::uint32_t hud_receive_possible;
    std::uint32_t hud_bind;
    std::uint32_t set_sending;
    std::uint32_t receive_audio;
    std::uint32_t enqueue_audio;
    std::uint32_t sequence_gap;
    std::uint32_t update_latency;
    std::uint32_t get_current_profile;
    std::uint32_t apply_online_options;
    std::uint32_t reset_online_options;
    std::uint32_t game_voice_open;
    std::uint32_t game_voice_close;
    std::uint32_t game_voice_is_open;
    std::uint32_t find_voice_node;
    std::uint32_t is_local;
    std::uint32_t send_connected;
    std::uint32_t receive_connected;
    std::uint32_t get_sending;
    std::uint32_t get_lobby_player_index;
    std::uint32_t node_open;
    std::uint32_t network_read;
    std::uint32_t network_get_packet;
    std::uint32_t network_send;
    std::uint32_t network_send_packet;
    std::uint32_t voice_lookup;
    std::uint32_t classify_packet;
    std::uint32_t packet_reset;
    std::uint32_t packet_release;
    std::uint32_t packet_audio;
    std::uint32_t receive_metrics;
    std::uint32_t voice_time;
    std::uint32_t voice_nodes;
};

// Steam and GOG retain the same voice implementation at different RVAs.
constexpr std::array kProfiles{
    ProfileSites{
        .profile = "BF2.Steam.59EDE353",
        .calculate_samples = {0x0013C5C0},
        .packet_duration = {0x00273E4F},
        .encoder_packet_duration = {0x0013D4DE},
        .encoder_sample_rate = {0x0013D5C4},
        .decoder_sample_rate = {0x0013CB6E},
        .vad_sample_rate = {0x00342020},
        .vad_release = {0x00342034},
        .vad_frequency_filter = {0x00342221},
        .vad_power_result = {0x00342213},
        .capture_source_open = {0x003411F0},
        .voice_capture_open = {0x0013D60D},
        .aux_renderer_open = {0x0033F4A0},
        .voice_playback_open = {0x0013CB96},
        .capture_ring_size = {0x0034132B},
        .agc_get_packet = {0x00341E50},
        .encoder_set_gain = {0x0013DA30},
        .decoder_open = {0x0013C810},
        .playback_gain_shifts = {0x00340CA7, 0x00340CD3, 0x00340D01, 0x00340D2F, 0x00340D6F},
        .gsm_decode = {0x0034BCA0},
        .gsm_encode = {0x0034BCC0},
        .gsm_init = {0x0034BCE0},
        .encoder_close = {0x0013D710},
        .decoder_close = {0x0013CCE0},
        .send_audio = {0x00273480},
        .send_network_dispatch = {0x0013E223},
        .local_echo_enable = {0x00196020},
        .local_echo_enabled = {0x01A57935},
        .local_echo_store = {0x00196123},
        .default_transport_method = {0x0013C164},
        .relay_audio = {0x002744D0},
        .get_relay_recipient = {0x00272870},
        .relay_recipient_call = {0x00274515},
        .recipient_count_start = {0x002729E6},
        .send_possible = {0x00274E70},
        .receive_possible = {0x00274EC0},
        .hud_receive_possible = {0x00251718},
        .hud_bind = 0x002514F0,
        .set_sending = {0x00274C10},
        .receive_audio = {0x00273F50},
        .enqueue_audio = {0x00273900},
        .sequence_gap = {0x002740CF},
        .update_latency = {0x00273080},
        .get_current_profile = {0x0021D5F0},
        .apply_online_options = {0x0021CFF0},
        .reset_online_options = {0x0021CEA0},
        .game_voice_open = {0x0013C040},
        .game_voice_close = {0x0013C2C0},
        .game_voice_is_open = {0x0013C2B0},
        .find_voice_node = {0x0013C4F0},
        .is_local = {0x00274AC0},
        .send_connected = {0x00274BD0},
        .receive_connected = {0x00274BF0},
        .get_sending = {0x00274C40},
        .get_lobby_player_index = {0x00192070},
        .node_open = 0x00272B70,
        .network_read = 0x0013E362,
        .network_get_packet = 0x001B29C0,
        .network_send = 0x0013E23A,
        .network_send_packet = 0x001B2970,
        .voice_lookup = 0x001B3725,
        .classify_packet = 0x001B3420,
        .packet_reset = 0x001D8FD0,
        .packet_release = 0x0013E380,
        .packet_audio = 0x0013E170,
        .receive_metrics = 0x01AAFD54,
        .voice_time = 0x01AAFD58,
        .voice_nodes = 0x003EBEEC,
    },
    ProfileSites{
        .profile = "BF2.GOG.59EDF52B",
        .calculate_samples = {0x0013D310},
        .packet_duration = {0x00274EEF},
        .encoder_packet_duration = {0x0013E22E},
        .encoder_sample_rate = {0x0013E314},
        .decoder_sample_rate = {0x0013D8BE},
        .vad_sample_rate = {0x00343110},
        .vad_release = {0x00343124},
        .vad_frequency_filter = {0x00343311},
        .vad_power_result = {0x00343303},
        .capture_source_open = {0x003422E0},
        .voice_capture_open = {0x0013E35D},
        .aux_renderer_open = {0x00340590},
        .voice_playback_open = {0x0013D8E6},
        .capture_ring_size = {0x0034241B},
        .agc_get_packet = {0x00342F40},
        .encoder_set_gain = {0x0013E780},
        .decoder_open = {0x0013D560},
        .playback_gain_shifts = {0x00341D97, 0x00341DC3, 0x00341DF1, 0x00341E1F, 0x00341E5F},
        .gsm_decode = {0x0034CD90},
        .gsm_encode = {0x0034CDB0},
        .gsm_init = {0x0034CDD0},
        .encoder_close = {0x0013E460},
        .decoder_close = {0x0013DA30},
        .send_audio = {0x00274520},
        .send_network_dispatch = {0x0013EF73},
        .local_echo_enable = {0x00196FD0},
        .local_echo_enabled = {0x01A58DE5},
        .local_echo_store = {0x001970D3},
        .default_transport_method = {0x0013CEB4},
        .relay_audio = {0x00275570},
        .get_relay_recipient = {0x00273910},
        .relay_recipient_call = {0x002755B5},
        .recipient_count_start = {0x00273A86},
        .send_possible = {0x00275F10},
        .receive_possible = {0x00275F60},
        .hud_receive_possible = {0x002527B8},
        .hud_bind = 0x00252590,
        .set_sending = {0x00275CB0},
        .receive_audio = {0x00274FF0},
        .enqueue_audio = {0x002749A0},
        .sequence_gap = {0x0027516F},
        .update_latency = {0x00274120},
        .get_current_profile = {0x0021E650},
        .apply_online_options = {0x0021E050},
        .reset_online_options = {0x0021DF10},
        .game_voice_open = {0x0013CD90},
        .game_voice_close = {0x0013D010},
        .game_voice_is_open = {0x0013D000},
        .find_voice_node = {0x0013D240},
        .is_local = {0x00275B60},
        .send_connected = {0x00275C70},
        .receive_connected = {0x00275C90},
        .get_sending = {0x00275CE0},
        .get_lobby_player_index = {0x00193010},
        .node_open = 0x00273C10,
        .network_read = 0x0013F0B2,
        .network_get_packet = 0x001B3960,
        .network_send = 0x0013EF8A,
        .network_send_packet = 0x001B3910,
        .voice_lookup = 0x001B46D5,
        .classify_packet = 0x001B43D0,
        .packet_reset = 0x001DA080,
        .packet_release = 0x0013F0D0,
        .packet_audio = 0x0013EEC0,
        .receive_metrics = 0x01AB11F8,
        .voice_time = 0x01AB11FC,
        .voice_nodes = 0x003ECEBC,
    },
};

// Checks the MOVSS form that stores XMM0; the modifier does not depend on the absolute destination.
fc::Evidence packet_duration_evidence() {
    return fc::exact_bytes({0xF3, 0x0F, 0x11, 0x05});
}

// Combines this profile's RVAs with native names, ABIs and evidence for instruction modifiers and writes.
Layout make_layout(const ProfileSites &sites) {
    Layout layout{
        .calculate_samples = {.rva = sites.calculate_samples, .name = "GameVoiceChat::CalculatePlayCaptureBufferSizeSamples"},
        .packet_duration =
            {
                .rva = sites.packet_duration,
                .name = "Voice packet duration store",
                .evidence = packet_duration_evidence(),
            },
        .encoder_packet_duration =
            {
                .rva = sites.encoder_packet_duration,
                .name = "Encoder minimum packet duration branch",
                .evidence = fc::exact_bytes({0x76, 0x08}),
            },
        .encoder_sample_rate =
            {
                .rva = sites.encoder_sample_rate,
                .name = "Encoder PCM sample rate",
                .evidence = fc::exact_bytes({0x68, 0x40, 0x1F, 0x00, 0x00}),
            },
        .decoder_sample_rate =
            {
                .rva = sites.decoder_sample_rate,
                .name = "Decoder PCM sample rate",
                .evidence = fc::exact_bytes({0x68, 0x40, 0x1F, 0x00, 0x00}),
            },
        .vad_sample_rate =
            {
                .rva = sites.vad_sample_rate,
                .name = "Voice activity sample rate",
                .evidence = fc::exact_bytes({0xC7, 0x46, 0x30, 0x40, 0x1F, 0x00, 0x00}),
            },
        .vad_release =
            {
                .rva = sites.vad_release,
                .name = "Voice activity release byte budget",
                .evidence = fc::exact_bytes({0xC7, 0x46, 0x34, 0xF0, 0x0A, 0x00, 0x00}),
            },
        .vad_frequency_filter =
            {
                .rva = sites.vad_frequency_filter,
                .name = "Voice activity frequency filter",
                .evidence = fc::exact_bytes({0x8B, 0x47, 0x08}),
            },
        .vad_power_result =
            {
                .rva = sites.vad_power_result,
                .name = "Voice activity mean-square power comparison",
                .evidence = fc::exact_bytes({0x83, 0xC4, 0x08, 0x85, 0xD2, 0x75, 0x07, 0x3D, 0x30, 0x75, 0x00, 0x00}),
            },
        .capture_source_open = {.rva = sites.capture_source_open, .name = "Snd::StreamCaptureSource::Open"},
        .voice_capture_open = {},
        .aux_renderer_open = {.rva = sites.aux_renderer_open, .name = "Snd::StreamAuxRenderer::Open"},
        .voice_playback_open = {},
        .capture_ring_size =
            {
                .rva = sites.capture_ring_size,
                .name = "Capture ring size",
                .evidence = fc::exact_bytes({0x03, 0xC0}),
            },
        .scale_capture_gain =
            {
                .rva = sites.agc_get_packet + 0xA2,
                .name = "StreamAGC::GetPacket gain scaling",
                .evidence = fc::exact_bytes(kHalvedCaptureGain),
            },
        .source_gain_store =
            {
                .rva = sites.encoder_set_gain,
                .name = "Encoder::SetGain source gain store",
                .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x89, 0xEC, 0x04, 0x00, 0x00}),
            },
        .playback_renderer_ready =
            {
                .rva = sites.decoder_open + 0x3F1,
                .name = "Decoder::Open after auxiliary renderer setup",
                .evidence = fc::exact_bytes(kPlaybackRendererReady),
            },
        .scale_playback_gain = {},
        .gsm_decode = {.rva = sites.gsm_decode, .name = "Snd::GSM::Decode"},
        .gsm_encode = {.rva = sites.gsm_encode, .name = "Snd::GSM::Encode"},
        .gsm_init = {.rva = sites.gsm_init, .name = "Snd::GSM::Init"},
        .encoder_close =
            {
                .rva = sites.encoder_close,
                .name = "GameVoiceChat::Encoder::Close",
                .evidence = fc::exact_bytes({0x56, 0x8B, 0xF1, 0xC7, 0x86, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
            },
        .decoder_close =
            {
                .rva = sites.decoder_close,
                .name = "GameVoiceChat::Decoder::Close",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x33, 0xFF}),
            },
        .send_audio = {.rva = sites.send_audio, .name = "Outgoing voice payload"},
        .send_method_dispatch =
            {
                .rva = sites.send_audio + 0x2E,
                .name = "Outgoing voice transport method dispatch",
                .evidence = fc::exact_bytes({0x83, 0xE8, 0x02}),
            },
        .send_network_dispatch =
            {
                .rva = sites.send_network_dispatch,
                .name = "Voice local or network delivery decision",
                .evidence = fc::exact_bytes({0x84, 0xC9, 0x74, 0x24}),
            },
        .local_echo_enable = {.rva = sites.local_echo_enable, .name = "VoiceLocalEchoEnable"},
        .local_echo_enabled = {.rva = sites.local_echo_enabled, .name = "gVoiceLocalEchoEnabled"},
        .local_echo_store =
            {
                .rva = sites.local_echo_store,
                .name = "Microphone test flag store",
                .evidence = fc::exact_bytes({0x88, 0x1D}),
            },
        .default_transport_method =
            {
                .rva = sites.default_transport_method,
                .name = "Voice transport method at open",
                .evidence = fc::exact_bytes({0xB9, 0x02, 0x00, 0x00, 0x00}),
            },
        .transport_override =
            {
                .rva = sites.default_transport_method + 0x10,
                .name = "Voice mode command-line override at open",
                .evidence = fc::exact_bytes({0x0F, 0x45, 0xC8}),
            },
        .receive_audio = {.rva = sites.receive_audio, .name = "Incoming voice payload"},
        .enqueue_audio = {.rva = sites.enqueue_audio, .name = "VoiceChat::Node::ReceiveAudioPacket"},
        .receive_enqueue_call = {},
        .sequence_gap =
            {
                .rva = sites.sequence_gap,
                .name = "VoiceChat::Node::Update admitted sequence gap",
                .evidence = fc::exact_bytes({0x0F, 0xB7, 0xD8, 0xEB, 0x02}),
            },
        .relay_audio = {.rva = sites.relay_audio, .name = "VoiceChat::Node::RelayAudioPacket"},
        .get_relay_recipient = {.rva = sites.get_relay_recipient, .name = "VoiceChat::Node::Header::GetReceiveNode"},
        .relay_recipient_call = {},
        .recipient_count_start =
            {
                .rva = sites.recipient_count_start,
                .name = "Voice recipient count bitmap start",
                .evidence = fc::exact_bytes({0x46, 0xE8, 0xC4, 0x1F, 0x00, 0x00}),
            },
        .send_enable = {.rva = sites.send_possible - 0x70, .name = "VoiceChat::Transport::GetSendEnable"},
        .receive_enable = {.rva = sites.send_possible - 0x20, .name = "VoiceChat::Transport::GetReceiveEnable"},
        .send_possible = {.rva = sites.send_possible, .name = "Voice send decision"},
        .receive_possible = {.rva = sites.receive_possible, .name = "Voice receive decision"},
        .hud_receive_possible = {},
        .hud_font = {.rva = sites.hud_bind + 0x3D, .name = "Voice HUD font", .evidence = fc::exact_bytes({0x68, 0xD5, 0x60, 0xF0, 0x46})},
        .hud_row = {.rva = sites.hud_bind + 0xAE, .name = "Voice HUD row spacing", .evidence = fc::exact_bytes({0x0F, 0xB6, 0x47, 0x0E})},
        .set_sending = {.rva = sites.set_sending, .name = "Local speaking state"},
        .native_update =
            {
                .rva = sites.sequence_gap - 0xAF,
                .name = "VoiceChat::Node::Update static tick",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x53, 0x56, 0x57, 0xF3, 0x0F, 0x11, 0x45, 0xE8}),
            },
        .jitter_target =
            {
                .rva = sites.update_latency + 0x75,
                .name = "Voice jitter packet target store",
                .evidence = fc::exact_bytes({0x66, 0x89, 0x4C, 0xC2, 0x10}),
            },
        .get_current_profile = {.rva = sites.get_current_profile, .name = "PlayerProfile::GetCurProfile"},
        .apply_online_options = {.rva = sites.apply_online_options, .name = "PlayerProfile::tOnlineOpt::ApplySettings"},
        .reset_online_options = {.rva = sites.reset_online_options, .name = "PlayerProfile::tOnlineOpt::ResetToDefault"},
        .game_voice_open = {.rva = sites.game_voice_open, .name = "GameVoiceChat::Open"},
        .game_voice_close = {.rva = sites.game_voice_close, .name = "GameVoiceChat::Close"},
        // Device callbacks call this thunk before resolving the node and its connection bit.
        .game_voice_is_open = {.rva = sites.game_voice_is_open, .name = "GameVoiceChat::IsOpen"},
        .find_voice_node = {.rva = sites.find_voice_node, .name = "GameVoiceChat::FindNodeByDeviceID"},
        .get_voice_transport = {.rva = sites.calculate_samples - 0x30, .name = "GameVoiceChat::GetTransport"},
        .is_local =
            {
                .rva = sites.is_local,
                .name = "VoiceChat::Transport::IsLocal",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8A, 0x51, 0x40, 0x32, 0xC0, 0x53, 0x56,
                                             0x84, 0xD2, 0x74, 0x14, 0x8B, 0x71, 0x2C, 0x8A, 0x5D, 0x08}),
            },
        .send_connected =
            {
                .rva = sites.send_connected,
                .name = "VoiceChat::Transport::IsConnectedSend",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0x01, 0xFF, 0x75, 0x08, 0xFF, 0x71,
                                             0x08, 0xE8, 0x10, 0x04, 0x00, 0x00, 0x5D, 0xC2, 0x04, 0x00}),
            },
        .receive_connected =
            {
                .rva = sites.receive_connected,
                .name = "VoiceChat::Transport::IsConnectedReceive",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0x00, 0xFF, 0x75, 0x08, 0xFF, 0x71,
                                             0x08, 0xE8, 0xF0, 0x03, 0x00, 0x00, 0x5D, 0xC2, 0x04, 0x00}),
            },
        .get_sending =
            {
                .rva = sites.get_sending,
                .name = "VoiceChat::Transport::GetSending",
                .evidence =
                    fc::exact_bytes({0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x8B, 0xF1, 0x0F, 0xB6, 0xD3, 0x8B, 0x4E, 0x0C}),
            },
        .get_lobby_player_index = {.rva = sites.get_lobby_player_index, .name = "Lua_Callbacks::GetLobbyPlayerIndex"},
        .node_open = {.rva = sites.node_open,
                      .name = "VoiceChat::Node::Open",
                      .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18})},
        .node_close = {.rva = sites.node_open + 0x200,
                       .name = "VoiceChat::Node::Close",
                       .evidence = fc::exact_bytes({0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x10})},
        .decode_stream = {.rva = sites.send_audio + 0x790,
                          .name = "VoiceChat::Node::DecodeStream",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56})},
        .flush_packet = {.rva = sites.send_audio + 0x5D0,
                         .name = "VoiceChat::Node::FlushPacket",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
        .get_num_nodes = {.rva = sites.send_possible - 0x4C0, .name = "VoiceChat::Transport::GetNumNodes"},
        // EDI points to receive metrics; ESI is nine times the parsed sender, EAX/CX are current/previous sequences.
        .sequence_admission = {.rva = sites.sequence_gap - 0x45,
                               .name = "Voice sequence admission",
                               .evidence = fc::exact_bytes({0x66, 0x3B, 0xC1, 0x73, 0x2F})},
        .hud_activity = {},
        .hud_last_activity = {},
        .network_read = {},
        .receive_packet = {.rva = sites.network_read - 0x62,
                           .name = "GameVoiceChat::ReadVoice",
                           .evidence = fc::exact_bytes({0x53, 0x56, 0x57, 0x33, 0xDB})},
        .network_send = {},
        .voice_lookup = {},
        .packet_reset = {.rva = sites.packet_reset, .name = "NetPkt::Reset"},
        .packet_release = {.rva = sites.packet_release, .name = "GameVoiceChat::ReleaseVoice"},
        .packet_audio = {.rva = sites.packet_audio, .name = "VoiceChat::NetPacketAudio constructor"},
        .read_header = {.rva = sites.send_audio - 0xB60, .name = "VoiceChat::Node::Header::Read"},
        .header_sender = {.rva = sites.send_audio - 0xAE0, .name = "VoiceChat::Node::Header::GetSendNode"},
        .receive_metrics = {.rva = sites.receive_metrics, .name = "Voice receive metrics pointer"},
        .voice_time = {.rva = sites.voice_time, .name = "VoiceChat::Node::sTime"},
        .voice_nodes = {.rva = sites.voice_nodes, .name = "VoiceChat::Node::sNodes"},
    };

    for (std::size_t index = 0; index < layout.scale_playback_gain.size(); ++index) {
        layout.scale_playback_gain[index] = {
            .rva = sites.playback_gain_shifts[index],
            .name = "StreamMixer::Update output gain scaling",
            .evidence = fc::exact_bytes({0xC1, 0xFA, 0x10}),
        };
    }

    // Call-target evidence ties each scoped call to its target in the same image profile.
    layout.voice_capture_open = {
        .rva = sites.voice_capture_open,
        .name = "Voice capture stream open",
        .evidence =
            fc::direct_call_to(fc::Location{std::string{sites.profile}, layout.capture_source_open.rva, layout.capture_source_open.name}),
    };
    layout.voice_playback_open = {
        .rva = sites.voice_playback_open,
        .name = "Voice playback stream open",
        .evidence =
            fc::direct_call_to(fc::Location{std::string{sites.profile}, layout.aux_renderer_open.rva, layout.aux_renderer_open.name}),
    };
    layout.relay_recipient_call = {
        .rva = sites.relay_recipient_call,
        .name = "Voice relay recipient lookup",
        .evidence =
            fc::direct_call_to(fc::Location{std::string{sites.profile}, layout.get_relay_recipient.rva, layout.get_relay_recipient.name}),
    };
    // ReceiveAudioPacketAllLocalNodes selects policy/header recipients before this one per-node copy.
    layout.receive_enqueue_call = {
        .rva = sites.receive_audio + 0x82,
        .name = "Voice local-node enqueue after recipient selection",
        .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, layout.enqueue_audio.rva, layout.enqueue_audio.name}),
    };
    layout.hud_receive_possible = {
        .rva = sites.hud_receive_possible,
        .name = "Speaker HUD receive decision",
        .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, layout.send_possible.rva, layout.send_possible.name}),
    };
    layout.hud_activity = {.rva = sites.hud_receive_possible - 0x16,
                           .name = "Speaker HUD activity",
                           .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.get_sending})};
    layout.hud_last_activity = {.rva = sites.hud_receive_possible + 0x14C,
                                .name = "Speaker HUD activity time",
                                .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.send_possible - 0xB50})};
    layout.network_read = {.rva = sites.network_read,
                           .name = "GameVoiceRead network-only intake",
                           .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.network_get_packet})};
    layout.network_send = {.rva = sites.network_send,
                           .name = "SendVoice network submission",
                           .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.network_send_packet})};
    layout.voice_lookup = {.rva = sites.voice_lookup,
                           .name = "Voice host connection lookup classification",
                           .evidence = fc::direct_call_to(fc::Location{std::string{sites.profile}, sites.classify_packet})};
    return layout;
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

} // namespace fc_plugins::voice_chat
