#pragma once

#include "api.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fc_plugins::melee_timing {

// Keeps one precise elapsed age per physical row of the existing eight-entry native queue.
class QueueTiming final {
  public:
    // Imports the current native clock once when precise timing begins for this queue.
    explicit QueueTiming(const void *weapon) noexcept {
        const double clock = fc::read_field<float>(weapon, 0x130);
        for (std::size_t index{}; index < ages_.size(); ++index) {
            ages_[index] = clock - fc::read_field<float>(weapon, 0x134 + index * 8);
        }
    }

    // Projects occupied event times once to float around a zero native clock.
    void project(void *weapon) const noexcept {
        const auto count = fc::read_field<std::uint8_t>(weapon, 0x174);
        const auto head = fc::read_field<std::uint8_t>(weapon, 0x175);
        for (std::size_t index{}; index < count; ++index) {
            const auto row = (head + index) & 7U;
            fc::write_field(weapon, 0x134 + row * 8, static_cast<float>(-ages_[row]));
        }
        fc::write_field(weapon, 0x130, 0.0F);
    }

    // Native producers stamp new rows at the zero clock; each replaces that row's preceding age.
    void produced(const void *weapon) noexcept {
        for (std::size_t index{}; index < ages_.size(); ++index) {
            if (fc::read_field<float>(weapon, 0x134 + index * 8) == 0.0F) {
                ages_[index] = 0.0;
            }
        }
    }

    // Update's resulting queue clock includes the actual accumulated elapsed time it consumed.
    void advance(void *weapon) noexcept {
        const double elapsed = fc::read_field<float>(weapon, 0x130);
        for (auto &age : ages_) {
            age += elapsed;
        }
        project(weapon);
    }

    // Copies occupied ages in native logical order, independent of the current circular head.
    QueueAges capture(const void *weapon) const noexcept {
        QueueAges result;
        const auto count = fc::read_field<std::uint8_t>(weapon, 0x174);
        const auto head = fc::read_field<std::uint8_t>(weapon, 0x175);
        for (std::size_t index{}; index < count; ++index) {
            result.values[index] = ages_[(head + index) & 7U];
        }
        return result;
    }

    // Restores logical ages after packet or checkpoint ownership has restored the native queue rows.
    void restore(void *weapon, const QueueAges &ages) noexcept {
        const auto count = fc::read_field<std::uint8_t>(weapon, 0x174);
        const auto head = fc::read_field<std::uint8_t>(weapon, 0x175);
        for (std::size_t index{}; index < count; ++index) {
            ages_[(head + index) & 7U] = ages.values[index];
        }
        project(weapon);
    }

  private:
    std::array<double, 8> ages_{};
};

// Keeps the queued timestamp chosen by native winner comparisons during one transition scan.
struct WinningInput {
    std::optional<float> query;
    std::optional<float> selected;

    // Promotes only the current successful query; an unqueued winner clears timestamp ownership.
    void select() noexcept {
        selected = query;
    }
};

} // namespace fc_plugins::melee_timing
