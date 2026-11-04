#include "handler.hpp"

#include <cstddef>
#include <cstdint>
#include <format>

#include <WinSock2.h>
#include <Windows.h>

namespace fc_plugins::update_scheduling {
namespace {

// NetObjStateMap stores its turn after 64 state pointers and its next map pointer.
constexpr std::size_t kObjectStateTurnOffset = 0x104;
// EBP-relative locals in WriteObjects and SendToClients.
constexpr std::ptrdiff_t kEmittedCreateOffset = -0x11;
constexpr std::ptrdiff_t kDestinationOffset = -0x10;
// ReceivedUpdateAck stores elapsed seconds at EBP-1C before entering its RTT estimator.
constexpr std::ptrdiff_t kAcknowledgedRoundTripOffset = -0x1C;
constexpr std::uint32_t kZeroFlag = 1U << 6;

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    destination_memory_ = plan.access(layout->current_destination);
    turn_memory_ = plan.access(layout->host_turn);
    rate_memory_ = plan.access(layout->turn_rate);
    time_memory_ = plan.access(layout->network_time);

    allocate_object_state_ = plan.function(layout->allocate_object_state);
    free_object_state_ = plan.function(layout->free_object_state);

    const std::array sites{layout->acknowledgement_resume, layout->continue_destination_checks, layout->next_destination,
                           layout->round_trip_update};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        continuation_memory_[i] = plan.access<const std::byte>(sites[i].rva, sites[i].evidence.bytes.size(), sites[i].evidence);
    }

    plan.check(layout->send_window_result.rva, 5, layout->send_window_result.evidence);

    for (std::size_t i = 0; i < kMaximumPlayers; ++i) {
        pending_memory_[i] = plan.access(layout->pending_states[i]);
        estimate_memory_[i] = plan.access(layout->round_trip_estimates[i]);
    }

    // Omit dedicated rendering and raise the send allowance from half the player limit to the full limit.
    plan.nop(layout->dedicated_render, 5);
    plan.nop(layout->client_visit_count, 5);

    plan.hook_instruction(layout->acknowledgement_guard, [this](fc::CpuContext &c) noexcept {
        guard_acknowledgement(c);
    });

    plan.hook_instruction(layout->untracked_acknowledgement, [this](fc::CpuContext &c) noexcept {
        sample_untracked_acknowledgement(c);
    });

    plan.observe(layout->network_create, fc::after([this](const bool &) noexcept {
                     // Native Create resets the clock even when it preserves the connection.
                     reset_world();
                 }));

    plan.observe_instruction(layout->create_marker, fc::before([this](const fc::CpuContext &c) noexcept {
                                 // Both CREATE branches reach this instruction; capture_create checks whether CREATE data was emitted.
                                 capture_create(c);
                             }));

    plan.hook_instruction(layout->send_window, [this](fc::CpuContext &c) noexcept {
        gate_destination(c);
    });

    plan.observe(layout->received_update_ack, fc::after([this](std::int32_t destination, std::uint32_t turn) noexcept {
                     acknowledge_update(destination, turn);
                 }));

    // Every destination becomes eligible on the next host turn; native ticket, pipe and window checks still apply.
    plan.write(layout->next_update_turn, fc::byte_array<0x83, 0xC1, 0x01>());
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    const auto destination = destination_memory_.view();
    if (!destination) {
        return std::unexpected(destination.error());
    }

    const auto turn = turn_memory_.view();
    if (!turn) {
        return std::unexpected(turn.error());
    }

    const auto rate = rate_memory_.view();
    if (!rate) {
        return std::unexpected(rate.error());
    }

    const auto time = time_memory_.view();
    if (!time) {
        return std::unexpected(time.error());
    }

    current_destination_ = destination->data();
    host_turn_ = turn->data();
    turn_rate_ = rate->data();
    network_time_ = time->data();

    std::array<const std::byte *, 4> continuations{};
    for (std::size_t i = 0; i < continuation_memory_.size(); ++i) {
        const auto view = continuation_memory_[i].view();
        if (!view) {
            return std::unexpected(view.error());
        }

        continuations[i] = view->data();
    }

