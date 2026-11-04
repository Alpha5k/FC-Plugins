#include "handler.hpp"

#include <Windows.h>
#include <opus.h>

namespace fc_plugins::voice_chat {

Handler::Handler(const fc::CreateContext &context) noexcept : logger_(context.log()) {}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    auto local_echo = local_echo_memory_.view();
    if (!local_echo) {
        return std::unexpected(local_echo.error());
    }
    local_echo_enabled_ = local_echo->data();
    auto sequence_admission = sequence_admission_memory_.view();
    if (!sequence_admission) {
        return std::unexpected(sequence_admission.error());
    }
    sequence_admission_address_ = reinterpret_cast<std::uintptr_t>(sequence_admission->data());

    auto metrics = receive_metrics_memory_.view();
    auto time = voice_time_memory_.view();
    auto nodes = voice_nodes_memory_.view();
    if (!metrics || !time || !nodes) {
        return std::unexpected(!metrics ? metrics.error() : !time ? time.error() : nodes.error());
    }
    receive_metrics_ = metrics->data();
    voice_time_ = time->data();
    voice_nodes_ = nodes->data();

    // Preparation also runs for dedicated launches, although their normal audio path only relays packets.
    if (!capture_devices_.prepare()) {
        option_issues_.fetch_or(DeviceEnumerationFailed, std::memory_order_relaxed);
    }
    if (auto result = codecs_.prepare(); !result) {
        return result;
    }

    // FC negotiates this exact feature ID; control version two must agree before routes admit identity-tagged native audio.
    multiplayer_ = context.multiplayer();
    auto feature = multiplayer_.feature({"FC.VoiceChat", fc::FeatureRole::Both});
    if (!feature) {
        return std::unexpected(feature.error());
    }
    feature_ = *feature;
    if (feature_.max_payload() < sizeof(HostSnapshot)) {
        return std::unexpected(fc::Error{"Voice control snapshots exceed the feature payload limit"});
    }

    game_ = context.game();
    if (auto result = game_.on_change([this](std::optional<fc::GameState> state) noexcept {
            game_changed(state);
        });

        !result) {
        return result;
    }

    if (auto result = feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            peer_changed(peer, agreement);
        });

        !result) {
        return result;
    }

    if (auto result = feature_.on_message([this](fc::PeerId peer, fc::Bytes payload) noexcept {
            message_received(peer, payload);
        });

        !result) {
        return result;
    }

    // Store the optional provider table before setting the ready flag that native callbacks read.
    if (auto result = context.on_available("Diagnostics.VoiceDiagnostics", voice_diagnostics::api_contract(),
                                           [this](voice_diagnostics::VoiceDiagnosticsApi api) noexcept {
                                               diagnostics_ = api;
                                               diagnostics_ready_.store(true, std::memory_order_release);
                                           });

        !result) {
        return result;
    }

    auto bindings = fc::lua::get(context);
    if (!bindings) {
        return std::unexpected(bindings.error());
    }

    // Extend the native settings table while retaining its apply and checksum path.
    if (auto result = bindings->bind("ScriptCB_GetOnlineOpts", fc::lua::BindingMode::Wrap,
                                     [this](fc::lua::Frame &frame) {
                                         get_online_options(frame);
                                     });

        !result) {
        return result;
    }

    if (auto result = bindings->bind("ScriptCB_SetOnlineOpts", fc::lua::BindingMode::Wrap,
                                     [this](fc::lua::Frame &frame) {
                                         set_online_options(frame);
                                     });

        !result) {
        return result;
    }

    // Combined PC glyphs have a separate contract from native directional permission statuses.
    if (auto result = bindings->bind("ScriptCB_FC_GetVoiceLobbyGlyph", fc::lua::BindingMode::Add,
                                     [this](fc::lua::Frame &frame) {
                                         voice_lobby_glyph(frame);
                                     });

        !result) {
        return result;
    }
    return {};
}

