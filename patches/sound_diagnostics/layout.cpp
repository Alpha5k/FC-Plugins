#include "layout.hpp"

#include <array>
#include <string>

namespace fc_plugins::sound_diagnostics {
namespace {

// Both retail sound implementations have identical instruction forms and field offsets at these boundaries.
struct ProfileSites {
    std::string_view profile;
    std::uint32_t renderer_update;
    std::uint32_t renderer_flush;
    std::uint32_t renderer_write;
    std::uint32_t renderer_format;
    std::uint32_t mixer_created;
    std::uint32_t mixer_closing;
    std::uint32_t renderer_created;
    std::uint32_t renderer_closing;
    std::uint32_t position_result;
    std::uint32_t consumed;
    std::uint32_t cursor_reset;
    std::uint32_t requested;
    std::uint32_t lock_result;
    std::uint32_t unlock_result;
    std::uint32_t queue_exhausted;
    std::uint32_t zero_fill;
    std::uint32_t game_play;
    std::uint32_t sound_play;
    std::uint32_t setup_voice;
    std::uint32_t voice_start;
    std::uint32_t voice_stop;
    std::uint32_t voice_inactive;
    std::uint32_t controller_play;
    std::uint32_t controller_stop;
    std::uint32_t owner_wait_before;
    std::uint32_t owner_wait_after;
    std::uint32_t owner_sleep_before;
    std::uint32_t owner_sleep_after;
    std::uint32_t game_permission;
    std::uint32_t ordinary_property;
    std::uint32_t ordinary_sample;
    std::uint32_t ordinary_gain;
    std::uint32_t ordinary_voice;
    std::uint32_t layered_play;
    std::uint32_t layered_bind;
    std::uint32_t layered_due;
    std::uint32_t layered_due_end;
    std::uint32_t stream_play;
    std::uint32_t stream_setup;
    std::uint32_t stream_start;
    std::uint32_t device_play;
    std::uint32_t device_stop;
    std::uint32_t device_play_result;
    std::uint32_t stream_close;
    std::uint32_t stream_connect;
    std::uint32_t stream_requested_stop;
    std::uint32_t stream_requested_pause;
    std::uint32_t stream_requested_play;
    std::uint32_t stream_downstream_stop;
    std::uint32_t device_position_result;
    std::uint32_t flush_position_result;
    std::uint32_t stop_position_result;
    std::uint32_t flush_lock_result;
    std::uint32_t flush_unlock_result;
    std::uint32_t voice_assignment;
    std::uint32_t stream_queued;
    std::uint32_t stream_due;
    std::uint32_t stream_due_end;
    std::uint32_t stream_deferred_start;
    std::uint32_t game_permission_resolved;
    std::uint32_t ordinary_admission;
    std::uint32_t stream_slot_gate;
    std::uint32_t stream_bus_gate;
    std::uint32_t stream_bus_gain_gate;
    std::uint32_t stream_sample_gate;
    std::uint32_t stream_pitch_gate;
    std::uint32_t stream_property_gate;
    std::uint32_t stream_gain_gate;
    std::uint32_t stream_queue_gate;
    std::uint32_t stream_priority_gate;
    std::uint32_t layered_queue_gate;
    std::uint32_t flush_format;
    std::uint32_t sleep_import;
    std::uint32_t source_route_software;
    std::uint32_t source_route_hardware;
    std::uint32_t physical_route;
    std::uint32_t ui_plain;
    std::uint32_t ui_number;
    std::uint32_t packet;
    std::uint32_t virtual_end;
    std::uint32_t downstream_end;
    std::uint32_t saber_effects;
    std::uint32_t source_end;
    std::uint32_t packet_complete;
    std::uint32_t sound_reset;
    std::uint32_t stop_all;
    std::uint32_t melee_read;
    std::uint32_t melee_controls;
    std::uint32_t binding_assign;
    std::uint32_t fire_audio;
    std::uint32_t toggle_on;
    std::uint32_t toggle_off;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",

                 .renderer_update = 0x0033E630,
                 .renderer_flush = 0x0033DAD0,
                 .renderer_write = 0x0033EB00,
                 .renderer_format = 0x0033DE90,
                 .mixer_created = 0x0033F81A,
                 .mixer_closing = 0x0033F866,
                 .renderer_created = 0x0033D915,
                 .renderer_closing = 0x0033D999,

