#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace fc_plugins::multiplayer_gc {

// Keeps host deadlines independent of Lua screens and commander replacement.
class DecisionTimers {
  public:
    using Clock = std::chrono::steady_clock;

    // Starts a faction's turn once, and a shared bonus round when its pending mask first becomes nonzero.
    void update(std::uint32_t turn, std::uint8_t faction, bool begin_turn, bool strategic, std::uint8_t bonus_mask, std::uint8_t minutes,
                Clock::time_point now) {
        const auto key = std::pair{turn, faction};
        if (begin_turn && key != turn_) {
            turn_ = key;
            turn_deadline_ = minutes ? std::optional{now + std::chrono::minutes{minutes}} : std::nullopt;
        }
        strategic_ = strategic;
        if (bonus_mask && !bonus_mask_) {
            bonus_deadline_ = now + std::chrono::seconds{60};
        } else if (!bonus_mask) {
            bonus_deadline_.reset();
        }
        bonus_mask_ = bonus_mask;
    }

    // Reports milliseconds left; expiry stays observable until campaign progression ends the decision.
    std::uint32_t turn_remaining(Clock::time_point now) const {
        return strategic_ ? remaining(turn_deadline_, now) : 0;
    }

    std::uint32_t bonus_remaining(Clock::time_point now) const {
        return remaining(bonus_deadline_, now);
    }

    bool turn_enabled() const {
        return strategic_ && turn_deadline_.has_value();
    }

    bool turn_expired(Clock::time_point now) const {
        return turn_enabled() && now >= *turn_deadline_;
    }

    bool bonus_expired(Clock::time_point now) const {
        return bonus_deadline_ && now >= *bonus_deadline_;
    }

  private:
    // Rounds up so a still-running deadline never displays zero milliseconds.
    static std::uint32_t remaining(const std::optional<Clock::time_point> &deadline, Clock::time_point now) {
        return deadline && now < *deadline
                   ? static_cast<std::uint32_t>(std::chrono::ceil<std::chrono::milliseconds>(*deadline - now).count())
                   : 0;
    }

    std::optional<std::pair<std::uint32_t, std::uint8_t>> turn_;
    std::optional<Clock::time_point> turn_deadline_, bonus_deadline_;
    std::uint8_t bonus_mask_{};
    bool strategic_{};
};

} // namespace fc_plugins::multiplayer_gc
