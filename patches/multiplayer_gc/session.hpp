#pragma once

#include "shell.hpp"
#include "participants.hpp"
#include "timers.hpp"

#include <fc/game.hpp>
#include <fc/lobby.hpp>
#include <fc/plugin.hpp>

#include <cstdint>
#include <atomic>
#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fc_plugins::multiplayer_gc {

class Transition;

struct Summary {
    std::string campaign;
    std::string era;
    std::uint32_t turn{};
    std::string factions;
    std::string victory_condition;
    std::string difficulty;
    bool team_damage{};
    std::string map{"Galaxy Map"};
    std::uint8_t maximum{}; // Copied from the native Max Players preference after the GC faction cap.
};

// Owns copied session data for Lua; the campaign pair is consumed separately from roster and phase changes.
struct Poll {
    std::string phase{"joining"};
    bool host{};
    Snapshot snapshot;
    std::string message;
    std::optional<State> pending;
    Decision decision;
    std::int32_t local_slot{-1};
    std::optional<Action> action;
    std::optional<BattleSetup> setup;
    bool launch{};
    bool returned{};
    bool action_done{};
    bool action_accepted{}; // Meaningful only when action_done reports a completed action.
    std::string action_message;
    bool timer_expired{};
    bool bonus_expired{};
    Summary summary;
};

// Serializes FC feature and Lobby work on the control thread, copying commands from Lua and native callbacks.
class Session {
  public:
    // Registers the single GC feature, its messages and the named FC Lobby item during prepare.
    fc::Result<> prepare(fc::PrepareContext &context, Shell &shell, Transition &transition);

    // Processes copied commands, current peer changes and unsent current state.
    void update() noexcept;

    // Copies already serialized native state and activates the first-advertisement gate synchronously.
    fc::Result<> host_campaign(State state, bool cooperative, std::uint8_t faction, Summary summary);

    // Marks this addon shell ready independently from feature compatibility.
    void shell_ready();

    // Queues a faction request; Poll remains authoritative about acceptance.
    fc::Result<> choose(std::uint8_t faction);

    // Clears native host intent immediately and queues control-thread session cleanup.
    void leave();

    // Returns current state; unless peeking, consumes newly received campaign data, actions, setup and action results.
    Poll poll(bool peek = false);

    // Copies browser fields without copying campaign work or the complete Lua view.
    Summary summary();

    // Publishes the saved campaign with its decision and native presentation as one copied update.
    void set_decision(Decision decision, State state);

    // Copies host options for browser publication without changing campaign decisions.
    void update_summary(Summary summary);

    // Retains the host's preference across shell VM recreation; zero disables automatic turn completion.
    void set_turn_timer(std::uint8_t minutes) {
        turn_minutes_.store(minutes, std::memory_order_release);
    }

    std::uint8_t turn_timer() const {
        return turn_minutes_.load(std::memory_order_acquire);
    }

    // Queues a local command through the same authority checks used for remote commanders.
    fc::Result<> request_action(Action action);

    // Completes the one action handed to the host Lua VM.
    void action_result(std::uint32_t token, bool accepted, std::string message);

    // Sends the host's captured native setup before any participant starts mission initialization.
    fc::Result<> prepare_battle(BattleSetup setup);

    // Acknowledges setup only after the Lua owner applied it and armed the native transition.
    void battle_ready(std::uint32_t id);

    // Observes native mission/shell continuation on the control thread without ending participant membership.
    void observe_transition(bool mission, bool shell);

  private:
    struct HostRequest {
        State state;
        Summary summary;
        bool cooperative{};
        std::uint8_t faction{};
    };

    struct Commands {
        // Keeps the saved pair with the decision captured by one Lua callback.
        struct DecisionRequest {
            Decision decision;
            State state;
        };

        bool leave{};
        bool ready{};
        std::optional<HostRequest> host;
        std::optional<std::uint8_t> choose;
        std::optional<DecisionRequest> decision;
        std::optional<Summary> summary;
        std::optional<Action> action;
        std::optional<Message> action_result;
        std::optional<BattleSetup> battle;
        std::optional<std::uint32_t> battle_ready;
    };

    // Each peer retains a few current values only while FC refuses submission.
    struct Delivery {
        fc::PeerId peer;
        PendingMessages values;
    };

    // Authenticates the current sender and applies only messages appropriate to this endpoint's role.
    void receive(fc::PeerId peer, fc::Bytes bytes);

    // Replaces a pending current value, rather than accumulating an update history.
    void queue(fc::PeerId peer, const Message &message);

    // Sends the complete roster and, when requested, the current campaign and decision.
    void send_state(fc::PeerId peer, bool campaign);

    // Copies native names into accepted participants and removes ended connections.
    bool reconcile();

    // Publishes a new copied Lua/native view without invoking native functions on this thread.
    void publish_view(bool campaign);

    // Ends feature-specific state without changing the game's ordinary native session.
    void clear(bool keep_host_intent = false);

    // Authenticates an action owner and queues explicit choices for host Lua.
    void accept_action(fc::PeerId peer, Action action);

    // Updates roster commanders and sends campaign state only when its value changed.
    void publish_decision();

    // Dispatches one queued bonus choice after rechecking its current faction and commander.
    void dispatch_bonus();

    // Samples host deadlines for real state publications and retains expiry for the Lua rule adapter.
    void update_timers();

    // Publishes browser fields only when their serialized value changes.
    void publish_metadata();

    // Reports action completion to its requesting player.
    void finish_action(fc::PeerId peer, bool accepted, std::string message);

    // Handles the commands that advance decisions, battle preparation and a completed return.
    void process_commands(Commands &commands);

    Shell *shell_{};
    Transition *transition_{};
    fc::Game game_;
    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    fc::LobbyItem item_;
    Participants participants_;
    std::vector<fc::PeerId> ready_;
    std::vector<Delivery> outgoing_;
    fc::PeerId host_, local_;
    fc::SessionId session_{};
    bool hosting_{};
    bool addon_ready_{};
    bool ready_sent_{};
    bool observed_session_{};
    bool active_{};
    Snapshot snapshot_;
    bool roster_received_{}; // Suppresses join notices from the first complete client roster.
    Summary summary_;
    std::string message_;
    enum class Phase {
        Strategic,
        Preparing,
        Battle,
        Returning
    };
    Phase phase_{Phase::Strategic};
    Decision decision_;
    BattleSetup battle_;
    std::vector<fc::PeerId> battle_ready_;
    std::optional<Action> action_;
    std::array<std::optional<std::pair<fc::PeerId, Action>>, 2> bonus_actions_;
    fc::PeerId action_peer_;
    std::uint32_t next_action_{};
    bool action_waiting_{}; // Local UI has submitted a request and is waiting for its result.
    bool returned_{};       // New shell has not yet consumed the host's campaign after a battle.
    bool decision_dirty_{};
    bool roster_dirty_{};
    std::optional<ShellSession> published_shell_;
    std::optional<bool> published_admission_;
    bool awaiting_launch_{};
    std::atomic<std::uint8_t> turn_minutes_{};
    DecisionTimers timers_;
    DecisionTimers::Clock::time_point countdown_received_{};
    std::vector<std::byte> metadata_;
    std::mutex commands_mutex_;
    Commands commands_;
    std::mutex poll_mutex_;
    Poll poll_;
};
} // namespace fc_plugins::multiplayer_gc
