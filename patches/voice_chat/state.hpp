#pragma once

#include <fc/multiplayer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace fc_plugins::voice_chat {

// SendVoice maps destination 0xFF to NetComm's host slot 64; player rows use slots 0 through 63.
inline constexpr std::uint8_t kHostSlot = 64;
// Control-table capacity; the native voice transport separately opens 50 voice IDs.
inline constexpr std::size_t kMaximumPlayers = 64;
inline constexpr std::uint8_t kInvalidPlayer = 0xFF;

// Control messages travel through FC's negotiated feature, separately from native audio packets.
enum class StateKind : std::uint8_t {
    ClientSnapshot = 1,
    HostSnapshot = 2,
    Delta = 3,
    Resync = 4, // Accepted from the host to request new client state; this patch never emits it.
};

// Player rows separate saved talk/listen policy from native capture/render readiness.
// Host snapshot flags describe relay service independently of physical devices.
enum StateFlag : std::uint8_t {
    Talk = 1U << 0U,
    Listen = 1U << 1U,
    SendConnected = 1U << 2U,
    ReceiveConnected = 1U << 3U,
    Speaking = 1U << 4U, // Recent native sending activity for lobby icons and native voice status queries.
};

inline constexpr std::uint8_t kRoutingFlags = Talk | Listen | SendConnected | ReceiveConnected;
inline constexpr std::uint8_t kStateFlags = kRoutingFlags | Speaking;

// Combines the saved preference with independently observed native devices for one player row.
constexpr std::uint8_t local_routing_flags(bool enabled, bool capture, bool playback) noexcept {
    return static_cast<std::uint8_t>((enabled ? Talk | Listen : 0) | (capture ? SendConnected : 0) | (playback ? ReceiveConnected : 0));
}

// Tests one direction without requiring a device used only by the other direction.
constexpr bool can_send_voice(std::uint8_t flags) noexcept {
    constexpr auto required = Talk | SendConnected;
    return (flags & required) == required;
}

constexpr bool can_receive_voice(std::uint8_t flags) noexcept {
    constexpr auto required = Listen | ReceiveConnected;
    return (flags & required) == required;
}

inline constexpr std::uint8_t kControlVersion = 2;

// The record carries a client snapshot, a changed player row or a host resync request.
// The native little-endian representation is also the control wire format.
struct StateMessage {
    StateKind kind{};
    std::uint8_t version{kControlVersion};
    std::uint8_t flags{};
    // Native player ID for row messages, entry count for a host snapshot, or 0xFF for resync.
    std::uint8_t player{kInvalidPlayer};
    // Nonzero sender revision; host row deltas can share the revision of one update.
    std::uint32_t revision{};
    // Host-assigned FC connection identity for a delta; client claims and snapshot headers leave this zero.
    std::uint64_t identity{};
};

#pragma pack(push, 1)

struct PlayerState {
    std::uint8_t player{kInvalidPlayer};
    std::uint8_t flags{};
    std::uint64_t identity{};
};

#pragma pack(pop)

// Replaces the client's player table; omitted rows become zero.
struct HostSnapshot {
    // Flags advertise host relay capability; player carries the transmitted row count.
    StateMessage header{.kind = StateKind::HostSnapshot, .player = 0};
    std::array<PlayerState, kMaximumPlayers> players{};
};

// Owns a validated copy of the incoming control record and any snapshot rows.
struct ParsedState {
    StateMessage header{};
    std::array<PlayerState, kMaximumPlayers> players{};
    std::uint8_t count{};
};

static_assert(sizeof(StateMessage) == 16);
static_assert(sizeof(PlayerState) == 10);
static_assert(offsetof(HostSnapshot, players) == sizeof(StateMessage));
static_assert(std::is_standard_layout_v<StateMessage> && std::is_trivially_copyable_v<StateMessage>);
static_assert(std::is_standard_layout_v<PlayerState> && std::is_trivially_copyable_v<PlayerState>);
static_assert(std::is_standard_layout_v<HostSnapshot> && std::is_trivially_copyable_v<HostSnapshot>);

// Borrows the fixed record for Feature::send, which copies the bytes when it accepts the message.
inline std::span<const std::byte> bytes(const StateMessage &message) noexcept {
    return std::as_bytes(std::span{&message, 1});
}

// Borrows the header and populated rows for Feature::send, which copies accepted messages before returning.
inline std::span<const std::byte> bytes(const HostSnapshot &snapshot) noexcept {
    const auto count = std::min<std::size_t>(snapshot.header.player, snapshot.players.size());
    return std::as_bytes(std::span{&snapshot, 1}).first(sizeof(StateMessage) + count * sizeof(PlayerState));
}

