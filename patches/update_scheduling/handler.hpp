#pragma once

#include "layout.hpp"
#include "send_window.hpp"
#include "slot_peers.hpp"

#include "../network_diagnostics/api.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::update_scheduling {

// Native scheduling hooks own CREATE fences and response windows; control callbacks publish peer identities.
class Handler final {
  public:
    // Plans dedicated rendering removal, host send eligibility, exact ACK timing and CREATE ordering.
    // Native world creation resets FC windows and fences even when it preserves the connection.
    void plan(fc::Plan &plan);

    // Resolves native fields and registers compatibility with LateUpdateRecovery.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // A pending map pointer and turn together identify the CREATE awaiting a native response.
    struct CreateFence {
        void *pending_state{}; // Borrowed nextObjState map pointer.
        std::int32_t turn{};   // Map turn, paired with the pointer to detect a native response.
        float sent_time{};     // netTime seconds when CREATE was emitted.
    };

    using DiagnosticsApi = network_diagnostics::NetworkDiagnosticsApi;

    // Publishes current native slot information; ended peers remove only their own identity.
    void peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept;

    // Returns the optional diagnostics table after its copied contents become visible.
    const DiagnosticsApi *diagnostics() const noexcept;

    // Records the exact map and turn whose emitted CREATE must precede later updates.
    void record_create(std::int32_t destination) noexcept;
    void report_fence(std::int32_t destination, std::uint32_t phase, const CreateFence &before) noexcept;

    // Waits for the native response or regenerates the pending map after its timeout.
    bool fence_blocks(std::int32_t destination, bool &timed_out) noexcept;

    // Skips native ACK writes beyond the two allocated slots and records compatible sends.
    void guard_acknowledgement(fc::CpuContext &context) noexcept;

    // Supplies an exact compatible send's elapsed time to the native estimator when its two slots have no match.
    void sample_untracked_acknowledgement(fc::CpuContext &context) noexcept;

    // Clears map-relative windows and CREATE fences on the native owner thread.
    void reset_world() noexcept;

    // Captures CREATE only when WriteObjects' frame local says CREATE data was emitted.
    void capture_create(const fc::CpuContext &context) noexcept;

    // Chooses the native or compatible response window after preserving CREATE ordering.
    void gate_destination(fc::CpuContext &context) noexcept;

    // Releases compatible-window sends through the client's latest accepted update turn; zero means no ACK.
    void acknowledge_update(std::int32_t destination, std::uint32_t turn) noexcept;

    // Clears fences on FC session changes and windows on peer changes, then returns the current peer.
    // Native Create independently clears map-relative state, including when the connection survives.
    fc::PeerId synchronize_window(std::size_t destination) noexcept;

    std::array<CreateFence, kMaximumPlayers> fences_{};
    std::array<PeerSendWindow, kMaximumPlayers> send_windows_{};

    SlotPeers<kMaximumPlayers> slot_peers_;

    std::array<fc::Memory<void *>, kMaximumPlayers> pending_memory_{};
    std::array<fc::Memory<const float>, kMaximumPlayers> estimate_memory_{};

    std::array<void **, kMaximumPlayers> pending_state_slots_{};
    std::array<const float *, kMaximumPlayers> round_trip_estimates_{}; // Native mean and deviation in seconds.

    fc::Memory<const std::int32_t> destination_memory_;
    fc::Memory<const std::uint32_t> turn_memory_;
    fc::Memory<const std::int32_t> rate_memory_;
    fc::Memory<const float> time_memory_;

    std::array<fc::Memory<const std::byte>, 4> continuation_memory_{};

    const std::int32_t *current_destination_{};
    const std::uint32_t *host_turn_{};
    const std::int32_t *turn_rate_{};
    const float *network_time_{}; // netTime seconds; native world creation resets this clock.

    fc::NativeFunction<AllocateObjectState> allocate_object_state_;
    fc::NativeFunction<FreeObjectState> free_object_state_;

    const std::byte *acknowledgement_resume_{};
    const std::byte *continue_destination_checks_{};
    const std::byte *next_destination_{};
    const std::byte *round_trip_update_{};

    fc::Logger log_;
    fc::Game game_;
    fc::SessionId session_{};
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;

    bool diagnostics_full_{};
    DiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};

    // Native scheduling claims one timeout warning for this handler lifetime.
    bool timeout_warning_claimed_{};
};

} // namespace fc_plugins::update_scheduling
