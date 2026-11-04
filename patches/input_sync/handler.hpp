#pragma once

#include "history.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

namespace fc_plugins::input_sync {

// Sends complete controls, corrects server retirement and supplies retained input to client prediction.
class ClientHandler {
  public:
    // Plans complete input delivery, server retirement, retained prediction input and native reset observation.
    void plan(fc::Plan &plan);
    // Binds the native input array before serialized game-thread callbacks run.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // Copies both completed local slots before the producer increments its native turn.
    void capture();
    // Replaces only the chosen local input pointer when its exact eligible turn is retained.
    void predict(fc::CpuContext &context);

    fc::Memory<const Row> input_rows_memory_;
    const Row *input_rows_{};
    fc::Memory<const std::uint32_t> current_turn_memory_;
    fc::Memory<const std::uint32_t> authority_turn_memory_;
    fc::Memory<const std::uint32_t> prediction_turn_memory_;
    fc::Memory<const std::uint32_t> turns_per_second_memory_;
    fc::Memory<const Players> players_memory_;
    History history_;
};

// Dedicated servers need only the received-flag retirement correction.
struct ServerHandler {
    // Changes the server boundary comparison without reserving local history or client hooks.
    void plan(fc::Plan &plan);
};

} // namespace fc_plugins::input_sync