                 .position_result = 0x0033E71D,
                 .consumed = 0x0033E8D7,
                 .cursor_reset = 0x0033E90F,
                 .requested = 0x0033E960,
                 .lock_result = 0x0033E991,
                 .unlock_result = 0x0033EA1E,
                 .queue_exhausted = 0x0033F262,
                 .zero_fill = 0x0033F2E8,
                 .game_play = 0x00138010,
                 .sound_play = 0x0033a430,
                 .setup_voice = 0x0033aa80,
                 .voice_start = 0x00337c80,
                 .voice_stop = 0x00337f30,
                 .voice_inactive = 0x00339e30,
                 .controller_play = 0x001384c0,
                 .controller_stop = 0x00138660,

                 .owner_wait_before = 0x0033c6c9,
                 .owner_wait_after = 0x0033c6ce,
                 .owner_sleep_before = 0x0033c70a,
                 .owner_sleep_after = 0x0033c710,
                 .game_permission = 0x00138022,
                 .ordinary_property = 0x0033a456,
                 .ordinary_sample = 0x0033a469,
                 .ordinary_gain = 0x0033a52e,
                 .ordinary_voice = 0x0033a623,

                 .layered_play = 0x003391d0,
                 .layered_bind = 0x00339030,
                 .layered_due = 0x00339ab1,
                 .layered_due_end = 0x00339ab6,
                 .stream_play = 0x00336ed0,
                 .stream_setup = 0x00337560,
                 .stream_start = 0x0019b470,
                 .device_play = 0x00344670,
                 .device_stop = 0x00344dc0,
                 .device_play_result = 0x003446d7,

                 .stream_close = 0x00334a00,
                 .stream_connect = 0x00334c80,
                 .stream_requested_stop = 0x0033552a,
                 .stream_requested_pause = 0x003355ba,
                 .stream_requested_play = 0x0033563e,
                 .stream_downstream_stop = 0x00335765,
                 .device_position_result = 0x003446b0,
                 .flush_position_result = 0x0033db0b,
                 .stop_position_result = 0x0033e278,
                 .flush_lock_result = 0x0033db4f,
                 .flush_unlock_result = 0x0033dba4,

                 .voice_assignment = 0x337e30,
                 .stream_queued = 0x337033,
                 .stream_due = 0x33785a,
                 .stream_due_end = 0x33785f,
                 .stream_deferred_start = 0x337927,

                 .game_permission_resolved = 0x0013806f,
                 .ordinary_admission = 0x0033a668,
                 .stream_slot_gate = 0x00336f05,
                 .stream_bus_gate = 0x00336f12,
                 .stream_bus_gain_gate = 0x00336f1f,
                 .stream_sample_gate = 0x00336f33,
                 .stream_pitch_gate = 0x00336f58,
                 .stream_property_gate = 0x00336f68,
                 .stream_gain_gate = 0x00336fa1,
                 .stream_queue_gate = 0x00336fe4,
                 .stream_priority_gate = 0x0033708d,
                 .layered_queue_gate = 0x003392eb,
                 .flush_format = 0x003868c8,
                 .sleep_import = 0x0036B0C8,
                 .source_route_software = 0x00346243,
                 .source_route_hardware = 0x0034616f,

                 .physical_route = 0x003458bc,
                 .ui_plain = 0x001a11b0,
                 .ui_number = 0x001a11d0,
                 .packet = 0x0034cd30,
                 .virtual_end = 0x00338302,
                 .downstream_end = 0x0033832d,
                 .saber_effects = 0x00289010,
                 .source_end = 0x0034cada,
                 .packet_complete = 0x0033ca90,
                 .sound_reset = 0x00138c60,
                 .stop_all = 0x0033ad90,
                 .melee_read = 0x001e8b20,
                 .melee_controls = 0x00288700,

                 .binding_assign = 0x1383C0,
                 .fire_audio = 0x278AA0,
                 .toggle_on = 0x28EB70,
                 .toggle_off = 0x28EB90},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",

                 .renderer_update = 0x0033F720,
                 .renderer_flush = 0x0033EBC0,
                 .renderer_write = 0x0033FBF0,
                 .renderer_format = 0x0033EF80,
                 .mixer_created = 0x0034090A,
                 .mixer_closing = 0x00340956,
                 .renderer_created = 0x0033EA05,
                 .renderer_closing = 0x0033EA89,

                 .position_result = 0x0033F80D,
                 .consumed = 0x0033F9C7,
                 .cursor_reset = 0x0033F9FF,
                 .requested = 0x0033FA50,
                 .lock_result = 0x0033FA81,
                 .unlock_result = 0x0033FB0E,
                 .queue_exhausted = 0x00340352,
                 .zero_fill = 0x003403D8,
                 .game_play = 0x00138d80,
                 .sound_play = 0x0033b510,
                 .setup_voice = 0x0033bb60,
                 .voice_start = 0x00338d70,
                 .voice_stop = 0x00339020,
                 .voice_inactive = 0x0033af10,
                 .controller_play = 0x00139230,
                 .controller_stop = 0x001393d0,

