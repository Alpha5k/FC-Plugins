#pragma once

#include <cstdint>

namespace fc_plugins::voice_chat {

// Scopes microphone-test routing to one captured local node's native send, preserving nested callers.
class MicrophoneTestRoute final {
  public:
    // Saves the enclosing thread's route and scopes captured sends to this player while testing is enabled.
    MicrophoneTestRoute(std::uint8_t player, bool enabled) noexcept : previous_(player_), previous_capture_(captured_) {
        player_ = enabled ? player : kNoPlayer;
        captured_ = true;
    }

    // Restores the enclosing captured send, including when this scope temporarily disables test routing.
    ~MicrophoneTestRoute() {
        player_ = previous_;
        captured_ = previous_capture_;
    }

    MicrophoneTestRoute(const MicrophoneTestRoute &) = delete;
    MicrophoneTestRoute &operator=(const MicrophoneTestRoute &) = delete;

    // Distinguishes captured-node submissions from shared native relay calls for transition logging.
    static bool captured() noexcept {
        return captured_;
    }

    // Uses recipient-addressed packets for the scoped test and preserves the stored method for every other send.
    static std::uint32_t method(std::uint32_t native_method) noexcept {
        return player_ != kNoPlayer ? 2U : native_method;
    }

    // Keeps the test's self packet on the native local queue, including during an online session.
    static bool network_delivery(bool native_network, std::uint8_t destination) noexcept {
        return player_ == kNoPlayer || destination != player_ ? native_network : false;
    }

    // Restricts only the captured test source to itself; other sources retain their normal route checks.
    static bool permits_send(std::uint8_t source, std::uint8_t destination) noexcept {
        return player_ == kNoPlayer || source != player_ || destination == player_;
    }

  private:
    static constexpr std::uint8_t kNoPlayer = 0xFF;
    static inline thread_local std::uint8_t player_{kNoPlayer};
    static inline thread_local bool captured_{};
    std::uint8_t previous_;
    bool previous_capture_;
};

} // namespace fc_plugins::voice_chat
