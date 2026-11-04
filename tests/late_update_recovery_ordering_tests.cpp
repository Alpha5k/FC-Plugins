#include "../patches/late_update_recovery/ordering.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <span>

namespace fc_plugins::late_update_recovery::tests {
namespace {

struct Update {
    std::uint32_t turn;
};

} // namespace

TEST_CASE("Late update recovery orders turns across the 32-bit wrap", "[plugins][late_update_recovery]") {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    std::array updates{Update{1}, Update{maximum}, Update{0}, Update{maximum - 1}};

    order_by_turn(std::span{updates});

    CHECK(updates[0].turn == maximum - 1);
    CHECK(updates[1].turn == maximum);
    CHECK(updates[2].turn == 0);
    CHECK(updates[3].turn == 1);
}

TEST_CASE("Late update recovery retains every group while sorting a full batch", "[plugins][late_update_recovery]") {
    struct OwnedUpdate {
        std::uint32_t turn;
        std::uint32_t group;
    };

    std::array<OwnedUpdate, 512> updates{};
    for (std::size_t i = 0; i < updates.size(); ++i) {
        const auto turn = static_cast<std::uint32_t>(updates.size() - i);
        updates[i] = {turn, turn + 1000};
    }

    order_by_turn(std::span{updates});

    // Group references move with their native turn so every received group still reaches its parser.
    for (std::size_t i = 0; i < updates.size(); ++i) {
        CHECK(updates[i].turn == i + 1);
        CHECK(updates[i].group == updates[i].turn + 1000);
    }
}

} // namespace fc_plugins::late_update_recovery::tests