                 .owner_wait_before = 0x0033d7b9,
                 .owner_wait_after = 0x0033d7be,
                 .owner_sleep_before = 0x0033d7fa,
                 .owner_sleep_after = 0x0033d800,
                 .game_permission = 0x00138d92,
                 .ordinary_property = 0x0033b536,
                 .ordinary_sample = 0x0033b549,
                 .ordinary_gain = 0x0033b60e,
                 .ordinary_voice = 0x0033b703,

                 .layered_play = 0x0033a2b0,
                 .layered_bind = 0x0033a110,
                 .layered_due = 0x0033ab91,
                 .layered_due_end = 0x0033ab96,
                 .stream_play = 0x00337fc0,
                 .stream_setup = 0x00338650,
                 .stream_start = 0x0019c420,
                 .device_play = 0x00345760,
                 .device_stop = 0x00345eb0,
                 .device_play_result = 0x003457c7,

                 .stream_close = 0x00335af0,
                 .stream_connect = 0x00335d70,
                 .stream_requested_stop = 0x0033661a,
                 .stream_requested_pause = 0x003366aa,
                 .stream_requested_play = 0x0033672e,
                 .stream_downstream_stop = 0x00336855,
                 .device_position_result = 0x003457a0,
                 .flush_position_result = 0x0033ebfb,
                 .stop_position_result = 0x0033f368,
                 .flush_lock_result = 0x0033ec3f,
                 .flush_unlock_result = 0x0033ec94,

                 .voice_assignment = 0x338f20,
                 .stream_queued = 0x338123,
                 .stream_due = 0x33894a,
                 .stream_due_end = 0x33894f,
                 .stream_deferred_start = 0x338a17,

                 .game_permission_resolved = 0x00138ddf,
                 .ordinary_admission = 0x0033b748,
                 .stream_slot_gate = 0x00337ff5,
                 .stream_bus_gate = 0x00338002,
                 .stream_bus_gain_gate = 0x0033800f,
                 .stream_sample_gate = 0x00338023,
                 .stream_pitch_gate = 0x00338048,
                 .stream_property_gate = 0x00338058,
                 .stream_gain_gate = 0x00338091,
                 .stream_queue_gate = 0x003380d4,
                 .stream_priority_gate = 0x0033817d,
                 .layered_queue_gate = 0x0033a3cb,
                 .flush_format = 0x00387878,
                 .sleep_import = 0x0036C0AC,
                 .source_route_software = 0x00347333,
                 .source_route_hardware = 0x0034725f,

                 .physical_route = 0x003469ac,
                 .ui_plain = 0x001a2150,
                 .ui_number = 0x001a2170,
                 .packet = 0x0034de20,
                 .virtual_end = 0x003393f2,
                 .downstream_end = 0x0033941d,
                 .saber_effects = 0x0028a0a0,
                 .source_end = 0x0034dbca,
                 .packet_complete = 0x0033db80,
                 .sound_reset = 0x001399d0,
                 .stop_all = 0x0033be70,
                 .melee_read = 0x001e9bc0,
                 .melee_controls = 0x00289780,

                 .binding_assign = 0x139130,
                 .fire_audio = 0x279B40,
                 .toggle_on = 0x28FC00,
                 .toggle_off = 0x28FC20},
};

