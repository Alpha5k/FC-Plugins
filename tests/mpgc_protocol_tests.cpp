#include "../patches/multiplayer_gc/protocol.hpp"
#include "../patches/multiplayer_gc/discovery.hpp"
#include "../patches/multiplayer_gc/shell.hpp"
#include "../patches/multiplayer_gc/timers.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

namespace {
namespace gc = fc_plugins::multiplayer_gc;

TEST_CASE("GC campaign messages preserve the native binary pair", "[mpgc]") {
    gc::Message message{.kind = gc::MessageKind::CurrentState};
    message.snapshot = {
        .cooperative = false, .host_faction = 2, .local_faction = 1, .state = {std::string(512, '\0'), std::string(256, 'm')}};
    const auto decoded = gc::decode_message(gc::encode_message(message));
    REQUIRE(decoded);
    CHECK(decoded->snapshot.host_faction == 2);
    CHECK(decoded->snapshot.local_faction == 0);
    CHECK(decoded->snapshot.state.campaign == message.snapshot.state.campaign);
    CHECK(decoded->snapshot.state.mission == message.snapshot.state.mission);
    message.decision = {.id = 5, .turn = 8, .faction = 2, .screen = "ifs_freeform_fleet", .pending_card = 3};
    const auto current = gc::decode_message(gc::encode_message(message));
    REQUIRE(current);
    CHECK(current->decision.id == 5);
    CHECK(current->decision.turn == 8);
    CHECK(current->decision.screen == "ifs_freeform_fleet");
    CHECK(current->snapshot.state == message.snapshot.state);
    auto incomplete = gc::encode_message(message);
    incomplete.pop_back();
    CHECK_FALSE(gc::decode_message(incomplete));
}

TEST_CASE("GC complete roster preserves join order, visitors and faction commanders", "[mpgc]") {
    gc::Message message{.kind = gc::MessageKind::Roster};
    message.roster = {{{0, 1, true, "Host"}, {63, 0, false, "Visitor"}, {7, 2, false, "Opponent"}}, {0, 7}};
    const auto decoded = gc::decode_message(gc::encode_message(message));
    REQUIRE(decoded);
    CHECK(decoded->roster.players == message.roster.players);
    CHECK(decoded->roster.commanders == message.roster.commanders);
    message.roster.players.erase(message.roster.players.begin() + 1);
    const auto replacement = gc::decode_message(gc::encode_message(message));
    REQUIRE(replacement);
    CHECK(replacement->roster.players.size() == 2);
    CHECK(replacement->roster.players.back().slot == 7);

    message.roster.players.push_back({7, 0, false, "Duplicate slot"});
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
    message.roster.players.pop_back();
    message.roster.commanders = {7, 0};
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
}

TEST_CASE("GC pending values retain only current state after FC refuses a send", "[mpgc]") {
    gc::PendingMessages pending;
    gc::Message state{.kind = gc::MessageKind::CurrentState, .snapshot = {.host_faction = 1, .state = {"old", ""}}};
    pending.replace(state);
    pending.replace({.kind = gc::MessageKind::Roster, .roster = {{{0, 1, true, "Host"}}, {0, -1}}});
    pending.replace({.kind = gc::MessageKind::BattleSetup, .battle = {.id = 9, .state = {"battle", "mission"}}});
    unsigned refused_calls{};
    const auto refused = pending.flush([&](const auto &) {
        ++refused_calls;
        return fc::SendResult::Full;
    });
    CHECK(refused == fc::SendResult::Full);
    CHECK(refused_calls == 1);
    CHECK_FALSE(pending.empty());
    state.snapshot.state.campaign = "current";
    pending.replace(state);
    std::vector<gc::Message> received;
    CHECK_FALSE(pending.flush([&](const auto &bytes) {
        const auto decoded = gc::decode_message(bytes);
        REQUIRE(decoded);
        received.push_back(*decoded);
        return fc::SendResult::Accepted;
    }));
    REQUIRE(received.size() == 3);
    CHECK(received[0].kind == gc::MessageKind::Roster);
    CHECK(received[1].kind == gc::MessageKind::CurrentState);
    CHECK(received[1].snapshot.state.campaign == "current");
    CHECK(received[2].kind == gc::MessageKind::BattleSetup);
    CHECK(received[2].battle.id == 9);
    CHECK(pending.empty());
    unsigned repeated{};
    pending.flush([&](const auto &) {
        ++repeated;
        return fc::SendResult::Accepted;
    });
    CHECK(repeated == 0);
}

TEST_CASE("GC faction chat excludes opposing and unassigned native slots", "[mpgc]") {
    const std::array players{gc::Player{0, 1, true, "host"}, gc::Player{2, 1, false, "ally"}, gc::Player{3, 2, false, "opponent"},
                             gc::Player{63, 0, false, "choosing"}};
    const auto team = gc::chat_exclusions(players, 2, true);
    CHECK((team & (1ull << 0)) == 0);
    CHECK((team & (1ull << 2)) == 0);
    CHECK((team & (1ull << 3)) != 0);
    CHECK((team & (1ull << 63)) != 0);
    CHECK(gc::chat_exclusions(players, 63, true) == UINT64_MAX);
    CHECK(gc::chat_exclusions(players, 2, false) == ~(1ull | (1ull << 2) | (1ull << 3) | (1ull << 63)));
}

TEST_CASE("GC browser reads mode and era from complete version-three metadata", "[mpgc]") {
    const std::vector expected{std::byte{77}, std::byte{71}, std::byte{67}, std::byte{16}, std::byte{0}, std::byte{2},
                               std::byte{7},  std::byte{0},  std::byte{0},  std::byte{0},  std::byte{1}, std::byte{0},
                               std::byte{50}, std::byte{1},  std::byte{0},  std::byte{103}};
    auto complete_expected = expected;
    for (const auto value : std::array<unsigned char, 20>{0, 0, 0, 0, 0, 0, 0, 10, 0, 71, 97, 108, 97, 120, 121, 32, 77, 97, 112, 1}) {
        complete_expected.push_back(static_cast<std::byte>(value));
    }
    complete_expected.push_back(std::byte{16});
    CHECK(gc::encode_metadata(
              {.count = 16, .cooperative = false, .era = "g", .campaign = "2", .turn = 7, .host_faction = 2, .maximum = 16}) ==
          complete_expected);
    const auto versus = gc::decode_metadata("FC1:3:TUdDEAACBwAAAAEAMgEAZwAAAAAAAAAKAEdhbGF4eSBNYXABEA==");
    REQUIRE(versus);
    CHECK(versus->count == 16);
    CHECK(versus->maximum == 16);
    CHECK_FALSE(versus->cooperative);
    CHECK(versus->era == "g");
    CHECK(versus->campaign == "2");
    CHECK(versus->turn == 7);
    CHECK(versus->host_faction == 2);
    const auto cooperative = gc::decode_metadata("FC1:3:TUdDCAEBAQAAAAIAMTIBAGMAAAAAAAAACgBHYWxheHkgTWFwAQg=");
    REQUIRE(cooperative);
    CHECK(cooperative->count == 8);
    CHECK(cooperative->maximum == 8);
    CHECK(cooperative->cooperative);
    CHECK(cooperative->era == "c");
    CHECK(cooperative->campaign == "12");
    CHECK(cooperative->turn == 1);
    CHECK(cooperative->host_faction == 1);
}

TEST_CASE("GC empty-map rows reach the stock type and options copy only with qualified metadata", "[mpgc]") {
    std::array<std::byte, 0x180> row{};
    fc::write_field<std::uint64_t>(row.data(), 0x14, 42);
    const auto lookup = [](std::uint64_t lobby) {
        return lobby == 42 ? gc::decode_metadata("FC1:3:TUdDEAACBwAAAAEAMgEAZwAAAAAAAAAKAEdhbGF4eSBNYXABEA==") : std::nullopt;
    };
    // Retail's TEST EAX/EAX skips type/password/version/ping when the advertised map begins with zero.
    CHECK(fc::read_field<std::int32_t>(row.data(), 0xD0) == 0);
    CHECK(gc::gc_map_condition(0, true, row.data(), lookup) == 1);
    CHECK(gc::gc_map_condition(0, false, row.data(), lookup) == 0);
    CHECK(gc::gc_map_condition('m', true, row.data(), lookup) == 'm');
    fc::write_field<std::uint8_t>(row.data(), 0x1C, 1);
    CHECK(gc::gc_map_condition(0, true, row.data(), lookup) == 0);
    fc::write_field<std::uint8_t>(row.data(), 0x1C, 0);
    fc::write_field<std::uint64_t>(row.data(), 0x14, 43);
    CHECK(gc::gc_map_condition(0, true, row.data(), lookup) == 0);
}

TEST_CASE("GC source transitions restore both native sort fields without toggling the same mode", "[mpgc]") {
    std::array<gc::BrowserSortState, 2> sources{};
    auto restored = gc::browser_sort_transition(false, true, {5, 0}, sources);
    REQUIRE(restored);
    CHECK(restored->mode == 2);
    CHECK(restored->direction == 1);
    restored = gc::browser_sort_transition(true, false, {2, 0}, sources);
    REQUIRE(restored);
    CHECK(restored->mode == 5);
    CHECK(restored->direction == 0);
    restored = gc::browser_sort_transition(false, true, {2, 1}, sources);
    REQUIRE(restored);
    CHECK(restored->mode == 2);
    CHECK(restored->direction == 0);
    CHECK_FALSE(gc::browser_sort_transition(true, true, {7, 1}, sources));
    restored = gc::browser_sort_transition(true, false, {7, 1}, sources);
    REQUIRE(restored);
    CHECK(restored->mode == 2);
    CHECK(restored->direction == 1);
}

TEST_CASE("GC filters preserve native session ordinals and stock filter flags", "[mpgc]") {
    std::array<std::byte, 0x70> browser{};
    std::array<std::array<std::byte, 0x180>, 5> rows{};
    std::array<std::array<std::byte, 16>, 6> nodes{};
    fc::write_field<void *>(browser.data(), 0x10, nodes[0].data());
    fc::write_field<std::int32_t>(browser.data(), 0x1C, 5);
    fc::write_field<std::int32_t>(browser.data(), 0x64, 3);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        fc::write_field<void *>(nodes[index].data(), 4, nodes[index + 1].data());
        fc::write_field<void *>(nodes[index].data(), 0xC, rows[index].data());
        fc::write_field<std::uint8_t>(rows[index].data(), 0x10, index != 0);
        fc::write_field<std::uint64_t>(rows[index].data(), 0x14, index);
        fc::write_field<std::int32_t>(rows[index].data(), 0xD0, 1);
        fc::write_field<std::uint16_t>(rows[index].data(), 0xD4, static_cast<std::uint16_t>(4 - index));
    }
    fc::write_field<std::uint8_t>(rows[1].data(), 0xD7, 1);
    const auto lookup = [](std::uint64_t lobby) -> std::optional<gc::SessionInfo> {
        if (lobby == 4) {
            return std::nullopt;
        }
        return gc::SessionInfo{1, lobby == 2, lobby == 2 ? "c" : "g"};
    };
    auto first = gc::filter_browser_rows(browser.data(), {"Galaxy Map", "Versus", "g"}, 7, true, lookup);
    REQUIRE(first);
    CHECK(*first == 2);
    CHECK(fc::read_field<std::int32_t>(browser.data(), 8) == 1);
    CHECK(fc::read_field<std::int32_t>(browser.data(), 0x64) == 3);
    CHECK(fc::read_field<std::uint16_t>(rows[3].data(), 0xD4) == 0);
    CHECK(fc::read_field<std::uint8_t>(rows[1].data(), 0xD7) == 1);
    CHECK(fc::read_field<std::uint8_t>(rows[2].data(), 0xD7) == 1);
    CHECK(fc::read_field<std::uint8_t>(rows[4].data(), 0xD7) == 1);

