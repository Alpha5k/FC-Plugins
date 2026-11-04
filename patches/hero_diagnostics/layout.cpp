#include "layout.hpp"

#include <array>
#include <string>
#include <string_view>

namespace fc_plugins::hero_diagnostics {
namespace {

// Each client row holds image-relative RVAs; AnimatorOffsets separately describe offsets within an object.
struct ClientSites {
    std::string_view profile;

    std::uint32_t predict;
    std::uint32_t chase_setup;
    std::uint32_t update;
    std::uint32_t set_network_state;
    std::uint32_t enter_state;
    std::uint32_t animator_state;
    std::uint32_t input_queue;
    std::uint32_t sound_play;
    std::uint32_t soldier_read;
    std::uint32_t read;
    std::uint32_t exit_state;
    std::uint32_t action;
    std::uint32_t pose;
    std::uint32_t controls;
    std::uint32_t velocity;
    std::uint32_t parsed_move;
    std::uint32_t contact;
    std::uint32_t get_joystick_index;

    std::uint32_t host_turn;
    std::uint32_t client_turn;
    std::uint32_t acknowledged_turn;
    std::uint32_t prediction_turn;
    std::uint32_t destination;

    std::uint32_t local_prediction_pass;
    std::uint32_t authoritative_update_pass;

    std::uint32_t network_enabled;
    std::uint32_t frame_delta;
    std::uint32_t remote_moves;
    std::array<std::uint32_t, 2> locomotion_thresholds;
    std::array<std::uint32_t, 2> collision_thresholds;

    // The absolute scalar-load operand differs by image; its four bytes are masked during validation.
    std::array<std::uint8_t, 16> input_queue_bytes;
};

constexpr ClientSites kSteamSites{
    .profile = "BF2.Steam.59EDE353",
    .predict = {0x001BA810},
    .chase_setup = {0x000545B0},
    .update = {0x00289290},
    .set_network_state = {0x0028B100},
    .enter_state = {0x0028BC30},
    .animator_state = {0x0023F990},
    .input_queue = {0x0028B400},
    .sound_play = {0x00138010},
    .soldier_read = {0x001EA140},
    .read = {0x001E8B20},
    .exit_state = {0x0028BEF0},
    .action = {0x0023ED60},
    .pose = {0x0023FAA0},
    .controls = {0x00288700},
    .velocity = {0x00288C30},
    .parsed_move = {0x001D2BA4},
    .contact = {0x0028A550},
    .get_joystick_index = {0x001B73C0},
    .host_turn = {0x01BA8798},
    .client_turn = {0x01A64BA4},
    .acknowledged_turn = {0x01BA6594},
    .prediction_turn = {0x01BA87B4},
    .destination = {0x01BA8778},
    .local_prediction_pass = {0x01A62F10},
    .authoritative_update_pass = {0x01A64A2C},
    .network_enabled = {0x01A62EA9},
    .frame_delta = {0x01A56058},
    .remote_moves = {0x01AD6960},
    .locomotion_thresholds = {0x003B1F88, 0x003B2104},
    .collision_thresholds = {0x003B1E84, 0x003B204C},
    .input_queue_bytes = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0xF3, 0x0F, 0x10, 0x1D, 0x98, 0x1F, 0x7B, 0x00, 0x8B, 0xD1},
};

constexpr ClientSites kGogSites{
    .profile = "BF2.GOG.59EDF52B",
    .predict = {0x001BB7C0},
    .chase_setup = {0x00054590},
    .update = {0x0028A320},
    .set_network_state = {0x0028C190},
    .enter_state = {0x0028CCC0},
    .animator_state = {0x00240A30},
    .input_queue = {0x0028C490},
    .sound_play = {0x00138D80},
    .soldier_read = {0x001EB1E0},
    .read = {0x001E9BC0},
    .exit_state = {0x0028CF80},
    .action = {0x0023FE00},
    .pose = {0x00240B40},
    .controls = {0x00289780},
    .velocity = {0x00289CB0},
    .parsed_move = {0x001D3B24},
    .contact = {0x0028B5E0},
    .get_joystick_index = {0x001B8370},
    .host_turn = {0x01BA9C4C},
    .client_turn = {0x01A66054},
    .acknowledged_turn = {0x01BA7A48},
    .prediction_turn = {0x01BA9C68},
    .destination = {0x01BA9C2C},
    .local_prediction_pass = {0x01A643C0},
    .authoritative_update_pass = {0x01A65EDC},
    .network_enabled = {0x01A64359},
    .frame_delta = {0x01A574F0},
    .remote_moves = {0x01AD7E18},
    .locomotion_thresholds = {0x003B2F00, 0x003B307C},
    .collision_thresholds = {0x003B2DFC, 0x003B2FC4},
    .input_queue_bytes = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0xF3, 0x0F, 0x10, 0x1D, 0x10, 0x2F, 0x7B, 0x00, 0x8B, 0xD1},
};

constexpr std::array kProfiles{kSteamSites, kGogSites};

