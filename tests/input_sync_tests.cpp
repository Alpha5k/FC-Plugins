#include "../patches/input_sync/history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace input = fc_plugins::input_sync;

namespace {

constexpr input::Players kPlayers{7, 11};

// Gives both native slots distinct observable contents for an independently named turn.
input::Row make_row(std::uint32_t turn) {
    input::Row row{};
    for (std::size_t slot = 0; slot < row.size(); ++slot) {
        row[slot].bytes.fill(static_cast<std::byte>((turn + slot * 61) % 256));
    }
    return row;
}

// Models the native 32-row producer, which overwrites a row before the capture instruction runs.
void produce(input::History &history, std::array<input::Row, 32> &native_rows, std::uint32_t turn, std::uint32_t authority,
             std::uint32_t rate, input::Players players = kPlayers) {
    native_rows[turn % 32] = make_row(turn);
    history.capture(native_rows, turn, authority, rate, players);
}

} // namespace

TEST_CASE("Input history retains both slots through native wrap and retires authority input", "[plugins][input_history]") {
    for (const std::uint32_t rate : {20U, 60U}) {
        CAPTURE(rate);
        input::History history;
        std::array<input::Row, 32> native_rows{};
        const auto last = 100 + 3 * rate - 1;
        for (auto turn = 100U; turn <= last; ++turn) {
            produce(history, native_rows, turn, 100, rate);
        }

        for (auto turn = 100U; turn <= last; ++turn) {
            const auto *row = history.lookup(turn, 100, rate, kPlayers);
            REQUIRE(row);
            CHECK(*row == make_row(turn));
        }
        CHECK(native_rows[100 % 32] != make_row(100));
        CHECK_FALSE(history.lookup(99, 100, rate, kPlayers));
        CHECK_FALSE(history.lookup(last + 1, 100, rate, kPlayers));

        for (auto turn = 100U; turn < 112; ++turn) {
            CHECK_FALSE(history.lookup(turn, 112, rate, kPlayers));
        }
        REQUIRE(history.lookup(112, 112, rate, kPlayers));
        CHECK(*history.lookup(112, 112, rate, kPlayers) == make_row(112));
        CHECK(*history.lookup(last, 112, rate, kPlayers) == make_row(last));

        CHECK_FALSE(history.lookup(last + 1, 112, rate, kPlayers));

        // Reusing a retained ring index must not make its prior tag appear to hold the new turn.
        produce(history, native_rows, last + 1, 112, rate);
        REQUIRE(history.lookup(last + 1, 112, rate, kPlayers));
        CHECK(*history.lookup(last + 1, 112, rate, kPlayers) == make_row(last + 1));
        CHECK_FALSE(history.lookup(last + 2, 112, rate, kPlayers));
    }
}

TEST_CASE("Input history keeps the latest rewrite at the native lead cap", "[plugins][input_history]") {
    input::History history;
    std::array<input::Row, 32> native_rows{};
    for (auto turn = 100U; turn < 160; ++turn) {
        produce(history, native_rows, turn, 100, 20);
    }

    native_rows[159 % 32] = make_row(201);
    history.capture(native_rows, 159, 100, 20, kPlayers);
    REQUIRE(history.lookup(159, 100, 20, kPlayers));
    CHECK(*history.lookup(159, 100, 20, kPlayers) == make_row(201));
    CHECK(*history.lookup(158, 100, 20, kPlayers) == make_row(158));
    CHECK_FALSE(history.lookup(160, 100, 20, kPlayers));
}

TEST_CASE("Input history seeds only earlier rows still resident after the current write", "[plugins][input_history]") {
    for (const std::uint32_t first_sample : {104U, 132U}) {
        CAPTURE(first_sample);
        input::History history;
        std::array<input::Row, 32> native_rows{};
        for (auto turn = 100U; turn < first_sample; ++turn) {
            native_rows[turn % 32] = make_row(turn);
        }
        produce(history, native_rows, first_sample, 100, 20);

        const auto first_retained = first_sample == 104 ? 100U : 101U;
        for (auto turn = first_retained; turn <= first_sample; ++turn) {
            REQUIRE(history.lookup(turn, 100, 20, kPlayers));
            CHECK(*history.lookup(turn, 100, 20, kPlayers) == make_row(turn));
        }
        if (first_sample == 132) {
            CHECK_FALSE(history.lookup(100, 100, 20, kPlayers));
        }
    }
}

TEST_CASE("Input history clears replaced players and changed rates without copying their old native rows", "[plugins][input_history]") {
    input::History history;
    std::array<input::Row, 32> native_rows{};
    produce(history, native_rows, 100, 100, 20);
    produce(history, native_rows, 101, 100, 20);

    const input::Players replacement{17, 11};
    CHECK_FALSE(history.lookup(100, 100, 20, replacement));
    produce(history, native_rows, 102, 100, 20, replacement);
    CHECK_FALSE(history.lookup(101, 100, 20, replacement));
    REQUIRE(history.lookup(102, 100, 20, replacement));
    CHECK(*history.lookup(102, 100, 20, replacement) == make_row(102));

    CHECK_FALSE(history.lookup(102, 100, 60, replacement));
    produce(history, native_rows, 103, 100, 60, replacement);
    CHECK_FALSE(history.lookup(102, 100, 60, replacement));
    REQUIRE(history.lookup(103, 100, 60, replacement));
    CHECK(*history.lookup(103, 100, 60, replacement) == make_row(103));
}

TEST_CASE("Native reset clears old input and retains the new session input", "[plugins][input_history]") {
    input::History history;
    std::array<input::Row, 32> native_rows{};
    produce(history, native_rows, 100, 100, 20);
    history.reset();
    CHECK_FALSE(history.lookup(100, 100, 20, kPlayers));

    for (auto turn = 200U; turn < 204; ++turn) {
        native_rows[turn % 32] = make_row(turn);
    }
    produce(history, native_rows, 204, 200, 20);
    REQUIRE(history.lookup(200, 200, 20, kPlayers));
    CHECK(*history.lookup(200, 200, 20, kPlayers) == make_row(200));
    history.reset();
    CHECK_FALSE(history.lookup(204, 200, 20, kPlayers));
}

TEST_CASE("Input history waits for the native startup rate and authority checkpoint", "[plugins][input_history]") {
    input::History history;
    std::array<input::Row, 32> native_rows{};
    produce(history, native_rows, 100, 100, 0);
    CHECK_FALSE(history.lookup(100, 100, 0, kPlayers));
    produce(history, native_rows, 100, 0, 20);
    CHECK_FALSE(history.lookup(100, 0, 20, kPlayers));
    produce(history, native_rows, 100, 100, 20);
    REQUIRE(history.lookup(100, 100, 20, kPlayers));
    CHECK(*history.lookup(100, 100, 20, kPlayers) == make_row(100));
}
