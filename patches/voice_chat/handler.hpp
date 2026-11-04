#pragma once

#include "codec.hpp"
#include "layout.hpp"
#include "options.hpp"
#include "state.hpp"
#include "../voice_diagnostics/api.hpp"

#include <fc/lua.hpp>
#include <fc/game.hpp>
#include <fc/plugin.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace fc_plugins::voice_chat {

using voice_diagnostics::CodecDirection;
using voice_diagnostics::CodecResult;
using voice_diagnostics::StateResult;

// Adapts native audio blocks to 48 kHz Opus and controls remote routes through FC feature messages.
// The game owns capture, playback, packet storage and audio relay.
class Handler {
  public:
    explicit Handler(const fc::CreateContext &context) noexcept;

    // Declares native voice operations for the selected retail profile; include_client adds player volume and HUD corrections.
    void plan(fc::Plan &plan, bool include_client = false);

    // Prepares Opus, device choices, public Lua bindings and the session feature.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Retries refused control updates and reports native issue bits.
    void update(const fc::UpdateContext &context) noexcept;

  private:
    enum OptionIssue : std::uint32_t {
        DeviceEnumerationFailed = 1U << 0U,
        CaptureOpenFailed = 1U << 1U,
        PlaybackOpenFailed = 1U << 2U,
        VoiceOpenFailed = 1U << 3U,
    };

    // Rounds a millisecond buffer request to complete 960-sample Opus frames.
    static std::uint32_t calculate_samples(float milliseconds) noexcept;

    // Sets XMM0 to 20 ms before the native packet-duration store.
    static void set_packet_duration(fc::CpuContext &context) noexcept;

    // Extends only a voice capture allocation while its native open call is active.
    void restore_capture_window(fc::CpuContext &context) const noexcept;

    // Selects recipient framing for a scoped microphone test without changing the stored multiplayer method.
    void select_send_method(fc::CpuContext &context) noexcept;

    // Sends only the scoped test's self carrier through the native local queue, preserving relay submissions.
    void select_send_delivery(fc::CpuContext &context) noexcept;

    // Logs changes in the actual native test flag after native local-node and mask updates.
    void microphone_test_changed() noexcept;

    // Loads saved microphone and routing options, then calls native voice open with the original arguments.
    bool open_voice(fc::NativeFunction<GameVoiceOpen> original, float open_rate, bool open_devices) noexcept;

    // Reports the native open result and selected device for both initial opens and microphone changes.
    void report_voice_open(const fc::Result<bool> &result) noexcept;

    // Reopens an existing voice node for a changed microphone, then runs native volume and profile handling.
    void apply_online_options(fc::NativeFunction<OnlineOptionsMethod> original, void *options) noexcept;

    // Resets saved voice options to off, Windows default input, default sensitivity and activity mode.
    static void reset_online_options(fc::NativeFunction<OnlineOptionsMethod> original, void *options) noexcept;

    // Uses the selected native DirectSound device and scopes the capture allocation hook.
    bool open_capture(fc::NativeFunction<CaptureSourceOpen> original, void *source, std::uint8_t device, void *format,
                      std::uint32_t packet_bytes, std::uint8_t packet_count, void *packets) noexcept;

    // Reuses the sound engine DirectSound device for native voice playback and reports open failures.
    bool open_playback(fc::NativeFunction<AuxRendererOpen> original, void *renderer, std::uint8_t device, void *format,
                       float buffer_ms) noexcept;

    // Maps 64-bit native signal power to the selected VAD threshold outcome.
    void apply_voice_threshold(fc::CpuContext &context) const noexcept;

    // Updates atomics for the saved microphone index, sensitivity threshold and voice enablement.
    void publish_profile_options(const void *options) noexcept;

    // Calls native GSM initialization, then resets the matching prepared Opus state.
    void initialize_gsm(fc::NativeFunction<GsmInit> original, void *state) noexcept;