constexpr AnimatorOffsets kAnimatorOffsets{
    .extra_delta = 0xCC,
    .upper_time = 0x1600,
    .lower_time = 0x1FB0,
    .upper_timer = 0x1FD0,
    .lower_timer = 0x1FCC,
    .upper_blend = 0x1FE4,
    .lower_blend = 0x1FE0,
    .animation_rate = 0x1FFC,
    .upper_clip = 0x15E4,
    .lower_clip = 0x1F94,
    .action = 0x1FEC,
    .weapon_animation = 0x2014,
    .weapon_state = 0x78,
    .animation_flags = 0x201B,
    .upper_finished = 0x1610,
    .lower_finished = 0x1FC0,
};

// Locates the state-exit entry shared by client and server observations.
fc::FunctionLocation<StateFunction> exit_state_location(const ClientSites &sites) {
    return {.rva = sites.exit_state, .name = "WeaponMelee::ExitState"};
}

// Locates the velocity override entry for observing its four scalar references.
fc::FunctionLocation<VelocityFunction> velocity_location(const ClientSites &sites) {
    return {.rva = sites.velocity, .name = "WeaponMelee::OverrideSoldierVelocity"};
}

// Locates WeaponMelee::Deflect for observing its projectile and vector operands.
fc::FunctionLocation<ContactFunction> contact_location(const ClientSites &sites) {
    return {.rva = sites.contact, .name = "WeaponMelee::Deflect"};
}

// ClearInput starts a new input epoch; destruction and ResetObject bound native allocation reuse.
// Paired retail bodies receive complete objects and return without stack cleanup.
LifecycleLayout make_lifetime_layout(const ClientSites &sites) {
    return {
        .clear_input = {.rva = sites.set_network_state + 0x230,
                        .name = "WeaponMelee::ClearInput",
                        .evidence = fc::exact_bytes({0xC7, 0x81, 0x20, 0x01, 0, 0, 0, 0, 0, 0})},
        .weapon_destroy = {.rva = sites.controls - 0x2D0,
                           .name = "WeaponMelee destructor",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        .soldier_reset = {.rva = sites.soldier_read + 0x1080,
                          .name = "EntitySoldier::ResetObject",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14})},
        .soldier_destroy = {.rva = {0x000DFA20},
                            .name = "EntitySoldier destructor",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
    };
}