    // Original resort refreshes stock flags before each GC pass; changing filters must reveal matching rows again.
    fc::write_field<std::uint8_t>(rows[2].data(), 0xD7, 0);
    fc::write_field<std::uint16_t>(rows[2].data(), 0xD4, 1);
    first = gc::filter_browser_rows(browser.data(), {}, 7, true, lookup);
    REQUIRE(first);
    CHECK(*first == 1);
    CHECK(fc::read_field<std::uint16_t>(rows[2].data(), 0xD4) == 0);
    CHECK(fc::read_field<std::uint16_t>(rows[3].data(), 0xD4) == 1);
    first = gc::filter_browser_rows(browser.data(), {}, 7, false, lookup);
    REQUIRE(first);
    CHECK(*first == 2);
    CHECK(fc::read_field<std::int32_t>(browser.data(), 8) == 2);
    fc::write_field<std::uint8_t>(rows[2].data(), 0xF0, 1);
    first = gc::filter_browser_rows(browser.data(), {}, 7, false, lookup);
    REQUIRE(first);
    CHECK(*first == 1);
    first = gc::filter_browser_rows(browser.data(), {"", "", "c"}, 6, true, lookup);
    REQUIRE(first);
    CHECK(*first == 1);
    first = gc::filter_browser_rows(browser.data(), {"Other map", "", ""}, 5, true, lookup);
    CHECK_FALSE(first);
    CHECK(fc::read_field<std::int32_t>(browser.data(), 8) == 0);
}