    // Assembles six native PCM blocks into a 20 ms Opus frame, including local microphone testing.
    void encode_gsm(void *state, const std::int16_t *pcm, std::byte *encoded) noexcept;

    // Decodes normal or recovery frames through six borrowed 160-sample native output blocks.
    void decode_gsm(void *state, const std::byte *encoded, std::int16_t *pcm) noexcept;

    // Trims the validated internal frame to the aligned native carrier payload.
    void send_audio(fc::NativeFunction<SendAudioPacket> original, void *node, AudioPacket *packet) noexcept;

    // Validates wire frames and scopes recovery to selected local queues; returns the native count of remaining recipients.
    std::uint8_t receive_audio(fc::NativeFunction<ReceiveAudioPacket> original, std::uint8_t sender, AudioPacket *packet,
                               std::uint8_t relay, std::uint16_t missing_packets) noexcept;

    // Calls native relay with a validated wire frame, exposing its sender to nested recipient checks.
    void relay_audio(fc::NativeFunction<RelayAudioPacket> original, AudioPacket *packet, std::uint8_t sender,
                     std::uint16_t sequence) noexcept;

    // Retires an empty discontinuity whose decoder input is already detached; other packets use native decode.
    bool decode_stream(fc::NativeFunction<DecodeStream> original, void *node, std::uint8_t sender, AudioPacket *packet) noexcept;

    // Accepts forward ten-bit sequences while an active stream retains progress, including rollover.
    void admit_sequence(fc::CpuContext &context) noexcept;

    // Resets only changed owners' native receive progress and queues on the native voice thread.
    void synchronize_receive_owners() noexcept;

    // Returns the host-assigned connection identity published for this player's current session.
    std::uint64_t audio_identity(std::uint8_t player) const noexcept;

    // Authenticates network-only slots and ownership before the native sequence/codec consumer.
    void *read_network_audio(fc::NativeFunction<GetNetworkPacket> original, std::uint32_t type, std::uint32_t priority) noexcept;

    // Appends the host-assigned connection identity once to each native network datagram before NetComm fan-out.
    bool send_network_audio(fc::NativeFunction<SendNetworkPacket> original, std::uint8_t destination, void *packet,
                            std::uint32_t header_bytes) noexcept;

    // Gives gameplay rows qualified recent send/receive activity; native row selection and fade remain unchanged.
    bool hud_activity(void *transport, std::uint8_t player) noexcept;

    // Returns recent local speech activity, or an expired timestamp when current HUD permission fails.
    // Optionally copies the local node lookup into the enclosing activity observation.
    float hud_last_activity(std::uint8_t player, voice_diagnostics::ActivityObservation *observation = nullptr) noexcept;

    // Requires permission to send to another player for local speech, or the current receive policy for remote speech.
    bool hud_permission(void *transport, std::uint8_t receiver, std::uint8_t source) noexcept;

    // Queues a selected local copy and records accepted current speech for the local player.
    bool enqueue_audio(void *node, std::uint8_t sender, AudioPacket *packet) noexcept;

    // Combines the native recipient bit with the host's approved sending and listening flags.
    bool select_recipient(fc::NativeFunction<GetRelayRecipient> original, void *transport, std::uint8_t destination) noexcept;

    // Preserves local echo and the sender's native recipient mask while gating sends by accepted peer state.
    bool send_possible(fc::NativeFunction<VoicePossible> original, void *transport, std::uint8_t source, std::uint8_t destination) noexcept;

    // Preserves local echo and the receiver's native listen mask while gating playback by accepted peer state.
    bool receive_possible(fc::NativeFunction<VoicePossible> original, void *transport, std::uint8_t receiver, std::uint8_t source) noexcept;

    // Answers connection and speaking queries from accepted FC flags for remote players.
    // Local devices retain their native state; all requested flags must be present.
    bool player_state(fc::NativeFunction<VoicePlayerQuery> original, void *transport, std::uint8_t player,
                      std::uint8_t required) const noexcept;