void Handler::update(const fc::UpdateContext &) noexcept {
    // A playing host distributes approved rows and also submits state for its distinct local client.
    const auto mode = mode_.load(std::memory_order_acquire);
    if (mode == fc::GameMode::Host || mode == fc::GameMode::Dedicated) {
        update_server_state();
    }
    if (mode == fc::GameMode::Host || mode == fc::GameMode::Client) {
        update_client_state();
    }

    observe_view();

    // Audio callbacks publish issue bits; logging stays on the control thread.
    const auto issues = codecs_.take_issues();
    const auto fresh = issues & ~reported_issues_;
    reported_issues_ |= issues;
    if ((fresh & CodecPool::NoEncoder) != 0) {
        logger_.error("No prepared Opus encoder was available for the voice stream; the voice frame was dropped");
    }
    if ((fresh & CodecPool::NoDecoder) != 0) {
        logger_.error("No prepared Opus decoder was available for the voice stream; affected audio was silenced");
    }
    if ((fresh & CodecPool::EncodeFailed) != 0) {
        const auto result = codecs_.encode_result();
        if (result < 0) {
            logger_.warning("Could not encode a voice frame: {} ({})", opus_strerror(result), result);
        } else {
            logger_.warning("Opus returned {} encoded bytes without a usable voice frame; the packet was dropped", result);
        }
    }
    if ((fresh & CodecPool::DecodeFailed) != 0) {
        const auto result = codecs_.decode_result();
        if (result < 0) {
            logger_.warning("Could not decode a voice frame: {} ({})", opus_strerror(result), result);
        } else {
            logger_.warning("Opus returned {} decoded samples; expected {}; affected audio was silenced", result, kFrameSamples);
        }
    }

    const auto options = option_issues_.exchange(0, std::memory_order_relaxed);
    const auto fresh_options = options & ~reported_option_issues_;
    reported_option_issues_ |= options;
    if ((fresh_options & DeviceEnumerationFailed) != 0) {
        logger_.warning("Could not enumerate microphones (DirectSound HRESULT 0x{:08X}); the Windows default input choice remains listed",
                        static_cast<std::uint32_t>(capture_devices_.enumeration_error()));
    }
}

void Handler::report_rejection() noexcept {
    if (!rejection_reported_) {
        rejection_reported_ = true;
        logger_.warning("Ignored an invalid voice control message");
    }
}

void Handler::observe_state(const CallbackEvent &event, StateResult result) const noexcept {
    if (result != StateResult::Rejected) {
        view_dirty_ = true;
    }
    if (!diagnostics_enabled()) {
        return;
    }
    const auto &state = event.state.header;
    const voice_diagnostics::StateObservation observation{.peer_id = event.peer.id.value,
                                                          .revision = state.revision,
                                                          .peer_slot = event.peer.slot,
                                                          .player = state.kind == StateKind::HostSnapshot ? kInvalidPlayer : state.player,
                                                          .flags = state.flags,
                                                          .message_kind = static_cast<std::uint8_t>(state.kind),
                                                          .result = result};
    const auto error = GetLastError();
    // The provider borrows this stack record synchronously; preserve the originating thread's Windows error.
    diagnostics_.state(&observation);
    if (result == StateResult::Accepted && state.kind == StateKind::HostSnapshot) {
        for (std::size_t index = 0; index < event.state.count; ++index) {
            auto row = observation;
            row.player = event.state.players[index].player;
            row.flags = event.state.players[index].flags;
            row.result = StateResult::SnapshotRow;
            diagnostics_.state(&row);
        }
    }
    SetLastError(error);
}

void Handler::observe_codec(void *native_state, CodecDirection direction, CodecProgress progress) const noexcept {
    if (progress == CodecProgress::Pending || !diagnostics_enabled()) {
        return;
    }
    const voice_diagnostics::CodecObservation observation{.native_state = reinterpret_cast<std::uintptr_t>(native_state),
                                                          .direction = direction,
                                                          .result = progress == CodecProgress::Complete ? CodecResult::Complete
                                                                                                        : CodecResult::Failed};
    const auto error = GetLastError();
    // The provider borrows this stack record synchronously; preserve the sound thread's Windows error.
    diagnostics_.codec(&observation);
    SetLastError(error);
}

} // namespace fc_plugins::voice_chat