TEST_CASE("GC browser rejects malformed or unsupported metadata", "[mpgc]") {
    constexpr std::array invalid{"FGD:16",
                                 "FC1:2:TUdDEAACBwAAAAEAMgEAZwAAAAAAAAAKAEdhbGF4eSBNYXAB",
                                 "FC1:3:TUdDEAACBwAAAAEAMgEAZw==",
                                 "FC1:3:TUdDAAACBwAAAAEAMgEAZw==",
                                 "FC1:3:TUdDQQACBwAAAAEAMgEAZw==",
                                 "FC1:3:TUdDEAICBwAAAAEAMgEAZw==",
                                 "FC1:3:TUdDEAADBwAAAAEAMgEAZw==",
                                 "FC1:3:TUdDEAACBwAAAAEAMgEA",
                                 "FC1:3:TUdDEAACBwAAAAEAMgEAZwA=",
                                 "FC1:3:TUdDEAACBwAAAAEAMgEAZx==",
                                 "FC1:3:TUdDEAACBwAAAAEAMgEAZw=",
                                 "FC1:3:TUdD=AAABwAAAAEAMgEAZw==",
                                 "FC1:3:TUdD!AACBwAAAAEAMgEAZw=="};
    for (const auto *text : invalid) {
        INFO(text);
        CHECK_FALSE(gc::decode_metadata(text));
    }
}

