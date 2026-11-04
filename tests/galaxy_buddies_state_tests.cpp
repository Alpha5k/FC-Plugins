#include "../patches/galaxy_buddies/state.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

namespace buddy_state = fc_plugins::galaxy_buddies::state;

TEST_CASE("Galaxy buddy callback state preserves bounded operation results", "[social][buddies]") {
    buddy_state::Mailbox<int, 2> mailbox;
    mailbox.publish(10);
    mailbox.publish(20);
    mailbox.publish(30);

    const auto messages = mailbox.consume();
    REQUIRE(messages.count == 2);
    CHECK(messages.records[0] == 10);
    CHECK(messages.records[1] == 20);
    CHECK(messages.dropped == 1);
    CHECK(mailbox.consume().count == 0);

    using buddy_state::PendingRequestState;
    buddy_state::PendingRequests<2> requests;
    CHECK(requests.reserve(100, PendingRequestState::Sending) == buddy_state::ReserveResult::Reserved);
    CHECK(requests.reserve(100, PendingRequestState::Canceling) == buddy_state::ReserveResult::Duplicate);
    CHECK(requests.reserve(200, PendingRequestState::Canceling) == buddy_state::ReserveResult::Reserved);
    CHECK(requests.reserve(300, PendingRequestState::Sending) == buddy_state::ReserveResult::Full);

    REQUIRE(requests.complete(100, PendingRequestState::Sending, PendingRequestState::SendSucceeded));
    REQUIRE(requests.complete(200, PendingRequestState::Canceling, PendingRequestState::CancelFailed, 7));
    const auto completions = requests.consume();
    REQUIRE(completions.count == 2);
    CHECK(completions.completions[0].user == 100);
    CHECK(completions.completions[0].state == PendingRequestState::SendSucceeded);
    CHECK(completions.completions[1].user == 200);
    CHECK(completions.completions[1].state == PendingRequestState::CancelFailed);
    CHECK(completions.completions[1].detail == 7);

    CHECK(requests.reserve(300, PendingRequestState::Sending) == buddy_state::ReserveResult::Reserved);
    requests.release(300, PendingRequestState::Sending);
    CHECK(requests.reserve(300, PendingRequestState::Canceling) == buddy_state::ReserveResult::Reserved);
}

TEST_CASE("Buddy cancellation keeps its reservation until the matching completion is consumed", "[social][buddies]") {
    using buddy_state::PendingRequestState;
    buddy_state::PendingRequests<1> requests;
    REQUIRE(requests.reserve(100, PendingRequestState::Canceling) == buddy_state::ReserveResult::Reserved);
    CHECK_FALSE(requests.complete(200, PendingRequestState::Canceling, PendingRequestState::CancelSucceeded));
    CHECK_FALSE(requests.complete(100, PendingRequestState::Sending, PendingRequestState::SendSucceeded));
    CHECK(requests.consume().count == 0);
    CHECK(requests.reserve(200, PendingRequestState::Sending) == buddy_state::ReserveResult::Full);
    REQUIRE(requests.complete(100, PendingRequestState::Canceling, PendingRequestState::CancelSucceeded));
    CHECK_FALSE(requests.complete(100, PendingRequestState::Canceling, PendingRequestState::CancelFailed, 9));
    CHECK(requests.reserve(100, PendingRequestState::Sending) == buddy_state::ReserveResult::Duplicate);
    const auto completion = requests.consume();
    REQUIRE(completion.count == 1);
    CHECK(completion.completions[0].user == 100);
    CHECK(completion.completions[0].state == PendingRequestState::CancelSucceeded);
    CHECK(requests.reserve(200, PendingRequestState::Sending) == buddy_state::ReserveResult::Reserved);
}

} // namespace
