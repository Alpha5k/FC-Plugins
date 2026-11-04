#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fc_plugins::input_sync {

// A processed native input contains controls for one local slot; each turn stores both slots.
struct Input {
    std::array<std::byte, 24> bytes{};
    bool operator==(const Input &) const = default;
};

using Row = std::array<Input, 2>;
using Players = std::array<std::int32_t, 2>;
static_assert(sizeof(Row) == 48);

// Retains exact input turns within the native three-second prediction allowance on the game thread.
class History {
  public:
    // Copies both completed native slots, including repeated writes at the prediction limit.
    // The first sample also copies up to 31 earlier rows still present in the native ring.
    void capture(std::span<const Row, 32> native_rows, std::uint32_t turn, std::uint32_t authority, std::uint32_t turns_per_second,
                 Players players);
    // Returns an exact eligible row, or nullptr for missing, retired or future input.
    const Row *lookup(std::uint32_t turn, std::uint32_t authority, std::uint32_t turns_per_second, Players players);
    // Clears retained input after either native Create form; the new session can copy its earlier native rows.
    void reset() noexcept;

  private:
    struct Entry {
        std::uint32_t turn{};
        bool valid{};
        Row row{};
    };

    // Resizes only on a rate change and discards rows when their local ownership changes.
    void synchronize(std::uint32_t turns_per_second, Players players);
    // Invalidates tags without changing storage or the current native rate and players.
    void invalidate() noexcept;
    // Tests the native authority interval; authority zero denotes startup before an accepted checkpoint.
    bool eligible(std::uint32_t turn, std::uint32_t authority) const noexcept;
    // Copies one exact native turn into its tagged ring position.
    void store(std::uint32_t turn, const Row &row) noexcept;

    std::vector<Entry> rows_;
    std::uint32_t turns_per_second_{};
    Players players_{-1, -1};
    bool seed_pending_{true};
    // Once sampling starts, changed identities or rates cannot seed old native rows into the new history.
    bool sampled_{};
};

} // namespace fc_plugins::input_sync