TEST_CASE("GC battle metadata retains campaign options while admission is closed", "[mpgc]") {
    const auto info =
        gc::decode_metadata("FC1:3:TUdDAgABBwAAAAEAMgEAZw8AUmViZWxzIC8gRW1waXJlDABDcmVkaXRzOiAyMDAFAEVsaXRlAQkAdGF0MmdfY29uAAY=");
    REQUIRE(info);
    CHECK(info->count == 2);
    CHECK(info->maximum == 6);
    CHECK(info->factions == "Rebels / Empire");
    CHECK(info->victory_condition == "Credits: 200");
    CHECK(info->difficulty == "Elite");
    CHECK(info->team_damage);
    CHECK(info->map == "tat2g_con");
    CHECK_FALSE(info->joinable);

    std::array<std::byte, 0x70> browser{};
    std::array<std::byte, 0x180> row{};
    std::array<std::byte, 16> node{};
    fc::write_field<void *>(browser.data(), 0x10, node.data());
    fc::write_field<std::int32_t>(browser.data(), 0x1C, 1);
    fc::write_field<void *>(node.data(), 0xC, row.data());
    fc::write_field<std::uint8_t>(row.data(), 0x10, 1);
    fc::write_field<std::int32_t>(row.data(), 0xD0, 1);
    const auto lookup = [&info](std::uint64_t) {
        return info;
    };
    CHECK(gc::filter_browser_rows(browser.data(), {"tat2g_con", "Versus", "g"}, 5, true, lookup) == 0);
    CHECK(fc::read_field<std::int32_t>(browser.data(), 8) == 1);
    CHECK_FALSE(gc::decode_metadata("FC1:1:TUdDEAACBwAAAAEAMgEAZw=="));
}