    acknowledgement_resume_ = continuations[0];
    continue_destination_checks_ = continuations[1];
    next_destination_ = continuations[2];
    round_trip_update_ = continuations[3];

    for (std::size_t i = 0; i < kMaximumPlayers; ++i) {
        const auto pending = pending_memory_[i].view();
        if (!pending) {
            return std::unexpected(pending.error());
        }

        const auto estimates = estimate_memory_[i].view();
        if (!estimates) {
            return std::unexpected(estimates.error());
        }

        pending_state_slots_[i] = pending->data();
        round_trip_estimates_[i] = estimates->data();
    }

    log_ = context.log();
    game_ = context.game();
    multiplayer_ = context.multiplayer();
    const auto feature = multiplayer_.feature({"FC.LateUpdateRecovery", fc::FeatureRole::Host});
    if (!feature) {
        return std::unexpected(feature.error());
    }

    feature_ = *feature;

    if (auto result = feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            peer_changed(peer, agreement);
        });
        !result) {
        return result;
    }

    // Publish readiness only after copying the diagnostics table for native scheduling callbacks.
    return context.on_available("Diagnostics.NetworkDiagnostics", network_diagnostics::api_contract(),
                                [this](const DiagnosticsApi &api) noexcept {
                                    diagnostics_full_ = api.capture_mode() == 2;
                                    diagnostics_ = api;
                                    diagnostics_ready_.store(true, std::memory_order_release);
                                });
}

void Handler::peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept {
    if (agreement == fc::Agreement::Unavailable) {
        slot_peers_.withdraw(peer.value);
        return;
    }

    // Copy slot information on the control thread; native hooks read only the published atomic identity.
    const auto info = multiplayer_.info(peer);
    if (info && info->slot) {
        slot_peers_.publish(*info->slot, peer.value);
    }
}

const Handler::DiagnosticsApi *Handler::diagnostics() const noexcept {
    return diagnostics_ready_.load(std::memory_order_acquire) ? &diagnostics_ : nullptr;
}

void Handler::record_create(std::int32_t destination) noexcept {
    if (destination < 0 || destination >= static_cast<std::int32_t>(kMaximumPlayers)) {
        return;
    }

    const auto index = static_cast<std::size_t>(destination);
    const auto pending_state = *pending_state_slots_[index];
    if (pending_state == nullptr) {
        fences_[index] = {};
        return;
    }

    synchronize_window(index);

    // Borrow the map identity and turn; native code keeps ownership of its stored object baselines.
    auto &fence = fences_[index];
    fence = {
        .pending_state = pending_state,
        .turn = fc::read_field<std::int32_t>(pending_state, kObjectStateTurnOffset),
        .sent_time = *network_time_,
    };

    report_fence(destination, 1, fence);
}

void Handler::report_fence(std::int32_t destination, std::uint32_t phase, const CreateFence &before) noexcept {
    if (const auto *output = diagnostics(); output != nullptr) {
        if (!diagnostics_full_) {
            const network_diagnostics::CreateFenceObservation observation{.destination = destination, .phase = phase};
            const auto winsock_error = WSAGetLastError();
            const auto windows_error = GetLastError();
            output->create_transition(&observation);
            WSASetLastError(winsock_error);
            SetLastError(windows_error);
            return;
        }
        const auto index = static_cast<std::size_t>(destination);
        const auto after = *pending_state_slots_[index];
        const network_diagnostics::CreateFenceObservation observation{
            .destination = destination,
            .phase = phase,
            .map_before = reinterpret_cast<std::uint32_t>(before.pending_state),
            .map_after = reinterpret_cast<std::uint32_t>(after),
            .turn_before = before.turn,
            .turn_after = after != nullptr ? fc::read_field<std::int32_t>(after, kObjectStateTurnOffset) : -1,
            .sent_time = before.sent_time,
            .network_time = *network_time_,
            .average_rtt = round_trip_estimates_[index][0],
            .deviation_rtt = round_trip_estimates_[index][1]};
        const auto winsock_error = WSAGetLastError();
        const auto windows_error = GetLastError();
        output->create_transition(&observation);
        WSASetLastError(winsock_error);
        SetLastError(windows_error);
    }
}