    // Publishes the native local voice ID and speaking flag from Node::Update.
    void set_sending(std::uint8_t player, bool speaking) noexcept;

    // Extends the original Online result table with saved voice options and enumerated device choices.
    void get_online_options(fc::lua::Frame &frame);

    // Validates and stores voice preferences before the native setter computes the profile checksum.
    void set_online_options(fc::lua::Frame &frame);

    // Returns combined PC glyphs (0/1/4/5) for local capture or incoming remote speech; an unmapped row returns 0.
    void voice_lobby_glyph(fc::lua::Frame &frame);

    mutable voice_diagnostics::ViewObservation last_view_{};
    mutable bool view_dirty_{true};
    mutable std::chrono::steady_clock::time_point next_view_{};

    // Tests the optional recorder before constructing observation-only fields, preserving native thread errors.
    bool diagnostics_enabled() const noexcept;

    // Resolves a numeric or numeric-string lobby row through the native player mapping.
    std::optional<std::uint8_t> lobby_player(fc::lua::Frame &frame) const;

    // Invalidates routing at session boundaries without closing the native menu microphone test.
    void game_changed(std::optional<fc::GameState> state) noexcept;

    // Records or clears the FC connection identity and native slot when feature agreement changes.
    void peer_changed(fc::PeerId peer, fc::Agreement agreement) noexcept;

    // Copies and validates complete control messages from currently compatible FC identities.
    void message_received(fc::PeerId peer, fc::Bytes payload) noexcept;

    // Applies a host or client control transition on the serialized control thread.
    void apply_session_event(const CallbackEvent &event, bool host) noexcept;

    // Retries dirty local state until Multiplayer accepts its complete message.
    void update_client_state() noexcept;

    // Sends replacement snapshots before deltas; refused sends leave a replacement pending for that peer.
    void update_server_state() noexcept;

    // Marks one published row dirty and advances a nonzero host revision.
    void mark_player_changed(std::uint8_t player) noexcept;

    // Queues one complete state record; true means FC copied it, and refusal submits nothing.
    bool send_state(PeerBinding peer, StateKind kind, std::uint8_t player, std::uint8_t flags, std::uint32_t revision) noexcept;

    // Queues the host's approved player table as a complete replacement; true means FC copied it.
    bool send_host_snapshot(PeerBinding peer) noexcept;

    // Requires Host or Client mode and negotiated host relay capability; each route checks its own direction.
    bool voice_active() const noexcept;

    // Allows host relay when approved source flags allow sending and destination flags allow listening.
    // Caller supplies zeroed exclusions; only reached FC policy checks contribute reasons.
    bool route_allowed(std::uint8_t source, std::uint8_t destination, std::uint8_t &exclusions) const noexcept;

    // Reports the native selected-node copy after it returns, preserving the borrowed descriptor and native release ownership.
    void observe_enqueue(void *node, std::uint8_t sender, const AudioPacket *packet, bool accepted) const noexcept;

    // Saves talk/listen policy while keeping device readiness independent.
    void set_local_routing(bool enabled) noexcept;

    // Publishes actual readiness through native local getters during native voice callbacks.
    void refresh_local_routing() noexcept;

    // Submits the invalid-message warning once per patch lifetime on the control thread.
    void report_rejection() noexcept;

    // Calls the optional diagnostic provider synchronously with one control decision.
    void observe_state(const CallbackEvent &event, StateResult result) const noexcept;

    // Copies the FC state used to explain one UI query.
    voice_diagnostics::UiObservation ui_observation(voice_diagnostics::UiQuery query, std::uint8_t player,
                                                    std::uint8_t receiver = kInvalidPlayer) const noexcept;

    // Sends one UI result to the optional recorder while preserving the native thread's last error.
    void observe_ui(const voice_diagnostics::UiObservation &observation) const noexcept;

