#pragma once

#include "layout.hpp"
#include "ordering.hpp"

#include "../network_diagnostics/api.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::late_update_recovery {

// Processes queued client updates in turn order before native prediction runs.
class Handler final {
  public:
    // Reserves the native receive calls, host activity timers and replacement instruction hook.
    void plan(fc::Plan &plan);

    // Resolves native memory views and advertises ordered recovery to compatible hosts.
    fc::Result<> prepare(fc::PrepareContext &context);

  private:
    // NetPktGroup stores packet-pool indices, so sorting these opaque values preserves native ownership.
    struct alignas(4) BufferedUpdate {
        std::array<std::byte, 0x1C> group{};
        std::uint32_t turn{};
    };

    static_assert(sizeof(BufferedUpdate) == 0x20);

    // Reads ReceiveClient's pipe/time locals, recovers groups and resumes after the stock drain.
    void recover(fc::CpuContext &context) noexcept;

    // Passes up to 512 complete groups to ReceiveUpdate, which decodes accepted turns and frees every group.
    // Returns whether any complete group arrived; later batches retain the native turn guard.
    bool drain() noexcept;

    fc::NativeFunction<ConstructGroup> construct_group_;
    fc::NativeFunction<ReceiveUpdate> receive_update_;
    fc::NativeFunction<GetLastReceive> get_last_receive_;
    fc::NativeFunction<ReceiveGroup> receive_group_;
    fc::NativeFunction<GetUpdateTurn> get_update_turn_;

    fc::Memory<float> deadline_memory_;
    fc::Memory<const float> extension_memory_;
    fc::Memory<const std::byte> resume_memory_;

    float *deadline_{};         // hostCycleTime, in network-clock seconds.
    const float *extension_{};  // Native allowance for the host to cycle, in seconds.
    const std::byte *resume_{}; // Native stats/timeout processing after the replaced drain.

    // ReceiveClient serializes access to this reused batch; native decoding releases each received group before reuse.
    std::array<BufferedUpdate, kMaximumBatch> updates_{};

    bool diagnostics_full_{};
    network_diagnostics::NetworkDiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};
};

} // namespace fc_plugins::late_update_recovery