bool Handler::fence_blocks(std::int32_t destination, bool &timed_out) noexcept {
    timed_out = false;

    if (destination < 0 || destination >= static_cast<std::int32_t>(kMaximumPlayers)) {
        return false;
    }

    const auto index = static_cast<std::size_t>(destination);
    synchronize_window(index);
    auto &fence = fences_[index];
    if (fence.pending_state == nullptr) {
        return false;
    }

    const auto live_state = *pending_state_slots_[index];
    const auto live_turn = live_state == fence.pending_state ? fc::read_field<std::int32_t>(live_state, kObjectStateTurnOffset) : 0;
    if (live_state != fence.pending_state || live_turn != fence.turn) {
        // A native response changes the pending map pointer or turn when it accepts or rejects CREATE state.
        report_fence(destination, 2, fence);
        fence = {};
        return false;
    }

    // Allow one estimated round trip, with a tenth-of-a-second minimum, for the pending map response.
    const auto *estimates = round_trip_estimates_[index];
    auto retry = estimates[0] + estimates[1];
    if (!(retry >= 0.1F)) {
        retry = 0.1F;
    }

    if (*network_time_ - fence.sent_time <= retry) {
        return true;
    }

    // Match the native CANT response: release the map's baselines, then allocate an empty pending map.
    static_cast<void>(free_object_state_.call(live_state));
    *pending_state_slots_[index] = allocate_object_state_.call().value_or(nullptr);
    report_fence(destination, 3, fence);
    fence = {};
    timed_out = true;

    if (!timeout_warning_claimed_) {
        timeout_warning_claimed_ = true;
        const auto winsock_error = WSAGetLastError();
        const auto windows_error = GetLastError();

        log_.warning("Regenerated pending CREATE state for destination {} after its response timed out", destination);

        WSASetLastError(winsock_error);
        SetLastError(windows_error);
    }

    return false;
}

void Handler::guard_acknowledgement(fc::CpuContext &context) noexcept {
    const auto *output = diagnostics();
    const auto detailed = output != nullptr && diagnostics_full_;
    // SentUpdate's cdecl argument identifies the native destination slot for this completed send.
    const auto destination = fc::read_field<std::int32_t>(context.ebp, 2 * sizeof(std::int32_t));
    network_diagnostics::SendStorageObservation observation{.destination = destination, .phase = 1};
    if (detailed) {
        observation.turn = *host_turn_;
        observation.network_time = *network_time_;
    }
    if (destination >= 0 && destination < static_cast<std::int32_t>(send_windows_.size())) {
        const auto index = static_cast<std::size_t>(destination);
        const auto peer = synchronize_window(index);
        if (detailed) {
            observation.peer_id = peer.value;
            observation.before_count = static_cast<std::uint32_t>(send_windows_[index].count());
        }
        if (peer.value != 0 && feature_.agreement(peer) == fc::Agreement::Compatible) {
            send_windows_[index].record(*host_turn_, *network_time_);
            if (detailed) {
                observation.extended_recorded = 1;
            }
        }
    }

    if (detailed && destination >= 0 && destination < static_cast<std::int32_t>(send_windows_.size())) {
        observation.after_count = static_cast<std::uint32_t>(send_windows_[static_cast<std::size_t>(destination)].count());
    }

    // SentUpdate keeps its acknowledgement slot index at EBP-4.
    const auto slot = fc::read_field<std::int32_t>(context.ebp, -static_cast<std::ptrdiff_t>(sizeof(std::int32_t)));
    if (output != nullptr) {
        const auto winsock_error = WSAGetLastError();
        const auto windows_error = GetLastError();

        observation.native_slot = slot;
        observation.native_write_skipped = slot >= 2;
        output->send_storage(&observation);

        WSASetLastError(winsock_error);
        SetLastError(windows_error);
    }

    // At slot two, native turn and timestamp writes would overwrite the first send timestamp and RTT mean.
    if (slot >= 2) {
        context.eip = reinterpret_cast<std::uint32_t>(acknowledgement_resume_);
    }
}

