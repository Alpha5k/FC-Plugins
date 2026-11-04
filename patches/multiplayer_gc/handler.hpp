#pragma once

#include "layout.hpp"
#include "discovery.hpp"
#include "session.hpp"
#include "transition.hpp"

#include <fc/lua.hpp>
#include <fc/plugin.hpp>

#include <cstdint>
#include <span>

namespace fc_plugins::multiplayer_gc {

// Connects the GC Lua shell to saved campaign data, native discovery, participants and chat.
class Handler final {
  public:
    // Checks native save instructions and reserves access to the campaign and mission buffers.
    void plan(fc::Plan &plan);

    // Registers Lua commands for current and future VMs.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Advances copied GC commands and public FC messages on the control thread.
    void update(const fc::UpdateContext &) noexcept;

  private:
    // Views the four native ranges used during one Lua callback.
    struct Storage {
        std::span<char> campaign;
        std::span<std::uint32_t> campaign_length;
        std::span<char> mission;
        std::span<std::uint32_t> mission_length;
    };

    // Checks access to all four ranges before a callback reads or modifies native storage.
    fc::Result<Storage> storage() const;

    // Copies the saved campaign and mission bytes with their recorded lengths.
    fc::Result<State> capture() const;

    // Restores captured bytes on the Lua VM's owning thread after checking access to both buffers.
    fc::Result<> restore(const State &state);

    // Registers campaign entry, participant commands and the update bridge.
    fc::Result<> bind_session(fc::lua::Bindings &bindings);

    // Binds native browser selection, filters and copied session details.
    fc::Result<> bind_browser(fc::lua::Bindings &bindings);

    // Binds native chat display, editor servicing and configured shell input.
    fc::Result<> bind_chat(fc::lua::Bindings &bindings);

    // Creates one copied table on the invoking Lua owner thread.
    void poll(fc::lua::Frame &frame);

    // Binds commander requests, battle setup acknowledgement and current decision publication.
    fc::Result<> bind_campaign(fc::lua::Bindings &bindings);

    // Returns true on success, or nil and the operation's error.
    static void return_result(fc::lua::Frame &frame, const fc::Result<> &result);

    fc::Memory<char> campaign_;
    fc::Memory<std::uint32_t> campaign_length_;
    fc::Memory<char> mission_;
    fc::Memory<std::uint32_t> mission_length_;
    Shell shell_;
    Discovery discovery_;
    Session session_;
    Transition transition_;
    std::optional<BattleSetup> pending_setup_;
};

} // namespace fc_plugins::multiplayer_gc