    // Reports every accepted player row and current local mapping on the control thread.
    void observe_view() const noexcept;

    // Reports completed or failed codec frames without observing intermediate blocks.
    void observe_codec(void *native_state, CodecDirection direction, CodecProgress progress) const noexcept;

    fc::Logger logger_;
    CodecPool codecs_;
    CaptureDevices capture_devices_;
    fc::Multiplayer multiplayer_;
    fc::Game game_;
    fc::Feature feature_;
    ServerPeers server_peers_;
    PlayerStates remote_players_;

    // Host endpoint used by the client side, including the local client in a playing-host process.
    PeerBinding host_peer_{};
    // Rows awaiting host distribution; a refused send leaves that peer needing a full replacement.
    std::array<bool, kMaximumPlayers> changed_players_{};
    std::uint64_t session_{};
    // Last host revision applied by the client side.
    std::uint32_t host_revision_{};
    // Revision sent by the host side; several row deltas can use the same current revision.
    std::uint32_t server_revision_{1};
    // The next accepted local submission must establish a full client claim.
    bool local_snapshot_pending_{};

    // Native hooks may overlap control callbacks and read these atomics individually.
    std::atomic<fc::GameMode> mode_{fc::GameMode::None};
    // Relay capability from the accepted host snapshot, independent of host microphone preference.
    std::atomic<std::uint8_t> host_flags_{};
    // Native local voice ID; update_client_state checks it against FC's local-client slot before sending.
    std::atomic<std::uint8_t> local_player_{kInvalidPlayer};
    // Only accepted control-thread submissions advance the client revision.
    std::uint32_t local_revision_{1};
    // Saved policy, native device readiness and independently updated Speaking activity.
    std::atomic<std::uint8_t> local_flags_{};
    // Native changes and refused sends set this flag for the next control-thread update.
    std::atomic_bool local_state_dirty_{};

    bool rejection_reported_{};
    std::uint32_t reported_issues_{};
    // Device enumeration failure is reported once by the control-thread logger.
    std::atomic<std::uint32_t> option_issues_{};
    std::uint32_t reported_option_issues_{};
    std::atomic<std::uint32_t> reported_open_failures_{};