TEST_CASE("GC result removal advances past unmatched rows and retains the current row", "[mpgc]") {
    std::array<std::byte, 16> first{}, second{}, terminal{}, locals{};
    auto *frame = locals.data() + locals.size();
    fc::write_field<const void *>(first.data(), 4, second.data());
    fc::write_field<const void *>(second.data(), 4, terminal.data());
    fc::write_field<const void *>(frame, -0xC, first.data());
    fc::write_field<const void *>(frame, -4, first.data());

    // A mismatch must reach the next node while native code still sees the compared row.
    gc::advance_result_iterator(frame);
    CHECK(fc::read_field<const void *>(frame, -0xC) == second.data());
    CHECK(fc::read_field<const void *>(frame, -4) == first.data());

    // A matching row may be freed after the callback; the next iterator must already be retained.
    fc::write_field<const void *>(frame, -4, second.data());
    gc::advance_result_iterator(frame);
    second.fill(std::byte{});
    CHECK(fc::read_field<const void *>(frame, -0xC) == terminal.data());
    CHECK(fc::read_field<const void *>(frame, -4) == second.data());
}

TEST_CASE("GC chat repeats only for stock keyboard character events", "[mpgc]") {
    std::array<std::byte, 0x287B> controller{};
    std::array<std::uint32_t, 144> keys{};
    std::array<std::uint8_t, 256> event_keys{};
    std::array<bool, 256> events{};
    keys[19] = 0x14;
    keys[13] = 0x0E;
    // Read the game's event-key map rather than treating an event code as a controller index.
    event_keys[0x14] = 87;
    event_keys[0x0E] = 91;
    fc::write_field<std::uint16_t>(controller.data(), 0x2768, 2);
    fc::write_field<wchar_t>(controller.data(), 0x276A, L'x');
    fc::write_field<std::uint8_t>(controller.data(), 0x2647 + 19, 1);
    fc::write_field<std::uint8_t>(controller.data(), 0x26D7 + 19, 1);

    gc::prepare_chat_repeat(controller.data(), keys, event_keys, events);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 19) == 0);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 13) == 0);

    events[87] = true;
    gc::prepare_chat_repeat(controller.data(), keys, event_keys, events);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 19) == 10);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 13) == 0);

    events.fill(false);
    gc::prepare_chat_repeat(controller.data(), keys, event_keys, events);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 19) == 0);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 13) == 0);

    events[91] = true;
    gc::prepare_chat_repeat(controller.data(), keys, event_keys, events);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 19) == 0);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x27EB + 13) == 10);
    CHECK(fc::read_field<std::uint16_t>(controller.data(), 0x2768) == 2);
    CHECK(fc::read_field<wchar_t>(controller.data(), 0x276A) == L'x');
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x2647 + 19) == 1);
    CHECK(fc::read_field<std::uint8_t>(controller.data(), 0x26D7 + 19) == 1);
}

TEST_CASE("GC battle preparation preserves its identity and exact native save pair", "[mpgc]") {
    gc::Message message{.kind = gc::MessageKind::BattleSetup};
    message.battle = {.id = 7, .state = {std::string("c\0x", 3), std::string("m\0y", 3)}};
    auto bytes = gc::encode_message(message);
    const auto decoded = gc::decode_message(bytes);
    REQUIRE(decoded);
    CHECK(decoded->battle.id == 7);
    CHECK(decoded->battle.state.campaign == message.battle.state.campaign);
    CHECK(decoded->battle.state.mission == message.battle.state.mission);
    bytes.pop_back();
    CHECK_FALSE(gc::decode_message(bytes));
    const auto acknowledgement = gc::decode_message(gc::encode_message({.kind = gc::MessageKind::BattleReady, .token = 7}));
    REQUIRE(acknowledgement);
    CHECK(acknowledgement->token == 7);
}

