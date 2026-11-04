#pragma once

#include "state.hpp"

#include <fc/multiplayer.hpp>

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc_plugins::multiplayer_gc {

inline constexpr std::string_view kFeatureId = "FC.MultiplayerGC";
inline constexpr std::string_view kLobbyItem = "MultiplayerGC";
inline constexpr std::uint32_t kLobbyVersion = 3;

// Rows use native slots, never process-local FC peer identities.
struct Player {
    std::uint8_t slot{};
    std::uint8_t faction{};
    bool host{};
    std::string name;
    bool operator==(const Player &) const = default;
};

struct Snapshot {
    bool cooperative{};
    std::uint8_t host_faction{};
    std::uint8_t local_faction{};
    State state;
    std::vector<Player> players;
};

// Describes the current strategic decision and the host's native fleet presentation.
struct Decision {
    std::uint32_t id{};
    std::uint32_t turn{};
    std::uint8_t faction{};
    std::string screen;
    std::uint32_t pending_card{};
    std::array<std::int32_t, 2> commanders{-1, -1}; // Shell slots; -1 means that faction has no human commander.
    std::string presentation_phase;                 // Empty between actions; "build" or "move" during the timed preview.
    std::string presentation_source;                // The fleet's starting planet, or the construction planet for a build.
    std::uint32_t presentation_remaining_ms{};
    std::uint8_t bonus_pending_mask{}; // Bit zero is faction one; bit one is faction two.
    bool timer_enabled{};
    std::uint32_t timer_remaining_ms{};
    std::uint32_t bonus_remaining_ms{};
    bool strategic_timer_active{true}; // Host Lua distinguishes reversible summary screens from completed results.

    // Identifies changes that invalidate pending campaign actions.
    bool same_context(const Decision &other) const noexcept;
};

// Contains explicit choices for one of the supported campaign actions.
struct Action {
    std::uint32_t token{};
    std::uint32_t decision{};
    std::uint8_t faction{}; // Assigned by the host after authenticating the sender.
    std::string name;
    std::string first;
    std::string second;
    std::uint32_t index{};
    bool confirm{};
};

// Carries the host's exact selection and native setup before the normal multiplayer launch.
struct BattleSetup {
    std::uint32_t id{};
    State state;
};

// Replaces the entire accepted view, retaining join order and current faction commanders.
struct Roster {
    std::vector<Player> players;
    std::array<std::int32_t, 2> commanders{-1, -1};
};

// Accepts only the finite campaign actions implemented by the Lua rule adapter.
bool valid_action_name(std::string_view name);

// Accepts the original fleet build and move presentations, or an idle decision.
bool valid_presentation_phase(std::string_view phase);

// Keeps Results Done with the host; every other action belongs to its faction's commander.
bool action_owner(std::string_view action, bool host, bool commander);

// Keeps the current decision's faction for host Results Done; otherwise selects the commander's permitted faction.
std::optional<std::uint8_t> action_faction(const Decision &decision, std::string_view action, std::uint8_t member_faction,
                                           bool host = false);

enum class MessageKind : std::uint8_t {
    Ready = 1,
    Choose,
    CurrentState,
    Roster,
    Error,
    Action,
    ActionResult,
    BattleSetup,
    BattleReady
};

struct Message {
    MessageKind kind{};
    std::uint8_t faction{};
    Snapshot snapshot;
    Roster roster;
    std::string error;
    Decision decision;
    Action action;
    BattleSetup battle;
    std::uint32_t token{};
    bool accepted{};
};

// Writes one complete feature message. Campaign state and the complete roster travel independently.
std::vector<std::byte> encode_message(const Message &message);

// Checks the complete native-capacity fields before copying one message.
std::optional<Message> decode_message(std::span<const std::byte> bytes);

// Retains only refused current values; FC owns messages as soon as it accepts them.
struct PendingMessages {
    using Value = std::optional<std::vector<std::byte>>;
    Value ready, choose, state, roster, error, action, result, battle, acknowledgement;

    // Replaces the unsent value for this operation without accumulating history.
    void replace(const Message &message);

    // Returns fields in dependency order, with the roster installed before state and battle setup.
    std::array<Value *, 9> ordered() {
        return {&ready, &choose, &roster, &state, &error, &action, &result, &battle, &acknowledgement};
    }

    // Submits each current value once; a refused send retains it and all dependent values.
    template <class Send> std::optional<fc::SendResult> flush(Send &&send) {
        for (auto *value : ordered()) {
            if (!*value) {
                continue;
            }
            const auto send_result = send(**value);
            if (send_result != fc::SendResult::Accepted) {
                return send_result;
            }
            value->reset();
        }
        return std::nullopt;
    }

    bool empty() const {
        return !ready && !choose && !state && !roster && !error && !action && !result && !battle && !acknowledgement;
    }
};

// Cached campaign fields displayed by the native session browser.
struct SessionInfo {
    std::uint8_t count{};
    bool cooperative{};
    std::string era;
    std::string campaign;
    std::uint32_t turn{};
    std::uint8_t host_faction{};
    std::string factions;
    std::string victory_condition;
    std::string difficulty;
    bool team_damage{};
    std::string map{"Galaxy Map"};
    bool joinable{true};
    std::uint8_t maximum{}; // Effective native host limit, including the host and the faction capacity.
};

// Publishes the accepted participant count and current campaign summary.
std::vector<std::byte> encode_metadata(const SessionInfo &info);

// Reads the complete version-three FC1 campaign summary used by native browser rows.
std::optional<SessionInfo> decode_metadata(std::string_view text);

// Returns native chat exclusion bits. Unassigned and opposing slots cannot receive faction traffic.
std::uint64_t chat_exclusions(std::span<const Player> players, std::uint8_t sender, bool team_only) noexcept;

} // namespace fc_plugins::multiplayer_gc