// Both retail evaluators receive window endpoints in XMM2/XMM3 and share five balanced RET 8 exits.
DetailLayout make_detail_layout(const ClientSites &sites) {
    DetailLayout layout{
        .locomotion_thresholds = sites.locomotion_thresholds,
        .condition_entry = {.rva = sites.enter_state - 0x190,
                            .name = "melee condition operands",
                            .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08})},
        .native_queue_ready = {.rva = sites.update + 0x1F6,
                               .name = "melee queue after native update",
                               .evidence = fc::exact_bytes({0x8B, 0x8B, 0x80, 0x01, 0x00, 0x00})},
        .consume_input = {.rva = sites.update + 0x973,
                          .name = "melee consume input threshold",
                          .evidence = fc::exact_bytes({0xE8, 0x48, 0x1C, 0x00, 0x00})},
        .base_read_return = {.rva = sites.read + 0x11,
                             .name = "melee base reader returned",
                             .evidence = fc::exact_bytes({0x6A, 0x05, 0x8D, 0x45, 0xFC})},
        // First site has all five stack arguments; second precedes the final root-address LEA/PUSH.
        .submissions = {{{.rva = 0x000E4AD9, .name = "body model submission", .evidence = fc::exact_bytes({0x8B, 0x01, 0xFF, 0x50, 0x04})},
                         {.rva = 0x000E4B01,
                          .name = "body model second submission",
                          .evidence = fc::exact_bytes({0x8D, 0x84, 0x24, 0x30, 0x02, 0x00, 0x00})}}},
        .submission_returns =
            {{{.rva = 0x000E4ADE, .name = "body model submission returned", .evidence = fc::exact_bytes({0x80, 0x7C, 0x24, 0x0E, 0})},
              {.rva = 0x000E4B0C,
               .name = "body model optional submission returned",
               .evidence = fc::exact_bytes({0x8B, 0x44, 0x24, 0x3C, 0x8B, 0x70, 0x58})}}},
        .render = {.rva = 0x000E23D0,
                   .name = "Soldier render invocation",
                   .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .pose_clock = {{{.rva = sites.pose + 0x68,
                         .name = "pose frame cache comparison",
                         .evidence = fc::exact_bytes({0x8B, 0xC8, 0x3B, 0xD1, 0x0F, 0x84, 0x77, 0x02, 0x00, 0x00})},
                        {.rva = sites.pose + 0x105,
                         .name = "pose clock advance",
                         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x8F, 0x94, 0x00, 0x00, 0x00})},
                        {.rva = sites.pose + 0xD12,
                         .name = "pose clock deferred",
                         .evidence = fc::exact_bytes({0x8B, 0x4D, 0x08, 0x8D, 0x87, 0xD0, 0x00, 0x00, 0x00})},
                        {.rva = sites.pose + 0xAA,
                         .name = "pose native tick and delta",
                         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x64, 0x24, 0x0C})}}},
        .jump_compare = {.rva = 0x000EDCB7,
                         .name = "jump projected speed comparison",
                         .evidence = fc::exact_bytes({0x76, 0x1F, 0xF3, 0x0F, 0x10, 0x80, 0xB8, 0x07, 0, 0})},
        .jump_cost = {.rva = 0x000EDCE5,
                      .name = "jump selected energy cost",
                      .evidence = fc::exact_bytes({0x56, 0x8D, 0xB2, 0x10, 0x0A, 0x00, 0x00})},
        .jump_results = {{{.rva = 0x000EDCF6,
                           .name = "jump energy admission returned",
                           .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x24, 0x8B, 0x82, 0x40, 0x02, 0, 0})},
                          {.rva = 0x000EDD0E,
                           .name = "jump movement attempt returned",
                           .evidence = fc::exact_bytes({0x84, 0xC0, 0x75, 0x13, 0xF3, 0x0F, 0x10, 0x4D, 0xFC})}}},
    };
    constexpr std::array<std::uint32_t, 5> returns{0x29, 0xDF, 0xF6, 0x12B, 0x152};
    for (std::size_t index{}; index < returns.size(); ++index) {
        layout.condition_returns[index] = {.rva = sites.enter_state - 0x190 + returns[index],
                                           .name = "melee condition result",
                                           .evidence = fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00})};
    }

    // Paired retail evaluators have identical control flow; only direct-call displacements differ.
    constexpr std::array<std::uint32_t, 5> calls{0x75, 0x9C, 0xBE, 0xE1, 0x106};
    constexpr std::array<std::uint32_t, 5> targets{0x00071240, 0x000712B0, 0x00071300, 0x00071400, 0};
    const auto predicate = sites.enter_state - 0x2B0;
    for (std::size_t index{}; index < calls.size(); ++index) {
        const auto target = index == 4 ? sites.enter_state - 0x360 : targets[index];
        layout.predicate_operands[index] = {.rva = predicate + calls[index],
                                            .name = "native condition operand call",
                                            .evidence =
                                                fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = target})};
    }
    layout.predicate_results = {
        {{.rva = predicate + 0x7A, .name = "posture native result", .evidence = fc::exact_bytes({0x84, 0xC0, 0x0F, 0x84, 0x95, 0, 0, 0})},
         {.rva = predicate + 0xA1,
          .name = "energy native result",
          .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x72, 0xF3, 0x0F, 0x10, 0x96, 0x20, 0x01, 0, 0})},
         {.rva = predicate + 0xC3,
          .name = "thrust native result",
          .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x50, 0xF3, 0x0F, 0x10, 0x9E, 0x2C, 0x01, 0, 0})},
         {.rva = predicate + 0xE6,
          .name = "velocity native result",
          .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x2D, 0x8B, 0x4F, 0x08})},
         {.rva = predicate + 0x10B, .name = "held button native result", .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x08, 0x5F})}}};
    layout.time_comparisons = {
        {{.rva = predicate + 0x36, .name = "condition upper window branch", .evidence = fc::exact_bytes({0x0F, 0x87, 0xDB, 0, 0, 0})},
         {.rva = predicate + 0x58, .name = "condition lower window branch", .evidence = fc::exact_bytes({0x0F, 0x87, 0xB9, 0, 0, 0})}}};

    const bool steam = sites.profile == kSteamSites.profile;
    // Controller input uses one stack delta; Trigger consumes XMM1 and a held byte.
    const auto controller = steam ? 0x0021A2B0U : 0x0021B320U;
    const auto trigger = steam ? 0x0003A950U : 0x0003A940U;
    layout.controller = {.rva = controller,
                         .name = "Player controller producer",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x08, 0x06, 0, 0})};
    layout.controller_selected = {
        .rva = controller + 0x241, .name = "Selected controller input", .evidence = fc::exact_bytes({0x8B, 0x8F, 0x88, 0, 0, 0})};
    layout.controller_reset = {.rva = controller + 0x226,
                               .name = "Disabled controller reset",
                               .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = 0x000837C0})};
    layout.forward_neutral = {.rva = controller + 0x451,
                              .name = "Forced forward neutral word",
                              .evidence = fc::exact_bytes({0x83, 0xC8, 0x1C, 0x89, 0x41, 0x68})};
    layout.trigger_entry = {
        .rva = trigger, .name = "Trigger custom ABI entry", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1})};
    // Keep each detour within its epilogue; the blocks immediately after RET are native branch targets.
    constexpr std::array<std::uint32_t, 8> trigger_exits{0x23, 0x3C, 0xDC, 0xED, 0x112, 0x147, 0x158, 0x163};
    for (std::size_t i{}; i < trigger_exits.size(); ++i) {
        layout.trigger_returns[i] = {.rva = trigger + trigger_exits[i],
                                     .name = "Trigger custom ABI return",
                                     .evidence = fc::exact_bytes({0x5E, 0x5D, 0xC2, 0x04, 0})};
    }
    layout.sprint_routes = {
        {{.rva = 0x000EAEAC,
          .name = "Stand forward edge shortcut",
          .evidence = fc::exact_bytes({0x8B, 0x47, 0x68, 0x83, 0xE0, 0x1F, 0x3C, 0x13, 0x75, 0x22})},
         {.rva = 0x000EAF58, .name = "Stand ordinary gates admitted", .evidence = fc::exact_bytes({0xE9, 0x59, 0xFF, 0xFF, 0xFF})},
         {.rva = 0x000EAEB8, .name = "Stand common sprint admission", .evidence = fc::exact_bytes({0xE8, 0xD3, 0x2E, 0, 0})},
         {.rva = 0x000EAEBD, .name = "Stand common admission returned", .evidence = fc::exact_bytes({0x84, 0xC0})}}};
    // The skipped reset and lock paths enter +F2 and +A1. Keep those branch targets outside displaced instructions.
    layout.controls_gates = {
        {{.rva = sites.controls + 0x39, .name = "Controls selected row", .evidence = fc::exact_bytes({0x8B, 0x43, 0x28})},
         {.rva = sites.controls + 0x98,
          .name = "Controls lock clock compare",
          .evidence = fc::exact_bytes({0x0F, 0x2F, 0x8E, 0x88, 0x01, 0, 0})},
         {.rva = sites.controls + 0xEA,
          .name = "Controls trigger reset",
          .evidence = fc::exact_bytes({0x83, 0xE1, 0xEC, 0x83, 0xC9, 0x0C})},
         {.rva = sites.controls + 0xFD, .name = "Controls scale row", .evidence = fc::exact_bytes({0x0F, 0xB6, 0x43, 0x24})},
         {.rva = sites.controls + 0x19E, .name = "Controls posture row", .evidence = fc::exact_bytes({0x0F, 0xB6, 0x5B, 0x1D})},
         {.rva = sites.controls + 0x219,
          .name = "Controls virtual sprint admission",
          .evidence = fc::exact_bytes({0x8B, 0x80, 0xAC, 0, 0, 0})},
         {.rva = sites.controls + 0x221, .name = "Controls virtual sprint returned", .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x19})},
         {.rva = sites.controls + 0xA1, .name = "Controls lock continuation", .evidence = fc::exact_bytes({0xF6, 0xC1, 0x01, 0x74, 0x1A})},
         {.rva = sites.controls + 0x1B1,
          .name = "Controls posture actual branch",
          .evidence = fc::exact_bytes({0x0F, 0x84, 0x69, 0x02, 0, 0})}}};
    layout.automatic_decisions = {
        {{.rva = sites.update + 0x2AD,
          .name = "Automatic duration actual branch",
          .evidence = fc::exact_bytes({0x0F, 0x82, 0xF2, 0x01, 0, 0})},
         {.rva = sites.update + 0x2BC,
          .name = "Automatic duration without end conditions",
          .evidence = fc::exact_bytes({0x89, 0x45, 0xDC})},
         {.rva = sites.update + 0x464,
          .name = "Automatic terminal selected condition",
          .evidence = fc::exact_bytes({0x85, 0xF6, 0x75, 0x08})},
         {.rva = sites.update + 0x8E6,
          .name = "Automatic posture fallback idle",
          .evidence = fc::exact_bytes({0x8B, 0xCB, 0xE8, 0x73, 0x23, 0, 0})},
         {.rva = sites.update + 0x97A, .name = "Automatic transition commit", .evidence = fc::exact_bytes({0xE8, 0xE1, 0x22, 0, 0})}}};
    // Observe before MOV ESP, EBP so each detour ends at RET and leaves the following block intact.
    layout.selector_returns = {{{.rva = steam ? 0x00241E6CU : 0x00242F0CU,
                                 .name = "Upper selector early return",
                                 .evidence = fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0})},
                                {.rva = steam ? 0x002420C7U : 0x00243167U,
                                 .name = "Upper selector completed return",
                                 .evidence = fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0})},
                                {.rva = steam ? 0x00242127U : 0x002431C7U,
                                 .name = "Lower selector early return",
                                 .evidence = fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0})},
                                {.rva = steam ? 0x00242328U : 0x002433C8U,
                                 .name = "Lower selector completed return",
                                 .evidence = fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0})}}};
    // Retail Soldier Update and state/admission helpers share RVAs and calling conventions in Steam and GOG.
    layout.soldier_state = {
        .rva = 0x000EE2C0, .name = "Soldier state commit", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1})};
    layout.sprint_admission = {
        .rva = 0x000EDD90, .name = "Soldier sprint admission", .evidence = fc::exact_bytes({0xF6, 0x81, 0x18, 0x0A, 0, 0, 1})};
    layout.notify = {.rva = steam ? 0x00288580U : 0x00289600U,
                     .name = "Melee soldier state notification",
                     .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x45, 0x0C})};
    layout.locomotion_decisions = {{
        {.rva = 0x000EAED8, .name = "stand sprint trigger", .evidence = fc::exact_bytes({0x8B, 0x47, 0x4C, 0x83, 0xE0, 0x0F})},
        {.rva = 0x000EAEF9, .name = "stand sprint cone result", .evidence = fc::exact_bytes({0x84, 0xC0, 0x0F, 0x84, 0x24, 0x16, 0, 0})},
        {.rva = 0x000EAF4F,
         .name = "stand sprint forward dot",
         .evidence = fc::exact_bytes({0x0F, 0x2F, 0xC8, 0x0F, 0x86, 0xCD, 0x15, 0, 0})},
        {.rva = 0x000EB166, .name = "sprint exit trigger", .evidence = fc::exact_bytes({0x8B, 0x47, 0x4C, 0x83, 0xE0, 0x0F})},
        {.rva = 0x000EB183,
         .name = "sprint exit cone result",
         .evidence = fc::exact_bytes({0x84, 0xC0, 0x74, 0x4E, 0xF3, 0x0F, 0x10, 0x87})},
        {.rva = 0x000EB1C6, .name = "sprint exit forward dot", .evidence = fc::exact_bytes({0x76, 0x0D, 0xE8, 0xC3, 0x2B, 0, 0})},
        {.rva = 0x000EB1CD,
         .name = "sprint exit admission result",
         .evidence = fc::exact_bytes({0x84, 0xC0, 0x0F, 0x85, 0x50, 0x13, 0, 0})},
        {.rva = 0x000EAE0D, .name = "stand missing ground clock", .evidence = fc::exact_bytes({0x76, 0x13, 0x6A, 0x08, 0x8B, 0xCE})},
        {.rva = 0x000EB0E3, .name = "sprint missing ground clock", .evidence = fc::exact_bytes({0x76, 0x13, 0x6A, 0x08, 0x8B, 0xCE})},
    }};
    layout.sprint_cone = {{
        {.rva = 0x000EE210, .name = "sprint control cone operands", .evidence = fc::exact_bytes({0x0F, 0x28, 0xC2, 0x0F, 0x28, 0xD9})},
        {.rva = 0x000EE22D, .name = "sprint control magnitude", .evidence = fc::exact_bytes({0x77, 0x38, 0x0F, 0x57, 0xC0})},
        {.rva = 0x000EE251, .name = "sprint control wide cone", .evidence = fc::exact_bytes({0x0F, 0x2F, 0xCA, 0x0F, 0x97, 0xC0})},
        {.rva = 0x000EE260, .name = "sprint control narrow cone", .evidence = fc::exact_bytes({0x0F, 0x2F, 0xCA, 0x0F, 0x97, 0xC0})},
    }};
    layout.animation_producer = {.rva = steam ? 0x0024087EU : 0x0024191EU,
                                 .name = "action animation restart operands",
                                 .evidence = fc::exact_bytes({0x0F, 0xB7, 0x8E, 0x1A, 0x20, 0, 0})};
    layout.movement_animation = {{
        {.rva = steam ? 0x00240F71U : 0x00242011U,
         .name = "movement animation direction operands",
         .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x00241A00U : 0x00242AA0U})},
        {.rva = steam ? 0x00240F76U : 0x00242016U,
         .name = "movement animation direction result",
         .evidence = fc::exact_bytes({0x80, 0x7C, 0x24, 0x18, 0, 0x8B, 0xC8})},
        {.rva = steam ? 0x00241923U : 0x002429C3U,
         .name = "movement animation final selection",
         .evidence = fc::exact_bytes({0x89, 0xBE, 0xF4, 0x1F, 0, 0, 0x66, 0x0B, 0xC8})},
    }};
    layout.motion_operands = {{
        {.rva = 0x000EE7AC, .name = "ground selected surface basis", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x97, 0xE0, 4, 0, 0})},
        {.rva = 0x000EE9F2, .name = "ground support admission", .evidence = fc::exact_bytes({0x8B, 0x8F, 0x3C, 4, 0, 0, 0x85, 0xC9})},
        {.rva = 0x000EEA05, .name = "ground support returned", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x6C, 0x24, 0x20})},
        {.rva = 0x000EEDCC, .name = "air support returned", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x64, 0x24, 0x14})},
        {.rva = steam ? 0x001FF7C3U : 0x00200863U,
         .name = "support movement type decision",
         .evidence = fc::exact_bytes({0x83, 0xB8, 0x2C, 1, 0, 0, 2})},
        {.rva = steam ? 0x001FF899U : 0x00200939U,
         .name = "support movement returned",
         .evidence = fc::exact_bytes({0x8B, 0x06, 0x8B, 0xCE})},
    }};
    layout.rotation_producers = {{
        {.rva = 0x000EEAD4,
         .name = "ground rotation helper operands",
         .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x00327C90U : 0x00328D60U})},
        {.rva = 0x000EEAD9, .name = "ground rotation helper returned", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x18})},
        {.rva = 0x000EEB64, .name = "ground rotated transform prepared", .evidence = fc::exact_bytes({0x8D, 0x8C, 0x24, 0x90, 0, 0, 0})},
        {.rva = 0x000EEB71, .name = "ground rotated transform applied", .evidence = fc::exact_bytes({0x5F, 0x5E, 0x8B, 0xE5, 0x5D})},
    }};
    layout.orientation = {{
        {.rva = steam ? 0x001E976CU : 0x001EA80CU,
         .name = "Soldier written orientation operands",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x20})},
        {.rva = steam ? 0x001EA2F7U : 0x001EB397U,
         .name = "Soldier decoded orientation operands",
         .evidence = fc::exact_bytes({0x8B, 0xCF, 0x50, 0x8D, 0x84, 0x24, 0x84, 0, 0, 0})},
    }};
    layout.energy_read = {.rva = steam ? 0x001E8030U : 0x001E90D0U,
                          .name = "EnergyBar native read",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x56, 0x57})};
    layout.energy_write = {.rva = steam ? 0x001E7FD0U : 0x001E9070U,
                           .name = "EnergyBar native write",
                           .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1})};
    layout.read_bits = {.rva = steam ? 0x001D9710U : 0x001DA7C0U,
                        .name = "NetPktGroup native read bits",
                        .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08})};
    layout.write_bits = {.rva = steam ? 0x001D9690U : 0x001DA740U,
                         .name = "NetPktGroup native write bits",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C})};
    layout.asset_name = {.rva = steam ? 0x00251F60U : 0x00253000U,
                         .name = "native loaded name registration",
                         .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53})};
    const auto base_read = sites.read - 0x650;
    layout.generic_timer = {
        {{.rva = base_read + 0xC3,
          .name = "generic weapon timer decode call",
          .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x001D9710U : 0x001DA7C0U})},
         {.rva = base_read + 0xC8,
          .name = "generic weapon timer decoded",
          .evidence = fc::exact_bytes({0x8B, 0x46, 0x6C, 0x80, 0xB8, 0x60, 0x01, 0x00, 0x00, 0x00})}}};
    layout.generic_timer_write = {
        .rva = steam ? 0x001E8397U : 0x001E9437U,
        .name = "generic weapon raw timer write",
        .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x001D9690U : 0x001DA740U})};
    layout.simulation = {
        .rva = 0x000E8920, .name = "Soldier native simulation", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})};
    layout.post_collision = {
        .rva = 0x000E6660, .name = "Soldier post collision update", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})};
    layout.collision_thresholds = sites.collision_thresholds;
    layout.collision_decisions = {
        {{.rva = 0x000E6707, .name = "contact accumulator magnitude decision", .evidence = fc::exact_bytes({0x0F, 0x86, 0x87, 0, 0, 0})},
         {.rva = 0x000E6772,
          .name = "contact normal slope decision",
          .evidence = fc::exact_bytes({0x76, 0x20, 0x8A, 0x87, 0xD4, 0x09, 0, 0})},
         {.rva = 0x000E6780, .name = "contact normal selected", .evidence = fc::exact_bytes({0x66, 0x0F, 0xD6, 0x9F, 0x4C, 0x04, 0, 0})}}};
    layout.integration = {{
        {.rva = 0x000EE590, .name = "ground integration operands", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        {.rva = 0x000EEB90, .name = "air integration operands", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        {.rva = 0x000EE578, .name = "selected gravity coefficient", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x59, 0xC2, 0x5E})},
        {.rva = 0x000EE57D,
         .name = "gravity product before sign",
         .evidence = fc::masked_bytes({0x0F, 0x57, 0x05, 0, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0, 0, 0, 0})},
        {.rva = 0x000EE5FD,
         .name = "ground normal projection decision",
         .evidence = fc::exact_bytes({0x76, 0x60, 0xF3, 0x0F, 0x10, 0x8F, 0x4C, 0x04, 0, 0})},
        {.rva = 0x000EE65F,
         .name = "ground normal projection result",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x87, 0x50, 0x04, 0x00, 0x00})},
        {.rva = 0x000EEA25,
         .name = "ground gravity applied velocity",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x87, 0xE0, 0x04, 0x00, 0x00})},
        {.rva = 0x000EEAA8, .name = "ground position applied", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x20})},
        {.rva = 0x000EEBFD,
         .name = "air normal projection decision",
         .evidence = fc::exact_bytes({0x76, 0x60, 0xF3, 0x0F, 0x10, 0x8E, 0x4C, 0x04, 0, 0})},
        {.rva = 0x000EEC5F,
         .name = "air normal projection result",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x8E, 0xE0, 0x04, 0x00, 0x00})},
        {.rva = 0x000EECCC,
         .name = "air damping multiplier",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x86, 0xDC, 0x04, 0x00, 0x00})},
        {.rva = 0x000EEDB9, .name = "air thrust applied", .evidence = fc::exact_bytes({0x8B, 0x8E, 0x3C, 0x04, 0x00, 0x00})},
        {.rva = 0x000EEDEC,
         .name = "air gravity applied velocity",
         .evidence = fc::exact_bytes({0xF3, 0x0F, 0x11, 0x86, 0xE0, 0x04, 0x00, 0x00})},
        {.rva = 0x000EEE6E, .name = "air position applied", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x14})},
    }};
    layout.gravity_sign_operand = {
        .rva = 0x000EE580,
        .name = "gravity sign mask operand",
        .evidence = fc::points_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x003B2980U : 0x003B38F0U})};
    const auto combat = steam ? 0x0028C230U : 0x0028D2C0U;
    layout.combat_update = {
        .rva = combat, .name = "WeaponMelee native UpdateFire", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})};
    layout.combat_decisions = {{
        {.rva = combat + 0x285, .name = "melee previous window branch", .evidence = fc::exact_bytes({0x0F, 0x82, 0x39, 0x0C, 0x00, 0x00})},
        {.rva = combat + 0x296, .name = "melee current window branch", .evidence = fc::exact_bytes({0x0F, 0x86, 0x28, 0x0C, 0x00, 0x00})},
        {.rva = combat + 0xAF7,
         .name = "melee candidate collision result",
         .evidence = fc::exact_bytes({0x89, 0x44, 0x24, 0x14, 0x85, 0xC0})},
        {.rva = combat + 0xB1F,
         .name = "melee repeated target branch",
         .evidence = fc::exact_bytes({0x3B, 0xCF, 0x0F, 0x8C, 0x7F, 0x01, 0x00, 0x00})},
        {.rva = combat + 0xC92,
         .name = "melee actual sweep result branch",
         .evidence = fc::exact_bytes({0x77, 0x2E, 0x40, 0x83, 0xC7, 0x1C})},
        {.rva = combat + 0xCCF, .name = "melee native target budget branch", .evidence = fc::exact_bytes({0x83, 0xF8, 0x08, 0x7D, 0xD2})},
        {.rva = combat + 0xD7D,
         .name = "melee block prepared call",
         .evidence = fc::exact_bytes({0x8B, 0x06, 0x8B, 0x80, 0xD4, 0x00, 0x00, 0x00})},
        {.rva = combat + 0xD87,
         .name = "melee block returned",
         .evidence = fc::exact_bytes({0x84, 0xC0, 0x0F, 0x85, 0x17, 0xFF, 0xFF, 0xFF})},
        {.rva = combat + 0xDAF,
         .name = "melee damage prepared call",
         .evidence = fc::exact_bytes({0x8D, 0x94, 0x24, 0x98, 0x01, 0x00, 0x00})},
        {.rva = combat + 0xDBA, .name = "melee damage returned", .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x70})},
        {.rva = combat + 0xDCE,
         .name = "melee knockback prepared call",
         .evidence = fc::exact_bytes({0x8D, 0x8C, 0x24, 0xD0, 0x00, 0x00, 0x00})},
    }};
    const auto upper = steam ? 0x00241E10U : 0x00242EB0U;
    const auto lower = steam ? 0x002420D0U : 0x00243170U;
    layout.animation_entry = {
        {{.rva = upper, .name = "upper animation selection entry", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C})},
         {.rva = lower, .name = "lower animation selection entry", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24})}}};
    layout.animation_selection = {
        {{.rva = upper + 0x1BD,
          .name = "upper selected animation start",
          .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x0032D430U : 0x0032E500U})},
         {.rva = lower + 0x1AE,
          .name = "lower selected animation start",
          .evidence = fc::direct_call_to(fc::Location{.image = std::string(sites.profile), .rva = steam ? 0x0032D430U : 0x0032E500U})}}};
    const auto advance = steam ? 0x0032D790U : 0x0032E860U;
    layout.animation_advance = {
        .rva = advance, .name = "animation instance native advance", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0xD1})};
    return layout;
}