TEST_CASE("GC commands carry explicit choices and a decision without accepting arbitrary functions", "[mpgc]") {
    gc::Message message{.kind = gc::MessageKind::Action};
    message.action = {.decision = 12, .name = "move_fleet", .first = "yav", .second = "tat", .index = 3, .confirm = true};
    auto decoded = gc::decode_message(gc::encode_message(message));
    REQUIRE(decoded);
    CHECK(decoded->action.decision == 12);
    CHECK(decoded->action.first == "yav");
    CHECK(decoded->action.second == "tat");
    CHECK(decoded->action.index == 3);
    CHECK(decoded->action.confirm);
    CHECK(decoded->action.faction == 0);
    message.action.name = "ScriptCB_EnterMission";
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
    message = {.kind = gc::MessageKind::CurrentState};
    message.snapshot.host_faction = 1;
    message.snapshot.state = {"campaign", ""};
    message.decision = {.id = 12, .turn = 8, .faction = 2, .screen = "ifs_freeform_fleet", .pending_card = 3, .commanders = {-1, 63}};
    decoded = gc::decode_message(gc::encode_message(message));
    REQUIRE(decoded);
    CHECK(decoded->decision.pending_card == 3);
}

TEST_CASE("GC native presentation arrives with the campaign state that selected its destination", "[mpgc]") {
    gc::Message message{.kind = gc::MessageKind::CurrentState};
    message.snapshot.host_faction = 1;
    message.snapshot.state = {std::string("c\0x", 3), std::string("m\0y", 3)};
    message.decision = {.id = 13,
                        .turn = 8,
                        .faction = 2,
                        .screen = "ifs_freeform_ai",
                        .presentation_phase = "move",
                        .presentation_source = "yav",
                        .presentation_remaining_ms = 1200};
    auto bytes = gc::encode_message(message);
    const auto decoded = gc::decode_message(bytes);
    REQUIRE(decoded);
    CHECK(decoded->decision.presentation_phase == "move");
    CHECK(decoded->decision.presentation_source == "yav");
    CHECK(decoded->decision.presentation_remaining_ms == 1200);
    CHECK(decoded->snapshot.state.campaign == message.snapshot.state.campaign);
    CHECK(decoded->snapshot.state.mission == message.snapshot.state.mission);
    bytes.pop_back();
    CHECK_FALSE(gc::decode_message(bytes));

    message.decision.presentation_phase = "build";
    CHECK(gc::decode_message(gc::encode_message(message)));
    message.decision.presentation_phase.clear();
    CHECK(gc::decode_message(gc::encode_message(message)));
    message.decision.presentation_phase = "attack";
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
    message.decision.presentation_phase = "move";
    message.snapshot.state.campaign.clear();
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
    message.snapshot.state.campaign.assign(gc::kCampaignCapacity + 1, 'c');
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
    message.snapshot.state.campaign = "campaign";
    message.snapshot.state.mission.assign(gc::kMissionCapacity + 1, 'm');
    CHECK_FALSE(gc::decode_message(gc::encode_message(message)));
}

TEST_CASE("GC fleet phase and source change the decision; its timer does not", "[mpgc]") {
    const gc::Decision current{.turn = 8,
                               .faction = 2,
                               .screen = "ifs_freeform_ai",
                               .presentation_phase = "move",
                               .presentation_source = "yav",
                               .presentation_remaining_ms = 1200};
    auto next = current;
    next.presentation_remaining_ms = 100;
    CHECK(current.same_context(next));
    next.commanders = {3, 7};
    CHECK(current.same_context(next));
    next.presentation_source = "tat";
    CHECK_FALSE(current.same_context(next));
    next = current;
    next.presentation_phase = "build";
    CHECK_FALSE(current.same_context(next));
    next.presentation_phase.clear();
    CHECK_FALSE(current.same_context(next));
}

