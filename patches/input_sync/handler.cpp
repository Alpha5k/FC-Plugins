#include "handler.hpp"

#include "layout.hpp"

#include <cstdint>
#include <format>
#include <span>

namespace fc_plugins::input_sync {
namespace {

// FinishTurn must clear the received flag when its row reaches the first reuse boundary.
void plan_server(fc::Plan &plan, const Layout &layout) {
    plan.write(layout.retire_received.rva, std::uint8_t{0x72}, layout.retire_received.evidence);
}

} // namespace

void ServerHandler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    plan_server(plan, *layout);
}

void ClientHandler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    plan_server(plan, *layout);

    // Select the existing full-input path while preserving native packet cadence and serialization.
    plan.check(layout->full_input, 4);
    plan.insert_jump(plan.at(layout->input_selection.rva, layout->input_selection.name), 5, fc::address(plan.at(layout->full_input.rva)),
                     layout->input_selection.evidence);

    input_rows_memory_ = plan.access(layout->input_rows);
    current_turn_memory_ = plan.access(layout->current_turn);
    authority_turn_memory_ = plan.access(layout->authority_turn);
    prediction_turn_memory_ = plan.access(layout->prediction_turn);
    turns_per_second_memory_ = plan.access(layout->turns_per_second);
    players_memory_ = plan.access(layout->players);

    // This absolute MOV operand relocates with the executable; qualify it independently from its opcode.
    plan.check(layout->completed_input.rva + 1, 4, fc::points_to(plan.at(layout->current_turn.rva)));
    plan.observe_instruction(layout->completed_input, fc::before([this](const fc::CpuContext &) noexcept {
                                 capture();
                             }));
    plan.hook_instruction(layout->prediction_input, [this](fc::CpuContext &context) noexcept {
        predict(context);
    });
    plan.observe(layout->create, fc::after([this](const bool &) noexcept {
                     history_.reset();
                 }));
}

fc::Result<> ClientHandler::prepare(fc::PrepareContext &) {
    const auto rows = input_rows_memory_.view();
    if (!rows) {
        return std::unexpected(rows.error());
    }
    input_rows_ = rows->data();
    return {};
}

void ClientHandler::capture() {
    history_.capture(std::span<const Row, 32>{input_rows_, 32}, current_turn_memory_.read().value_or(0),
                     authority_turn_memory_.read().value_or(0), turns_per_second_memory_.read().value_or(0),
                     players_memory_.read().value_or(Players{-1, -1}));
}

void ClientHandler::predict(fc::CpuContext &context) {
    const auto turn = prediction_turn_memory_.read().value_or(0);
    const auto *row = history_.lookup(turn, authority_turn_memory_.read().value_or(0), turns_per_second_memory_.read().value_or(0),
                                      players_memory_.read().value_or(Players{-1, -1}));
    if (row) {
        // Only the native local-index branch reaches this hook, with a slot of zero or one in EBP-8.
        const auto slot = fc::read_field<std::uint32_t>(context.ebp, -8);
        context.eax = reinterpret_cast<std::uint32_t>(&(*row)[slot]);
    }
}

} // namespace fc_plugins::input_sync
