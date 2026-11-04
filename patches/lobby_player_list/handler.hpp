#pragma once

#include "layout.hpp"
#include "roster.hpp"
#include <fc/plugin.hpp>
#include <fc/game.hpp>
#include <fc/lua.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace fc_plugins::lobby_player_list {

// Publishes host rosters and copies FC lobby items for the server browser player table.
class Handler final {
  public:
    // Plans native host roster reads, capture and browser access.
    void plan(fc::Plan &plan);

    // Registers Players version 2 and wraps the browser's native Lua callbacks.
    fc::Result<> prepare(fc::PrepareContext &context);

    // Publishes completed host captures and refreshes the selected browser lobby on the control thread.
    void update(const fc::UpdateContext &) noexcept;

  private:
    // Reads browser selection only on its owning Lua thread, without changing native iteration cursors.
    fc::LobbyId selected_lobby() const noexcept;

    // Calls the native browser callback, then replaces its player table when the copied roster matches selection.
    void browser_info(fc::lua::Frame &frame);

    // Captures game-owned session/player data only at the native game-update boundary.
    void capture_roster() noexcept;

    // Copies native names, network kills/deaths and signed player/hero scores for host publication.
    protocol::Snapshot read_roster() const noexcept;

    // Keeps each asynchronous request until completion; changing its lobby cancels the previous read.
    void refresh(fc::LobbyId id);

    // Rejects results for another selection and publishes immutable snapshots for the browser Lua reader.
    void accept(fc::LobbyRecord record) noexcept;

    struct CapturedRoster {
        fc::SessionId session; // FC session that owned the native capture.
        protocol::Snapshot players;
    };

    std::atomic_bool capture_requested_{};                        // Control thread requests one capture at the next native update.
    std::atomic<std::shared_ptr<const CapturedRoster>> captured_; // Native thread transfers an immutable copy to the control thread.

    fc::NativeFunction<LobbyPlayerCount> native_count_;
    fc::NativeFunction<LobbyPlayerIndex> native_index_;
    fc::NativeFunction<LobbyPlayerName> native_name_;
    fc::Memory<void *const> browser_operation_;
    fc::Memory<const void *const> browser_vtable_;
    fc::NativeFunction<BrowserUpdate> browser_update_;
    fc::Memory<const CharacterStorage> character_storage_;
    fc::Memory<const std::byte> player_totals_;

    fc::Game game_;
    fc::Lobby lobby_;
    fc::LobbyItem item_;
    fc::lua::Bindings lua_;
    fc::Logger log_;

    std::atomic<std::uint64_t> selected_{};      // Browser Lua thread publishes the selected Galaxy lobby for control reads.
    std::atomic<std::uint64_t> browser_state_{}; // Owning Lua state used for deferred refresh and repaint.
    std::atomic<std::shared_ptr<const Roster>> browser_;
    bool browser_pending_{};
    std::optional<fc::LobbyResult> browser_result_; // Last reported result for the selected lobby.
    std::optional<std::size_t> published_count_;    // Last reported host roster count.

    // Each Request owns its callback storage and stays on the control thread.
    // Replacing it cancels delivery before freeing that storage.
    fc::Request browser_request_;
    fc::LobbyId requested_browser_{};
    fc::LobbyId published_lobby_{};
    fc::SessionId published_session_{};
    std::chrono::steady_clock::time_point next_publish_{};
    std::chrono::steady_clock::time_point next_refresh_{};
};

} // namespace fc_plugins::lobby_player_list