TEST_CASE("GC deadlines survive screen changes, commander replacement and an unfinished bonus round", "[mpgc]") {
    gc::DecisionTimers timers;
    const gc::DecisionTimers::Clock::time_point start{};
    timers.update(8, 2, true, true, 0, 1, start);
    CHECK(timers.turn_remaining(start + std::chrono::seconds{30}) == 30000);
    // A purchase screen does not begin a turn; undo may re-enter the fleet screen in the same turn.
    timers.update(8, 2, false, true, 0, 10, start + std::chrono::seconds{30});
    timers.update(8, 2, true, true, 0, 10, start + std::chrono::seconds{45});
    CHECK(timers.turn_remaining(start + std::chrono::seconds{45}) == 15000);
    CHECK(timers.turn_expired(start + std::chrono::seconds{60}));
    CHECK(timers.turn_expired(start + std::chrono::seconds{65}));
    timers.update(8, 2, false, false, 3, 10, start + std::chrono::seconds{65});
    CHECK_FALSE(timers.turn_expired(start + std::chrono::seconds{66}));
    CHECK(timers.bonus_remaining(start + std::chrono::seconds{66}) == 59000);
    timers.update(8, 2, false, false, 1, 10, start + std::chrono::seconds{100});
    CHECK(timers.bonus_remaining(start + std::chrono::seconds{100}) == 25000);
    CHECK_FALSE(timers.bonus_expired(start + std::chrono::seconds{124}));
    CHECK(timers.bonus_expired(start + std::chrono::seconds{125}));
    timers.update(8, 2, false, false, 0, 10, start + std::chrono::seconds{186});
    CHECK_FALSE(timers.bonus_expired(start + std::chrono::seconds{186}));
    timers.update(8, 2, false, false, 2, 10, start + std::chrono::seconds{187});
    CHECK(timers.bonus_remaining(start + std::chrono::seconds{187}) == 60000);
    timers.update(9, 1, true, true, 0, 0, start + std::chrono::seconds{188});
    CHECK_FALSE(timers.turn_enabled());
    CHECK_FALSE(timers.turn_expired(start + std::chrono::hours{24}));
}

TEST_CASE("GC simultaneous bonus choices authorize each commander's assigned faction", "[mpgc]") {
    gc::Decision decision{.turn = 8, .faction = 2, .screen = "ifs_freeform_battle_card", .bonus_pending_mask = 3};
    CHECK(gc::action_faction(decision, "choose_bonus", 1) == 1);
    CHECK(gc::action_faction(decision, "choose_bonus", 2) == 2);
    CHECK_FALSE(gc::action_faction(decision, "choose_bonus", 0));
    CHECK_FALSE(gc::action_faction(decision, "recruit", 1));
    CHECK(gc::action_faction(decision, "recruit", 2) == 2);
    const auto prior = decision;
    decision.bonus_pending_mask = 1;
    CHECK(prior.same_context(decision));
    CHECK(gc::action_faction(decision, "choose_bonus", 1) == 1);
    CHECK_FALSE(gc::action_faction(decision, "choose_bonus", 2));
    decision.bonus_remaining_ms = 60000;
    decision.timer_enabled = true;
    decision.timer_remaining_ms = 600000;
    const auto message = gc::decode_message(gc::encode_message(
        {.kind = gc::MessageKind::CurrentState, .snapshot = {.host_faction = 1, .state = {"campaign", ""}}, .decision = decision}));
    REQUIRE(message);
    CHECK(message->decision.bonus_pending_mask == 1);
    CHECK(message->decision.bonus_remaining_ms == 60000);
    CHECK(message->decision.timer_enabled);
    CHECK(message->decision.timer_remaining_ms == 600000);
}

TEST_CASE("GC Results Done preserves the attacker when the host belongs to the other faction", "[mpgc]") {
    const gc::Decision decision{.turn = 8, .faction = 2, .screen = "ifs_freeform_result"};
    CHECK(gc::action_faction(decision, "continue_result", 1, true) == 2);
}

TEST_CASE("GC Results Done belongs to the host even when another player commands its faction", "[mpgc]") {
    CHECK(gc::action_owner("continue_result", true, false));
    CHECK(gc::action_owner("continue_result", true, true));
    CHECK_FALSE(gc::action_owner("continue_result", false, true));
    CHECK_FALSE(gc::action_owner("continue_result", false, false));
    for (const auto action : {"continue_summary", "build_fleet", "move_fleet", "choose_bonus"}) {
        CAPTURE(action);
        CHECK_FALSE(gc::action_owner(action, true, false));
        CHECK(gc::action_owner(action, false, true));
    }
    const gc::Decision decision{.turn = 8, .faction = 2, .screen = "ifs_freeform_result"};
    CHECK_FALSE(gc::action_faction(decision, "build_fleet", 1, true));
    CHECK_FALSE(gc::action_faction(decision, "continue_summary", 1, true));
    CHECK(gc::action_faction(decision, "continue_result", 2, true) == 2);
    CHECK_FALSE(gc::action_faction(decision, "continue_result", 0, true));
}
} // namespace