inline bool valid_flags(std::uint8_t flags) noexcept {
    return (flags & static_cast<std::uint8_t>(~kStateFlags)) == 0;
}

// Validates one complete incoming FC control message and copies its header and player rows.
inline std::optional<ParsedState> parse_state(std::span<const std::byte> payload) noexcept {
    if (payload.size() < sizeof(StateMessage)) {
        return std::nullopt;
    }

    ParsedState parsed{};
    std::memcpy(&parsed.header, payload.data(), sizeof(parsed.header));
    if (parsed.header.version != kControlVersion || parsed.header.revision == 0 || !valid_flags(parsed.header.flags)) {
        return std::nullopt;
    }

    if (parsed.header.kind == StateKind::Resync) {
        // Resync requests carry neither a player claim nor routing flags.
        return payload.size() == sizeof(StateMessage) && parsed.header.flags == 0 && parsed.header.player == kInvalidPlayer &&
                       parsed.header.identity == 0
                   ? std::optional{parsed}
                   : std::nullopt;
    }

    if (parsed.header.kind != StateKind::HostSnapshot) {
        if (payload.size() != sizeof(StateMessage) || parsed.header.player >= kMaximumPlayers ||
            (parsed.header.kind != StateKind::ClientSnapshot && parsed.header.kind != StateKind::Delta)) {
            return std::nullopt;
        }
        return parsed;
    }

    const auto count = static_cast<std::size_t>(parsed.header.player);
    if (parsed.header.identity != 0) {
        return std::nullopt;
    }
    if (count > parsed.players.size() || payload.size() != sizeof(StateMessage) + count * sizeof(PlayerState)) {
        return std::nullopt;
    }

    // Validate every copied row before exposing the replacement, including duplicate player IDs.
    std::array<bool, kMaximumPlayers> populated{};
    std::memcpy(parsed.players.data(), payload.data() + sizeof(StateMessage), count * sizeof(PlayerState));
    for (std::size_t index = 0; index < count; ++index) {
        const auto &state = parsed.players[index];
        if (state.player >= kMaximumPlayers || !valid_flags(state.flags) || populated[state.player] || state.identity == 0) {
            return std::nullopt;
        }
        populated[state.player] = true;
    }
    parsed.count = static_cast<std::uint8_t>(count);
    return parsed;
}

// Associates a full FC connection identity with its retail NetComm slot.
// VoiceChat checks that a client player claim equals this slot; FC's PeerInfo alone makes no such promise.
struct PeerBinding {
    // Process-local connection identity; keeping it does not keep the connection alive.
    fc::PeerId id{};
    // Native player slot for a client, or slot 64 for the host endpoint.
    std::uint8_t slot{kInvalidPlayer};
    bool operator==(const PeerBinding &) const = default;
};

enum class CallbackKind : std::uint8_t {
    Available,
    Unavailable,
    Message,
    Rejected,
};

struct CallbackEvent {
    CallbackKind kind{};
    PeerBinding peer{};
    ParsedState state{};
};

// Publishes approved player flags and connection identities to concurrent native hooks.
// Flags and identities are separate atomics; clearing or replacing rows is a sequence of stores.
class PlayerStates final {
  public:
    // Clears every published row when the accepted host state is discarded.
    void clear() noexcept {
        for (auto &flags : flags_) {
            flags.store(0, std::memory_order_release);
        }
        for (auto &identity : identities_) {
            identity.store(0, std::memory_order_release);
        }
    }

    // Publishes one valid native player row to routing and UI readers.
    void set(std::uint8_t player, std::uint8_t flags, std::uint64_t identity = 0) noexcept {
        if (player < flags_.size()) {
            flags_[player].store(flags, std::memory_order_release);
            identities_[player].store(identity, std::memory_order_release);
        }
    }

    std::uint8_t get(std::uint8_t player) const noexcept {
        return player < flags_.size() ? flags_[player].load(std::memory_order_acquire) : 0;
    }

    std::uint64_t identity(std::uint8_t player) const noexcept {
        return player < identities_.size() ? identities_[player].load(std::memory_order_acquire) : 0;
    }

  private:
    std::array<std::atomic<std::uint8_t>, kMaximumPlayers> flags_{};
    std::array<std::atomic<std::uint64_t>, kMaximumPlayers> identities_{};
};

// Retains client connection identities and approves their native player claims.
// The serialized control thread changes rows; native routing reads published flags.
class ServerPeers final {
  public:
    // Discards all connection identities and routing publications at a Game session boundary.
    void reset() noexcept {
        rows_.fill({});
        states_.clear();
    }

    struct BeginResult {
        // Previously approved player cleared by replacing this slot, or 0xFF if none.
        std::uint8_t previous{kInvalidPlayer};
    };