    // Normalized saved DirectSound index from the startup device list.
    std::atomic<std::uint8_t> selected_capture_{kDefaultCaptureDevice};
    std::atomic<std::uint32_t> reported_capture_choice_{0xFFFFFFFF};
    // Device recorded after a successful native voice open.
    std::atomic<std::uint8_t> applied_capture_{kDefaultCaptureDevice};
    // Mean-square PCM16 power required by the game's voice-activity check.
    std::atomic<std::uint32_t> vad_threshold_{30'000};
    // Keeps the native rate-like open argument's original float bits for device reopen.
    std::atomic<std::uint32_t> last_open_rate_{};
    // Preserves the native caller's device-open choice when changing microphones.
    std::atomic_bool last_open_devices_{};

    fc::Memory<const std::uint8_t> local_echo_memory_;
    const std::uint8_t *local_echo_enabled_{}; // Borrowed native flag, read on the native voice call thread.
    std::atomic_bool reported_microphone_test_{};
    // Native callbacks log only when method or delivery changes; frame details remain in VoiceDiagnostics.
    std::atomic<std::uint32_t> reported_transport_method_{0xFFFFFFFF};
    std::atomic<int> reported_network_delivery_{-1};

    voice_diagnostics::VoiceDiagnosticsApi diagnostics_{};
    // diagnostics_ is written before this flag is set with release ordering and then stays unchanged.
    std::atomic_bool diagnostics_ready_{};

    fc::NativeFunction<GameVoiceOpen> game_voice_open_;
    fc::NativeFunction<GameVoiceClose> game_voice_close_;
    fc::NativeFunction<GameVoiceIsOpen> game_voice_is_open_;
    fc::NativeFunction<FindVoiceNode> find_voice_node_;
    fc::NativeFunction<FindVoiceNode> get_voice_transport_;
    fc::NativeFunction<VoicePlayerQuery> is_local_;
    fc::NativeFunction<VoicePlayerQuery> send_connected_;
    fc::NativeFunction<VoicePlayerQuery> receive_connected_;
    fc::NativeFunction<VoicePlayerQuery> get_sending_original_;
    fc::NativeFunction<GetCurrentProfile> get_current_profile_;
    fc::NativeFunction<GetLobbyPlayerIndex> get_lobby_player_index_;
    fc::NativeFunction<VoicePossible> send_enable_;
    fc::NativeFunction<VoicePossible> receive_enable_;
    fc::NativeFunction<VoicePossible> send_possible_original_;
    fc::NativeFunction<VoicePossible> receive_possible_original_;
    fc::NativeFunction<OnlineOptionsMethod> apply_options_original_;
    fc::NativeFunction<OnlineOptionsMethod> reset_options_original_;
    fc::NativeFunction<GsmInit> gsm_init_original_;
    fc::NativeFunction<VoiceStreamClose> encoder_close_original_;
    fc::NativeFunction<VoiceStreamClose> decoder_close_original_;
    fc::NativeFunction<SendAudioPacket> send_audio_original_;
    fc::NativeFunction<ReceiveAudioPacket> receive_audio_original_;
    fc::NativeFunction<EnqueueAudioPacket> enqueue_audio_original_;
    fc::NativeFunction<CaptureSourceOpen> capture_original_;
    fc::NativeFunction<AuxRendererOpen> playback_original_;
    fc::NativeFunction<RelayAudioPacket> relay_audio_original_;
    fc::NativeFunction<GetRelayRecipient> relay_recipient_original_;
    fc::NativeFunction<NodeOpen> node_open_original_;
    fc::NativeFunction<NodeClose> node_close_original_;
    fc::NativeFunction<DecodeStream> decode_stream_original_;
    fc::NativeFunction<FlushPacket> flush_packet_;
    fc::NativeFunction<GetNumNodes> get_num_nodes_;
    fc::NativeFunction<GetNetworkPacket> network_read_original_;
    fc::NativeFunction<SendNetworkPacket> network_send_original_;
    fc::NativeFunction<ClassifyPacket> classify_packet_original_;
    fc::NativeFunction<PacketMethod> packet_reset_;
    fc::NativeFunction<PacketMethod> packet_release_;
    fc::NativeFunction<PacketAudio> packet_audio_;
    fc::NativeFunction<ReadHeader> read_header_;
    fc::NativeFunction<HeaderSender> header_sender_;
    fc::Memory<const std::uintptr_t> receive_metrics_memory_;
    fc::Memory<const std::byte> sequence_admission_memory_;
    std::uintptr_t sequence_admission_address_{}; // Original game instruction address used by the two admission branches.
    fc::Memory<const float> voice_time_memory_;
    fc::Memory<const std::uintptr_t> voice_nodes_memory_;
    const std::uintptr_t *receive_metrics_{};
    const float *voice_time_{};
    const std::uintptr_t *voice_nodes_{};
    // Only the native voice thread reads or changes applied owners.
    std::array<std::uint64_t, kMaximumPlayers> receive_owners_{};

    // Shares one source's last accepted speech time, in native voice seconds, with the HUD.
    struct ReceivedSpeech {
        std::atomic<float> time{-2.0F};
    };

    // Native enqueue/reset writes may overlap HUD reads; transport-only reception does not refresh these times.
    std::array<ReceivedSpeech, kMaximumPlayers> received_speech_{};
    // Client ownership values are scoped to this existing local FC host connection identity.
    std::atomic<std::uint64_t> audio_host_{};
    std::uint64_t receive_host_{};
};

} // namespace fc_plugins::voice_chat