void Handler::sample_untracked_acknowledgement(fc::CpuContext &context) noexcept {
    // Only a nonzero ACK absent from both native slots reaches this branch.
    const auto destination = fc::read_field<std::int32_t>(context.ebp, 2 * sizeof(std::int32_t));
    const auto turn = fc::read_field<std::uint32_t>(context.ebp, 3 * sizeof(std::int32_t));
    if (destination < 0 || destination >= static_cast<std::int32_t>(send_windows_.size())) {
        return;
    }

    const auto index = static_cast<std::size_t>(destination);
    const auto peer = synchronize_window(index);
    if (peer.value == 0 || feature_.agreement(peer) != fc::Agreement::Compatible) {
        return;
    }

    const auto sent_at = send_windows_[index].sent_at(turn);
    if (!sent_at) {
        return;
    }

    // Resume the native estimator and ping update for this exact ACK; earlier native slots retain timeout/loss accounting.
    fc::write_field(context.ebp, kAcknowledgedRoundTripOffset, *network_time_ - *sent_at);
    context.eip = reinterpret_cast<std::uint32_t>(round_trip_update_);
}

void Handler::reset_world() noexcept {
    fences_.fill({});
    for (auto &window : send_windows_) {
        window.reset_world();
    }
}

void Handler::capture_create(const fc::CpuContext &context) noexcept {
    // WriteObjects sets this local only when it emitted CREATE state for the current destination.
    const auto emitted_create = fc::read_field<std::uint8_t>(context.ebp, kEmittedCreateOffset);
    if (emitted_create != 0) {
        record_create(*current_destination_);
    }
}

void Handler::gate_destination(fc::CpuContext &context) noexcept {
    // SendToClients keeps the destination being scanned in this frame local.
    const auto destination = fc::read_field<std::int32_t>(context.ebp, kDestinationOffset);
    const auto *output = diagnostics();
    CreateFence observed_fence{};
    if (output != nullptr && destination >= 0 && destination < static_cast<std::int32_t>(fences_.size())) {
        observed_fence = fences_[static_cast<std::size_t>(destination)];
    }
    bool timed_out{};
    const auto blocked = fence_blocks(destination, timed_out);

    const auto native_window_open = (context.eflags & kZeroFlag) == 0;
    // Report the decision reached by the existing gate; observation never re-runs its expiry or fence logic.
    const auto report = [this, output, destination, native_window_open, timed_out,
                         observed_fence](network_diagnostics::DestinationGateReason reason, bool extended_window = false) noexcept {
        if (output != nullptr) {
            const auto winsock_error = WSAGetLastError();
            const auto windows_error = GetLastError();
            network_diagnostics::DestinationGateObservation observation{
                .destination = destination,
                .reason = reason,
                .network_time = *network_time_,
                .native_window_open = native_window_open,
                .timed_out = timed_out,
            };
            if (destination >= 0 && destination < static_cast<std::int32_t>(send_windows_.size())) {
                const auto index = static_cast<std::size_t>(destination);
                const auto &fence = timed_out ? observed_fence : fences_[index];
                observation.peer_id = send_windows_[index].peer_identity();
                observation.outstanding = static_cast<std::uint32_t>(send_windows_[index].count());
                observation.fence_map = reinterpret_cast<std::uint32_t>(fence.pending_state);
                observation.fence_turn = fence.turn;
                observation.fence_sent_time = fence.sent_time;
                observation.average_rtt = round_trip_estimates_[index][0];
                observation.deviation_rtt = round_trip_estimates_[index][1];
                if (extended_window) {
                    observation.capacity = static_cast<std::uint32_t>(
                        PeerSendWindow::capacity(observation.average_rtt, observation.deviation_rtt, *turn_rate_));
                }
            }
            output->destination_gate(&observation);
            WSASetLastError(winsock_error);
            SetLastError(windows_error);
        }
    };

    if (blocked) {
        report(network_diagnostics::DestinationGateReason::CreateFence);
        context.eip = reinterpret_cast<std::uint32_t>(next_destination_);
        return;
    }

    // IsSendWindowOpen has already aged native slots and updated loss state; TEST EDX,EDX left its result in ZF.
    if (destination < 0 || destination >= static_cast<std::int32_t>(send_windows_.size())) {
        report(native_window_open ? network_diagnostics::DestinationGateReason::Open
                                  : network_diagnostics::DestinationGateReason::NativeWindow);
        context.eip = reinterpret_cast<std::uint32_t>(native_window_open ? continue_destination_checks_ : next_destination_);
        return;
    }

    const auto index = static_cast<std::size_t>(destination);
    const auto peer = synchronize_window(index);
    if (peer.value == 0 || feature_.agreement(peer) != fc::Agreement::Compatible) {
        // Clients without Compatible agreement retain the native window, which tracks at most two updates.
        report(native_window_open ? network_diagnostics::DestinationGateReason::Open
                                  : network_diagnostics::DestinationGateReason::NativeWindow);
        context.eip = reinterpret_cast<std::uint32_t>(native_window_open ? continue_destination_checks_ : next_destination_);
        return;
    }

    // Compatible clients use the larger ring; native slots still expire independently of cumulative progress.
    const auto *estimates = round_trip_estimates_[index];
    const auto open = send_windows_[index].can_send(*network_time_, estimates[0], estimates[1], *turn_rate_);
    report(open ? network_diagnostics::DestinationGateReason::Open : network_diagnostics::DestinationGateReason::ExtendedWindow, true);
    context.eip = reinterpret_cast<std::uint32_t>(open ? continue_destination_checks_ : next_destination_);
}

