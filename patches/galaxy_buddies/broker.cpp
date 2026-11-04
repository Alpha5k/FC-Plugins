#include "handler.hpp"

#include <Windows.h>

#include <array>
#include <format>
#include <optional>
#include <string_view>

namespace fc_plugins::galaxy_buddies {
namespace {

// Groups the sign-in and context operations used to start the native session broker.
struct BrokerOperations {
    fc::FunctionLocation<FinishSignIn> finish_sign_in;
    fc::FunctionLocation<OperationContext> operation_context;
    fc::FunctionLocation<StartSessionBroker> start_session_broker;
};

// A supported peer can require no additional broker startup operations.
struct BrokerLayout {
    std::string_view profile;
    std::optional<BrokerOperations> startup;
};

// Only the 2017 peer needs the additional startup hook; the 2018 peer is a supported no-op.
const std::array kProfiles{
    BrokerLayout{
        .profile = "GalaxyPeer.59E6304A",
        .startup =
            BrokerOperations{
                .finish_sign_in = {.rva = {0x0028DCC0}, .name = "SignInWithSteamOperation::NotifyFinished"},
                .operation_context =
                    {
                        .rva = {0x00263510},
                        .name = "sign-in operation context",
                        .evidence = fc::exact_bytes({0x8B, 0x41, 0x34, 0xC3}),
                    },
                .start_session_broker = {.rva = {0x000C74E0}, .name = "session broker startup"},
            },
    },
    BrokerLayout{
        .profile = "GalaxyPeer.5BBE22A6",
        .startup = std::nullopt,
    },
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<BrokerLayout> select_broker_layout(std::string_view profile) {
    for (const auto &layout : kProfiles) {
        if (layout.profile == profile) {
            return layout;
        }
    }

    return std::nullopt;
}

} // namespace

void Handler::plan_broker(fc::Plan &peer) {
    const auto layout = select_broker_layout(peer.profile());
    if (!layout) {
        peer.fail(std::format("No native layout for image profile '{}'", peer.profile()));
        return;
    }
    if (!layout->startup) {
        return;
    }

    operation_context_ = peer.function(layout->startup->operation_context);
    start_session_broker_ = peer.function(layout->startup->start_session_broker);
    original_finish_sign_in_ = peer.hook(layout->startup->finish_sign_in, [this](void *operation) noexcept {
        this->finish_sign_in(operation);
    });
}

void Handler::finish_sign_in(void *operation) noexcept {
    // Completion callbacks can destroy the operation. Read its borrowed Galaxy context first;
    // the accessor does not retain that context across the original completion.
    auto *context = operation_context_.call(operation).value_or(nullptr);
    static_cast<void>(original_finish_sign_in_.call(operation));

    if (context == nullptr) {
        if (!missing_broker_context_reported_.exchange(true, std::memory_order_acq_rel)) {
            // Logging can call Win32; preserve the last error left by native completion.
            const auto last_error = GetLastError();
            logger_.warning("The Galaxy sign-in operation had no session broker context");
            SetLastError(last_error);
        }
        return;
    }

    // After completion dispatch, startup replaces the context's shared broker manager and starts account subscriptions.
    // Startup is unconditional here; its call result and asynchronous connection and delivery results are not inspected.
    static_cast<void>(start_session_broker_.call(context));
}

} // namespace fc_plugins::galaxy_buddies