    struct UpdateResult {
        bool changed{};
    };

    // Replaces a native slot identity and requires a fresh client snapshot before enabling its route.
    std::optional<BeginResult> begin(PeerBinding peer) noexcept {
        if (peer.id.value == 0 || peer.slot >= rows_.size()) {
            return std::nullopt;
        }

        auto &row = rows_[peer.slot];
        BeginResult result{};
        if (row.live && row.peer == peer) {
            return result;
        }
        if (row.live) {
            if (const auto previous = clear(row.peer)) {
                result.previous = *previous;
            }
        }
        row = {.peer = peer, .live = true, .snapshot_pending = true, .needs_snapshot = true};
        states_.set(peer.slot, 0, peer.id.value);
        return result;
    }

    // Accepts a matching live client's own slot and nondecreasing revision; equal revisions must repeat identical flags.
    std::optional<UpdateResult> update(PeerBinding peer, std::uint8_t player, std::uint8_t flags, std::uint32_t revision,
                                       bool snapshot) noexcept {
        if (!matches(peer) || player >= kMaximumPlayers || player != peer.slot) {
            return std::nullopt;
        }

        auto &row = rows_[peer.slot];
        if (row.needs_snapshot && !snapshot) {
            return std::nullopt;
        }
        if (revision < row.revision || (revision == row.revision && (row.needs_snapshot || states_.get(player) != flags))) {
            return std::nullopt;
        }

        // A first snapshot establishes the row even when all routing flags are zero.
        const auto changed = row.needs_snapshot || states_.get(player) != flags;
        row.revision = revision;
        row.needs_snapshot = false;
        states_.set(player, flags, peer.id.value);
        return UpdateResult{.changed = changed};
    }

    // Removes a matching connection and returns its occupied player slot for publication of a zero row.
    std::optional<std::uint8_t> clear(PeerBinding peer) noexcept {
        if (!matches(peer)) {
            return std::nullopt;
        }

        auto &row = rows_[peer.slot];
        const auto player = peer.slot;
        states_.set(player, 0);
        row = {};
        return player;
    }

    // Finds a retained native association when an expired FC peer no longer has live PeerInfo.
    std::optional<PeerBinding> find(fc::PeerId id) const noexcept {
        for (const auto &row : rows_) {
            if (row.live && row.peer.id == id) {
                return row.peer;
            }
        }
        return std::nullopt;
    }

    bool matches(PeerBinding peer) const noexcept {
        if (peer.slot >= rows_.size()) {
            return false;
        }
        const auto &row = rows_[peer.slot];
        return row.live && row.peer.id == peer.id;
    }

    std::uint8_t flags(std::uint8_t player) const noexcept {
        return states_.get(player);
    }

    std::uint64_t identity(std::uint8_t player) const noexcept {
        return states_.identity(player);
    }

    // Retains a replacement snapshot request after a refused host send.
    void require_snapshot(std::uint8_t slot) noexcept {
        if (slot < rows_.size() && rows_[slot].live) {
            rows_[slot].snapshot_pending = true;
        }
    }

    // Clears a replacement request only after Multiplayer accepts the full snapshot.
    void snapshot_sent(std::uint8_t slot) noexcept {
        if (slot < rows_.size()) {
            rows_[slot].snapshot_pending = false;
        }
    }

    // Visits current FC endpoint identities on the serialized control thread.
    template <class Callback> void for_each(Callback &&callback) const noexcept {
        for (const auto &row : rows_) {
            if (row.live) {
                callback(row.peer, row.snapshot_pending);
            }
        }
    }

    // Copies every occupied row, including a connected player with voice disabled.
    std::uint8_t copy_players(std::span<PlayerState, kMaximumPlayers> output) const noexcept {
        std::uint8_t count{};
        for (std::size_t player = 0; player < kMaximumPlayers; ++player) {
            const auto flags = states_.get(static_cast<std::uint8_t>(player));
            const auto identity = states_.identity(static_cast<std::uint8_t>(player));
            if (identity != 0) {
                output[count++] = {.player = static_cast<std::uint8_t>(player), .flags = flags, .identity = identity};
            }
        }
        return count;
    }

  private:
    struct Row {
        PeerBinding peer{};
        // Last accepted client revision for this connection.
        std::uint32_t revision{};
        bool live{};
        // A complete host snapshot still needs to be accepted by FC's outgoing queue.
        bool snapshot_pending{};
        // The client must establish its player claim with a snapshot before sending deltas.
        bool needs_snapshot{};
    };

    std::array<Row, kMaximumPlayers> rows_{};
    PlayerStates states_{};
};

} // namespace fc_plugins::voice_chat