// Builds native locations from RVAs and exact instruction preimages.
Layout make_layout(const ProfileSites &sites) {
    const auto call_bytes = [](std::uint32_t site, std::uint32_t target) {
        const auto displacement = target - site - 5U;
        return fc::exact_bytes({0xe8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8),
                                static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)});
    };
    return {
        .renderer_update = {.rva = sites.renderer_update,
                            .name = "DSBufferRenderer::UpdateObject",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28})},
        .renderer_flush = {.rva = sites.renderer_flush,
                           .name = "DSBufferRenderer::Flush",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10})},
        .renderer_write = {.rva = sites.renderer_write,
                           .name = "DSBufferRenderer::WriteData",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20})},
        .renderer_format = {.rva = sites.renderer_format,
                            .name = "DSBufferRenderer::SetFormat",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28})},
        .mixer_created = {.rva = sites.mixer_created, .name = "initialized mixer", .evidence = fc::exact_bytes({0x8B, 0x4D, 0xF4})},
        .mixer_closing = {.rva = sites.mixer_closing,
                          .name = "mixer before native Flush",
                          .evidence = fc::exact_bytes({0xE8, 0xE5, 0x00, 0x00, 0x00})},
        // Constructor has paused its owner; destructor has waited for the same owner before touching its list node.
        .renderer_created = {.rva = sites.renderer_created,
                             .name = "initialized renderer before owner resume",
                             .evidence = fc::exact_bytes({0x8B, 0xCF})},
        .renderer_closing = {.rva = sites.renderer_closing,
                             .name = "renderer after owner pause",
                             .evidence = fc::exact_bytes({0x8D, 0xB7, 0x38, 0x01, 0x00, 0x00})},
        .position_result = {.rva = sites.position_result,
                            .name = "GetCurrentPosition HRESULT",
                            .evidence = fc::exact_bytes({0x68, 0x0C, 0x01, 0x00, 0x00})},
        .consumed = {.rva = sites.consumed,
                     .name = "native play cursor delta",
                     .evidence = fc::exact_bytes({0x8B, 0x9E, 0x64, 0x01, 0x00, 0x00})},
        .cursor_reset = {.rva = sites.cursor_reset,
                         .name = "native lock cursor recovery",
                         .evidence = fc::exact_bytes({0x89, 0x96, 0x5C, 0x01, 0x00, 0x00})},
        .requested = {.rva = sites.requested, .name = "native lock byte request", .evidence = fc::exact_bytes({0x85, 0xD2})},
        .lock_result = {.rva = sites.lock_result, .name = "Lock HRESULT", .evidence = fc::exact_bytes({0x68, 0x69, 0x01, 0x00, 0x00})},
        .unlock_result = {.rva = sites.unlock_result,
                          .name = "Unlock HRESULT",
                          .evidence = fc::exact_bytes({0x8D, 0x8E, 0xF8, 0x00, 0x00, 0x00})},
        .queue_exhausted = {.rva = sites.queue_exhausted,
                            .name = "renderer exhausted queued packets",
                            .evidence = fc::exact_bytes({0x8A, 0x86, 0x8C, 0x01, 0x00, 0x00})},
        .zero_fill = {.rva = sites.zero_fill, .name = "renderer native silence fill", .evidence = fc::exact_bytes({0x8B, 0x5D, 0x0C})},

        .game_play = {.rva = sites.game_play, .name = "GameSound::Play", .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8})},
        .sound_play = {.rva = sites.sound_play, .name = "Sound::Play", .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x58})},
        .setup_voice = {.rva = sites.setup_voice,
                        .name = "Properties::SetupVoiceVirtual",
                        .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x53, 0x56})},
        .voice_start = {.rva = sites.voice_start,
                        .name = "VoiceVirtual::Play",
                        .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1})},
        .voice_stop = {.rva = sites.voice_stop,
                       .name = "VoiceVirtual::Stop",
                       .evidence = fc::exact_bytes({0x56, 0x8b, 0xf1, 0xba, 0xfa, 0xff, 0x00, 0x00})},
        .voice_inactive = {.rva = sites.voice_inactive,
                           .name = "FireForget inactive",
                           .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x0c})},
        .controller_play = {.rva = sites.controller_play,
                            .name = "GameSoundControllable::Play",
                            .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8})},
        .controller_stop = {.rva = sites.controller_stop,
                            .name = "GameSoundControllable::Stop",
                            .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1})},
        .owner_wait_before = {.rva = sites.owner_wait_before,
                              .name = "sound owner before semaphore wait",
                              .evidence = fc::exact_bytes({0xe8, 0xf2, 0x8a, 0x00, 0x00})},
        .owner_wait_after = {.rva = sites.owner_wait_after,
                             .name = "sound owner after semaphore wait",
                             .evidence = fc::exact_bytes({0x8d, 0x7e, 0x10, 0x33, 0xdb})},
        .owner_sleep_before = {.rva = sites.owner_sleep_before,
                               .name = "sound owner before Sleep",
                               .evidence = fc::masked_bytes({0xFF, 0x15, 0, 0, 0, 0}, {0xFF, 0xFF, 0, 0, 0, 0})},
        .owner_sleep_after = {.rva = sites.owner_sleep_after,
                              .name = "sound owner after Sleep",
                              .evidence = fc::exact_bytes({0x85, 0xdb, 0x75, 0xac, 0x8d, 0x4e, 0x14})},
        .game_permission = {.rva = sites.game_permission,
                            .name = "game permission",
                            .evidence = fc::exact_bytes({0x84, 0xc0, 0x0f, 0x84, 0x7b, 0x01, 0x00, 0x00})},
        .ordinary_property = {.rva = sites.ordinary_property,
                              .name = "ordinary property",
                              .evidence = fc::exact_bytes({0x84, 0xc0, 0x0f, 0x84, 0xcd, 0x01, 0x00, 0x00})},
        .ordinary_sample = {.rva = sites.ordinary_sample,
                            .name = "ordinary sample",
                            .evidence = fc::exact_bytes({0x89, 0x45, 0xe0, 0x85, 0xc0, 0x0f, 0x84, 0xb7, 0x01, 0x00, 0x00})},
        .ordinary_gain = {.rva = sites.ordinary_gain,
                          .name = "ordinary gain",
                          .evidence = fc::exact_bytes({0x80, 0x7d, 0x18, 0x00, 0x0f, 0x85, 0xf3, 0x00, 0x00, 0x00})},
        .ordinary_voice = {.rva = sites.ordinary_voice,
                           .name = "ordinary voice",
                           .evidence = fc::exact_bytes({0x85, 0xc0, 0x75, 0x0d, 0x85, 0xdb, 0x75, 0x39})},
        .layered_play = {.rva = sites.layered_play,
                         .name = "layered play",
                         .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x8b, 0x45, 0x0c})},
        .layered_bind = {.rva = sites.layered_bind,
                         .name = "layered bind",
                         .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x08})},
        .layered_due = {.rva = sites.layered_due, .name = "layered due", .evidence = fc::exact_bytes({0xe8, 0x7a, 0x09, 0x00, 0x00})},
        .layered_due_end = {.rva = sites.layered_due_end,
                            .name = "layered due end",
                            .evidence = fc::exact_bytes({0x8b, 0x4f, 0x3c, 0x8d, 0x57, 0x3c})},
        .stream_play = {.rva = sites.stream_play, .name = "stream play", .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x40})},
        .stream_setup = {.rva = sites.stream_setup,
                         .name = "stream setup",
                         .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08})},
        .stream_start = {.rva = sites.stream_start,
                         .name = "stream start",
                         .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1})},
        .device_play = {.rva = sites.device_play, .name = "device play", .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1})},
        .device_stop = {.rva = sites.device_stop, .name = "device stop", .evidence = fc::exact_bytes({0x8b, 0x01, 0x50, 0x8b, 0x08})},
        .device_play_result = {.rva = sites.device_play_result,
                               .name = "device play result",
                               .evidence = fc::exact_bytes({0x33, 0xc9, 0x85, 0xc0, 0x5f})},
        .stream_close = {.rva = sites.stream_close,
                         .name = "stream close",
                         .evidence = fc::exact_bytes({0x56, 0x57, 0x6a, 0x00, 0x8b, 0xf1})},
        .stream_connect = {.rva = sites.stream_connect,
                           .name = "stream connect",
                           .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x28})},
        .stream_requested_stop = {.rva = sites.stream_requested_stop,
                                  .name = "stream requested stop",
                                  .evidence = fc::exact_bytes({0x6a, 0x01, 0x6a, 0x01, 0x8b, 0xcb})},
        .stream_requested_pause = {.rva = sites.stream_requested_pause,
                                   .name = "stream requested pause",
                                   .evidence = fc::exact_bytes({0x0f, 0xb7, 0x83, 0xc8, 0x81, 0x0d, 0x00})},
        .stream_requested_play = {.rva = sites.stream_requested_play,
                                  .name = "stream requested play",
                                  .evidence = fc::exact_bytes({0x0f, 0xb7, 0x83, 0xc8, 0x81, 0x0d, 0x00})},
        .stream_downstream_stop = {.rva = sites.stream_downstream_stop,
                                   .name = "stream downstream stop",
                                   .evidence = fc::exact_bytes({0x56, 0x8d, 0x8b, 0x84, 0x00, 0x00, 0x00})},
        .device_position_result = {.rva = sites.device_position_result,
                                   .name = "device position result",
                                   .evidence = fc::exact_bytes({0x8a, 0x4d, 0x10, 0x33, 0xc0})},
        .flush_position_result = {.rva = sites.flush_position_result,
                                  .name = "flush position result",
                                  .evidence = fc::exact_bytes({0x68, 0x42, 0x02, 0x00, 0x00})},
        .stop_position_result = {.rva = sites.stop_position_result,
                                 .name = "stop position result",
                                 .evidence = fc::exact_bytes({0x68, 0x42, 0x02, 0x00, 0x00})},
        .flush_lock_result = {.rva = sites.flush_lock_result,
                              .name = "flush lock result",
                              .evidence = fc::masked_bytes({0x6a, 0x55, 0x68, 0, 0, 0, 0}, {0xff, 0xff, 0xff, 0, 0, 0, 0})},
        .flush_unlock_result = {.rva = sites.flush_unlock_result,
                                .name = "flush unlock result",
                                .evidence = fc::exact_bytes({0x8d, 0xbb, 0x70, 0x01, 0x00, 0x00})},
        .voice_assignment = {.rva = sites.voice_assignment,
                             .name = "voice assignment",
                             .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1})},
        .stream_queued = {.rva = sites.stream_queued, .name = "stream queued", .evidence = fc::exact_bytes({0xd9, 0x5e, 0x1c, 0x5e, 0x5b})},
        .stream_due = {.rva = sites.stream_due, .name = "stream due", .evidence = fc::exact_bytes({0xe8, 0x01, 0xfd, 0xff, 0xff})},
        .stream_due_end = {.rva = sites.stream_due_end,
                           .name = "stream due end",
                           .evidence = fc::exact_bytes({0x84, 0xc0, 0x0f, 0x84, 0xc0, 0x00, 0x00, 0x00})},
        .stream_deferred_start = {.rva = sites.stream_deferred_start,
                                  .name = "stream deferred start",
                                  // Empty, not-due and failed-setup branches enter this shared iteration continuation.
                                  .evidence = fc::exact_bytes({0x8b, 0x4d, 0xf0, 0x43, 0x83, 0x45, 0xf8, 0x0c})},
        .game_permission_resolved = {.rva = sites.game_permission_resolved,
                                     .name = "game permission resolved",
                                     .evidence = fc::exact_bytes({0x84, 0xc0, 0x75, 0xbd, 0x5f})},
        .ordinary_admission = {.rva = sites.ordinary_admission,
                               .name = "ordinary admission",
                               .evidence = fc::exact_bytes({0x85, 0xf6, 0x74, 0x7b, 0x8b, 0xcb})},
        .stream_slot_gate = {.rva = sites.stream_slot_gate,
                             .name = "stream slot gate",
                             .evidence = fc::exact_bytes({0x85, 0xf6, 0x0f, 0x84, 0x3d, 0x02, 0x00, 0x00})},
        .stream_bus_gate = {.rva = sites.stream_bus_gate,
                            .name = "stream bus gate",
                            .evidence = fc::exact_bytes({0x0f, 0x84, 0x32, 0x02, 0x00, 0x00})},
        .stream_bus_gain_gate = {.rva = sites.stream_bus_gain_gate,
                                 .name = "stream bus gain gate",
                                 .evidence = fc::exact_bytes({0x0f, 0x83, 0x25, 0x02, 0x00, 0x00})},
        .stream_sample_gate = {.rva = sites.stream_sample_gate,
                               .name = "stream sample gate",
                               .evidence = fc::exact_bytes({0x85, 0xff, 0x0f, 0x84, 0x0e, 0x02, 0x00, 0x00})},
        .stream_pitch_gate = {.rva = sites.stream_pitch_gate,
                              .name = "stream pitch gate",
                              .evidence = fc::exact_bytes({0x0f, 0x83, 0xeb, 0x01, 0x00, 0x00})},
        .stream_property_gate = {.rva = sites.stream_property_gate,
                                 .name = "stream property gate",
                                 .evidence = fc::exact_bytes({0x84, 0xc0, 0x0f, 0x84, 0xd9, 0x01, 0x00, 0x00})},
        .stream_gain_gate = {.rva = sites.stream_gain_gate,
                             .name = "stream gain gate",
                             .evidence = fc::exact_bytes({0x0f, 0x83, 0xa2, 0x01, 0x00, 0x00})},
        .stream_queue_gate = {.rva = sites.stream_queue_gate,
                              .name = "stream queue gate",
                              .evidence = fc::exact_bytes({0x85, 0xf6, 0x0f, 0x84, 0x5d, 0x01, 0x00, 0x00})},
        .stream_priority_gate = {.rva = sites.stream_priority_gate,
                                 .name = "stream priority gate",
                                 .evidence = fc::exact_bytes({0x0f, 0x82, 0xb6, 0x00, 0x00, 0x00})},
        .layered_queue_gate = {.rva = sites.layered_queue_gate,
                               .name = "layered queue gate",
                               .evidence = fc::exact_bytes({0x85, 0xff, 0x74, 0x18, 0xeb, 0x05})},
        .flush_lock_operand = {.rva = sites.flush_lock_result + 3,
                               .name = "native Lock diagnostic operand",
                               .evidence = fc::points_to(fc::Location{.image = std::string{sites.profile}, .rva = sites.flush_format})},
        .sleep_operand = {.rva = sites.owner_sleep_before + 2,
                          .name = "Sleep import operand",
                          .evidence = fc::points_to(fc::Location{.image = std::string{sites.profile}, .rva = sites.sleep_import})},
        .source_route_software = {.rva = sites.source_route_software,
                                  .name = "software voice mixer route",
                                  .evidence = fc::exact_bytes({0xe8, 0x68, 0x0d, 0x00, 0x00})},
        .source_route_hardware = {.rva = sites.source_route_hardware,
                                  .name = "hardware voice mixer route",
                                  .evidence = fc::exact_bytes({0xe8, 0x3c, 0x0e, 0x00, 0x00})},
        .physical_route = {.rva = sites.physical_route,
                           .name = "physical voice output route",
                           .evidence = fc::exact_bytes({0x83, 0xf8, 0x02, 0x8d, 0x8f, 0x90, 0x04, 0x00, 0x00})},
        // Both retail event loops use EDI=manager, ESI=Event before the type switch and before freeing the event.
        .ui_dispatch = {.rva = 0x00128fb0,
                        .name = "GuiManager::HandleEvents",
                        .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x81, 0xec, 0x80, 0, 0, 0})},
        .ui_event = {.rva = 0x0012901d, .name = "GUI event dispatch", .evidence = fc::exact_bytes({0x8b, 0x06, 0x83, 0xf8, 0x05})},
        .ui_event_end = {.rva = 0x001290fe, .name = "GUI event completed", .evidence = fc::exact_bytes({0x56, 0xb9})},
        // ECX=selected table name, EDX=callback name, including the popup/error routes.
        .ui_callbacks =
            {{{.rva = 0x00128dec, .name = "GUI popup callback", .evidence = call_bytes(0x00128dec, sites.ui_plain)},
              {.rva = 0x00128df7, .name = "GUI selected callback", .evidence = call_bytes(0x00128df7, sites.ui_plain)},
              {.rva = 0x00128e68, .name = "GUI error numeric callback", .evidence = call_bytes(0x00128e68, sites.ui_number)},
              {.rva = 0x00128e9c, .name = "GUI popup numeric callback", .evidence = call_bytes(0x00128e9c, sites.ui_number)},
              {.rva = 0x00128eab, .name = "GUI selected numeric callback", .evidence = call_bytes(0x00128eab, sites.ui_number)}}},
        // Complete the popup at the next call site, before its entry observer starts the selected callback.
        .ui_callback_ends =
            {{{.rva = 0x00128df7, .name = "GUI popup callback completed", .evidence = call_bytes(0x00128df7, sites.ui_plain)},
              {.rva = 0x00128dfc, .name = "GUI selected callback completed", .evidence = fc::exact_bytes({0x5e, 0x5d, 0xc2, 8, 0})},
              {.rva = 0x00128e6d,
               .name = "GUI error numeric callback completed",
               .evidence = fc::exact_bytes({0xf3, 0x0f, 0x10, 0x45, 0xfc})},
              {.rva = 0x00128ea1,
               .name = "GUI popup numeric callback completed",
               .evidence = fc::exact_bytes({0xf3, 0x0f, 0x10, 0x55, 0xfc})},
              {.rva = 0x00128eb0,
               .name = "GUI selected numeric callback completed",
               .evidence = fc::exact_bytes({0x5f, 0x5e, 0x5b, 0x8b, 0xe5})}}},
        .packet = {.rva = sites.packet,
                   .name = "StreamResampler::GetPacket",
                   .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c})},
        .voice_ends = {{{.rva = sites.virtual_end,
                         .name = "virtual voice reaches sample end",
                         .evidence = call_bytes(sites.virtual_end, sites.voice_stop)},
                        {.rva = sites.downstream_end,
                         .name = "virtual voice observes stopped output",
                         .evidence = call_bytes(sites.downstream_end, sites.voice_stop)}}},
        // UpdateEffects keeps EBX=weapon and its own unhooked EBP frame; these are actual controller calls.
        .saber_decisions = {{{.rva = sites.saber_effects + 0x73,
                              .name = "saber ambient starts",
                              .evidence = call_bytes(sites.saber_effects + 0x73, sites.controller_play)},
                             {.rva = sites.saber_effects + 0x8b,
                              .name = "saber ambient stops",
                              .evidence = call_bytes(sites.saber_effects + 0x8b, sites.controller_stop)},
                             {.rva = sites.saber_effects + 0xe6,
                              .name = "saber ambient properties change",
                              .evidence = call_bytes(sites.saber_effects + 0xe6, sites.controller_stop)},
                             {.rva = sites.saber_effects + 0x147,
                              .name = "saber mask adds active bits",
                              .evidence = call_bytes(sites.saber_effects + 0x147, sites.controller_stop)},
                             {.rva = sites.saber_effects + 0x194,
                              .name = "saber mask removes active bits",
                              .evidence = call_bytes(sites.saber_effects + 0x194, sites.controller_stop)}}},
        .source_end = {.rva = sites.source_end,
                       .name = "source completes sample or requested transport",
                       .evidence = call_bytes(sites.source_end, sites.packet_complete)},
        // Both global functions are cdecl with no arguments; reset calls StopAll before rebuilding sound state.
        .sound_reset = {.rva = sites.sound_reset,
                        .name = "game resets sound",
                        .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 0x34})},
        // Observing reset's CALL leaves StopAll's loop target at function+4 intact.
        .stop_all = {.rva = sites.sound_reset + 0x57,
                     .name = "sound manager stops all voices",
                     .evidence = call_bytes(sites.sound_reset + 0x57, sites.stop_all)},
        // Soldier::Update keeps EDI=soldier+0x240; health/max are -0xFC/-0xF8, threshold is properties+0x108C.
        .health_decisions =
            {{{.rva = 0x000e9996, .name = "soldier starts health warning sound", .evidence = call_bytes(0x000e9996, sites.controller_play)},
              {.rva = 0x000e99ae,
               .name = "soldier stops health warning sound",
               .evidence = call_bytes(0x000e99ae, sites.controller_stop)}}},
        // GuiManager::SetScreen queues initialized exit/enter events with EDI=event, EBX=manager and its native EBP frame.
        .ui_queued = {{{.rva = 0x00128523, .name = "GUI queues screen exit", .evidence = fc::exact_bytes({0x89, 0x38})},
                       {.rva = 0x001285c2, .name = "GUI queues screen entry", .evidence = fc::exact_bytes({0x89, 0x38})}}},
        // Paired retail Read methods take weapon in ECX and one stream argument, returning with RET 4.
        .melee_read = {.rva = sites.melee_read,
                       .name = "WeaponMelee::Read ambient binding",
                       .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x51, 0x56, 0x8b, 0x75, 0x08})},
        // OverrideSoldierControls takes only ECX=weapon and returns its existing bool result in AL.
        .melee_controls = {.rva = sites.melee_controls,
                           .name = "WeaponMelee::OverrideSoldierControls ambient binding",
                           .evidence = fc::exact_bytes({0x55, 0x8b, 0xec, 0x51, 0x53, 0x56, 0x8b, 0xf1})},
        // Common descriptor writers are filtered against Hero's already-bound weapon identity before sampling.
        .binding_assign = {.rva = sites.binding_assign,
                           .name = "ambient descriptor assignment",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08})},
        .effect_emit = {.rva = 0x11DC10,
                        .name = "native effect emitter",
                        .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        // ECX=Soldier+240; the no-argument function returns AL without stack cleanup.
        .ground_return = {.rva = 0xED080,
                          .name = "Soldier state-0 transition",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .toggle = {.rva = 0x8E520,
                   .name = "native toggle transition",
                   .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x80, 0x7D, 0x08, 0})},
        .fire_audio = {.rva = sites.fire_audio,
                       .name = "weapon fire descriptor copy",
                       .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8})},
        .ground_commit = {.rva = 0xED173,
                          .name = "Soldier state-0 commit result",
                          .evidence = fc::exact_bytes({0x84, 0xC0, 0x0F, 0x84, 0xBB, 0x01, 0, 0})},
        .ground_sounds = {{{.rva = 0xED23E, .name = "Soldier SoundClass+70 emission", .evidence = call_bytes(0xED23E, sites.game_play)},
                           {.rva = 0xED2E5, .name = "Soldier SoundClass+50 emission", .evidence = call_bytes(0xED2E5, sites.game_play)}}},
        // ECX retains the property object selected by 68E8C0; ESI names the same toggle object.
        .toggle_sounds = {{{.rva = 0x8E5AA, .name = "toggle property+26C emission", .evidence = call_bytes(0x8E5AA, sites.toggle_on)},
                           {.rva = 0x8E727, .name = "toggle property+274 emission", .evidence = call_bytes(0x8E727, sites.toggle_off)}}},
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

} // namespace fc_plugins::sound_diagnostics
