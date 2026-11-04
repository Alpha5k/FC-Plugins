#include "handler.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <span>
#include <format>

namespace fc_plugins::late_update_recovery {
void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Check the instructions that capture ReceiveClient's local values and update timers.
    for (const auto *site : {&layout->pipe_state_capture, &layout->timestamp_capture, &layout->timer_refresh}) {
        plan.check(*site, site->evidence.bytes.size());
    }

    construct_group_ = plan.function(layout->construct_group);
    receive_update_ = plan.function(layout->receive_update);
    get_last_receive_ = plan.function(layout->get_last_receive);
    receive_group_ = plan.function(layout->receive_group);
    get_update_turn_ = plan.function(layout->get_update_turn);

    deadline_memory_ = plan.access(layout->host_timeout_deadline);
    extension_memory_ = plan.access(layout->host_timeout_extension);
    resume_memory_ = plan.access<const std::byte>(layout->receive_resume.rva, layout->receive_resume.evidence.bytes.size(),
                                                  layout->receive_resume.evidence);

    plan.hook_instruction(layout->stock_drain, [this](fc::CpuContext &context) noexcept {
        recover(context);
    });
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    const auto deadline = deadline_memory_.view();
    if (!deadline) {
        return std::unexpected(deadline.error());
    }

    const auto extension = extension_memory_.view();
    if (!extension) {
        return std::unexpected(extension.error());
    }

    const auto resume = resume_memory_.view();
    if (!resume) {
        return std::unexpected(resume.error());
    }

    deadline_ = deadline->data();
    extension_ = extension->data();
    resume_ = resume->data();

    // Advertise processing queued updates in turn order so UpdateScheduling can allow more outstanding updates.
    const auto feature = context.multiplayer().feature({"FC.LateUpdateRecovery", fc::FeatureRole::Client});
    if (!feature) {
        return std::unexpected(feature.error());
    }

    // Publish readiness only after copying the diagnostics table for native receive callbacks.
    return context.on_available("Diagnostics.NetworkDiagnostics", network_diagnostics::api_contract(),
                                [this](const network_diagnostics::NetworkDiagnosticsApi &api) noexcept {
                                    diagnostics_full_ = api.capture_mode() == 2;
                                    diagnostics_ = api;
                                    diagnostics_ready_.store(true, std::memory_order_release);
                                });
}

void Handler::recover(fc::CpuContext &context) noexcept {
    // ReceiveClient has already saved the host connection's pipe index and network time in these frame locals.
    const auto frame = context.ebp;
    const auto pipe = fc::read_field<std::int32_t>(frame, -0x14);
    const auto timestamp = fc::read_field<float>(frame, -0x10);

    if (drain()) {
        // Match stock activity accounting: +0x40 selects the host route; hostCycleTime gets the native allowance.
        if (const auto last_receive = get_last_receive_.call(pipe + 0x40); last_receive && *last_receive) {
            **last_receive = timestamp;
        }
        *deadline_ = timestamp + *extension_;
    }

    context.eip = reinterpret_cast<std::uint32_t>(resume_);
}

bool Handler::drain() noexcept {
    const auto observed = diagnostics_ready_.load(std::memory_order_acquire);
    const auto measured = observed && diagnostics_full_;
    network_diagnostics::RecoveryDrainObservation observation{};
    const auto sample = []() noexcept {
        LARGE_INTEGER value{};
        const auto winsock_error = WSAGetLastError();
        const auto windows_error = GetLastError();
        const auto valid = QueryPerformanceCounter(&value);
        WSASetLastError(winsock_error);
        SetLastError(windows_error);
        return valid ? static_cast<std::uint64_t>(value.QuadPart) : std::uint64_t{};
    };
    observation.entered_qpc = measured ? sample() : 0;
    const auto report = [this, observed, measured, &observation, &sample]() noexcept {
        if (observed) {
            observation.returned_qpc = measured ? sample() : 0;
            const auto winsock_error = WSAGetLastError();
            const auto windows_error = GetLastError();
            diagnostics_.recovery_drain(&observation);
            WSASetLastError(winsock_error);
            SetLastError(windows_error);
        }
    };
    // NetComm::Recv supplies complete game-update groups; native storage retains incomplete fragments.
    std::size_t count{};
    for (; count < updates_.size(); ++count) {
        auto &update = updates_[count];
        static_cast<void>(construct_group_.call(update.group.data()));
        if (!receive_group_.call(update.group.data(), std::int32_t{0x0C}).value_or(false)) {
            break;
        }

        update.turn = get_update_turn_.call(update.group.data()).value_or(0);
    }

    if (count == 0) {
        report();
        return false;
    }

    // Order only this batch; the native turn guard can reject an older group received in a later pass.
    auto batch = std::span{updates_}.first(count);
    order_by_turn(batch);

    const auto oldest_turn = batch.front().turn;
    const auto newest_turn = batch.back().turn;

    // ReadUpdate accepts only turns newer than the last accepted turn, then applies state and events.
    // ReceiveUpdate frees each group's packets even when that turn is rejected.
    for (auto &update : batch) {
        static_cast<void>(receive_update_.call(update.group.data()));
    }

    observation.groups = static_cast<std::uint32_t>(count);
    observation.oldest_turn = oldest_turn;
    observation.newest_turn = newest_turn;
    report();
    return true;
}

} // namespace fc_plugins::late_update_recovery