// Builds native locations from one client profile; instruction sites are observed before displaced instructions execute.
ClientLayout make_client_layout(const ClientSites &sites) {
    const fc::FunctionLocation<GameSoundPlayFunction> sound_play{
        .rva = sites.sound_play,
        .name = "GameSound::Play",
    };
    return {
        .lifetime = make_lifetime_layout(sites),
        .detail = make_detail_layout(sites),
        .predict = {.rva = sites.predict,
                    .name = "NetGame::Predict",
                    .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x80, 0, 0, 0})},
        // Concrete Chase callback: ECX camera, one RedCamera pointer stack input, RET 4.
        .chase_setup = {.rva = sites.chase_setup,
                        .name = "ChaseCamera::SetupInterpolatedCamera",
                        .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0})},
        .animator = kAnimatorOffsets,
        .update = {.rva = sites.update, .name = "WeaponMelee::Update"},
        .set_network_state =
            {
                .rva = sites.set_network_state,
                .name = "WeaponMelee::SetNetworkState",
                .evidence =
                    fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7D, 0x08, 0x3B, 0xBE, 0x80, 0x01, 0x00, 0x00}),
            },
        .enter_state = {.rva = sites.enter_state, .name = "WeaponMelee::EnterState"},
        .animator_state = {.rva = sites.animator_state, .name = "SoldierAnimator::SetWeaponComboState"},
        .input_queue =
            {
                .rva = sites.input_queue,
                .name = "WeaponMelee::InputQueue::Update",
                .evidence =
                    fc::masked_bytes(sites.input_queue_bytes, std::array<std::uint8_t, 16>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                                           0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF}),
            },
        .prediction_transition =
            {
                .rva = (sites.update + 0x97A),
                .name = "combo state transition calls",
                .evidence = fc::exact_bytes({0xE8, 0xE1, 0x22, 0x00, 0x00, 0x56, 0x8B, 0xCB, 0xE8, 0x19, 0x20, 0x00, 0x00}),
            },
        .transition_sound =
            {
                .rva = (sites.enter_state + 0x210),
                .name = "combo transition sound call",
                .evidence = fc::direct_call_to(fc::Location{.image = std::string{sites.profile}, .rva = sound_play.rva}),
            },
        .soldier_read = {.rva = sites.soldier_read, .name = "EntitySoldier::ReadObject"},
        .read = {.rva = sites.read, .name = "WeaponMelee::Read"},
        .exit_state = exit_state_location(sites),
        .action = {.rva = sites.action, .name = "SoldierAnimator::SetAction"},
        .pose = {.rva = sites.pose, .name = "SoldierAnimator::SetupPose"},
        .controls =
            {
                .rva = sites.controls,
                .name = "WeaponMelee::OverrideSoldierControls",
                .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x56, 0x6C}),
            },
        .velocity = velocity_location(sites),
        .parsed_move =
            {
                .rva = sites.parsed_move,
                .name = "parsed remote player move",
                .evidence = fc::exact_bytes({0xE9, 0xDD, 0xFE, 0xFF, 0xFF}),
            },
        .contact = contact_location(sites),
        .jump = {.rva = {0x000EDC60U}, .name = "EntitySoldier::JumpUsingEnergy"},
        .roll =
            {
                .rva = {0x000EDD30U},
                .name = "EntitySoldier::RollUsingEnergy",
                .evidence =
                    fc::exact_bytes({0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x86, 0x40, 0x04, 0x00, 0x00, 0x8D, 0x8E, 0x10, 0x0A, 0x00, 0x00}),
            },
        .get_joystick_index = {.rva = sites.get_joystick_index, .name = "NetGame::GetJoystickIndex"},
        .host_turn = {.rva = sites.host_turn, .name = "host turn"},
        .client_turn = {.rva = sites.client_turn, .name = "client turn"},
        .update_turn = {.rva = (sites.acknowledged_turn + 0x1C), .name = "native update turn"},
        .prediction_turn = {.rva = sites.prediction_turn, .name = "prediction turn"},
        .acknowledged_turn = {.rva = sites.acknowledged_turn, .name = "acknowledged turn"},
        .destination = {.rva = sites.destination, .name = "network destination"},
        .local_prediction_pass = {.rva = sites.local_prediction_pass, .name = "local prediction pass"},
        .authoritative_update_pass = {.rva = sites.authoritative_update_pass, .name = "native update pass"},
        .rollback = {.rva = (sites.local_prediction_pass + 1), .name = "rollback pass"},
        .network_enabled = {.rva = sites.network_enabled, .name = "network enabled"},
        .network_client_active = {.rva = (sites.network_enabled + 2), .name = "network client active"},
        .frame_delta = {.rva = sites.frame_delta, .name = "client frame delta in seconds"},
        .remote_moves = {.rva = sites.remote_moves, .name = "remote player moves"},
    };
}