void Handler::acknowledge_update(std::int32_t destination, std::uint32_t turn) noexcept {
    if (turn == 0 || destination < 0 || destination >= static_cast<std::int32_t>(send_windows_.size())) {
        return;
    }

    const auto *output = diagnostics();
    const auto detailed = output != nullptr && diagnostics_full_;
    const auto index = static_cast<std::size_t>(destination);
    const auto peer = synchronize_window(index);
    if (peer.value != 0 && feature_.agreement(peer) == fc::Agreement::Compatible) {
        // Retire cumulative progress after native RTT processing, which samples only the exact acknowledged send.
        if (!detailed) {
            send_windows_[index].acknowledge(turn);
            return;
        }
        network_diagnostics::SendStorageObservation observation{.destination = destination,
                                                                .turn = turn,
                                                                .phase = 2,
                                                                .peer_id = peer.value,
                                                                .network_time = *network_time_,
                                                                .before_count = static_cast<std::uint32_t>(send_windows_[index].count()),
                                                                .extended_recorded = 1};
        send_windows_[index].acknowledge(turn);
        observation.after_count = static_cast<std::uint32_t>(send_windows_[index].count());
        if (output != nullptr) {
            const auto winsock_error = WSAGetLastError();
            const auto windows_error = GetLastError();
            output->send_storage(&observation);
            WSASetLastError(winsock_error);
            SetLastError(windows_error);
        }
    }
}

fc::PeerId Handler::synchronize_window(std::size_t destination) noexcept {
    // Session changes also clear state on the native thread; Create handles maps that retain the session and peer.
    if (const auto state = game_.current(); state && state->session != session_) {
        session_ = state->session;
        reset_world();
        for (auto &window : send_windows_) {
            window.synchronize(0);
        }
    }

    const auto peer = slot_peers_.current(destination);

    // Agreement discovery can identify a slot after its CREATE was sent; that map still needs its native response.
    send_windows_[destination].synchronize(peer);

    return fc::PeerId{peer};
}

} // namespace fc_plugins::update_scheduling