// The GOG dedicated server shares melee entries with its client image and adds Write, prepared input and readiness boundaries.
ServerLayout make_server_layout() {
    return {
        .lifetime = make_lifetime_layout(kGogSites),
        .detail = make_detail_layout(kGogSites),
        // The retained path has copied all six words; EBP-4 is player and EBP-C is candidate turn.
        .readiness_retained = {.rva = {0x001BAC50},
                               .name = "readiness retained move",
                               .evidence = fc::exact_bytes({0x69, 0x55, 0xFC, 0x08, 0x02, 0, 0})},
        // Observe the default branch before its copy; the shared loop jump remains outside the displaced instructions.
        .readiness_default = {.rva = {0x001BAC6F},
                              .name = "readiness default move",
                              .evidence = fc::exact_bytes({0x69, 0x55, 0xEC, 0x00, 0x06, 0x00, 0x00})},
        .readiness_publication = {.rva = {0x001BAED2},
                                  .name = "active host input publication",
                                  .evidence = fc::exact_bytes({0x8B, 0x45, 0x9C, 0x8B, 0x0A, 0x89, 0x08})},
        .late_replay = {.rva = {0x001BB0C0},
                        .name = "NetGame::ApplyLateMoves",
                        .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54})},
        .active_inputs = {.rva = {0x01A65EE0}, .name = "active host processed inputs"},
        .update = {.rva = {0x0028A320U}, .name = "WeaponMelee::Update"},
        .enter_state = {.rva = {0x0028CCC0U}, .name = "WeaponMelee::EnterState"},
        .exit_state = exit_state_location(kGogSites),
        .controls = {.rva = kGogSites.controls, .name = "WeaponMelee::OverrideSoldierControls"},
        .velocity = velocity_location(kGogSites),
        .write = {.rva = {0x001E9B00U}, .name = "WeaponMelee::Write"},
        .contact = contact_location(kGogSites),
        .host_turn = {.rva = {0x01BA9C4CU}, .name = "host turn"},
        .destination = {.rva = {0x01BA9C2CU}, .name = "network destination"},
    };
}

} // namespace

std::optional<ClientLayout> select_client_layout(const std::string_view profile) {
    for (const auto &row : kProfiles) {
        if (row.profile == profile) {
            return make_client_layout(row);
        }
    }

    return std::nullopt;
}

std::optional<ServerLayout> select_server_layout(const std::string_view profile) {
    if (profile != "BF2.GOG.59EDF52B") {
        return std::nullopt;
    }

    return make_server_layout();
}

} // namespace fc_plugins::hero_diagnostics
