#include "handler.hpp"
#include "browser.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <initializer_list>
#include <span>
#include <utility>

namespace fc_plugins::lobby_player_list {
namespace {

constexpr std::string_view kItemName = "Players";
constexpr auto kInterval = std::chrono::seconds{5};

// Names the Lobby API result without exposing its numeric representation in roster diagnostics.
std::string_view result_name(fc::LobbyResult result) noexcept {
    switch (result) {
    case fc::LobbyResult::Value:
        return "Value";
    case fc::LobbyResult::Missing:
        return "Missing";
    case fc::LobbyResult::Malformed:
        return "Malformed";
    case fc::LobbyResult::Unsupported:
        return "Unsupported";
    case fc::LobbyResult::Unavailable:
        return "Unavailable";
    case fc::LobbyResult::Failed:
        return "Failed";
    }
    return "Unknown";
}

} // namespace

void Handler::plan(fc::Plan &plan) {
    const auto layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // Capture after the game update on its native thread; control updates only request and publish copies.
    plan.observe(layout->native_update, fc::after([this](const bool &) noexcept {
                     capture_roster();
                 }));
    native_count_ = plan.function(layout->player_count);
    native_index_ = plan.function(layout->player_index);
    native_name_ = plan.function(layout->player_name);
    browser_operation_ = plan.access(layout->browser_operation);
    browser_vtable_ = plan.access(layout->browser_vtable);

    // Qualify the browser's Update slot and dirty getter, including pointers in relocated images.
    plan.check(layout->browser_vtable.rva + 4, 4, fc::points_to(plan.at(layout->browser_update.rva)));
    plan.check(layout->browser_vtable.rva + 0x5C, 4, fc::points_to(plan.at(layout->browser_dirty_lookup.rva)));
    plan.check(layout->browser_dirty_lookup.rva, 17,
               fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0x8B, 0x45, 0xFC, 0x8A, 0x40, 0x04, 0x8B, 0xE5, 0x5D, 0xC3}));
    plan.check(layout->browser_update.rva + 0xD5C, 9, fc::exact_bytes({0x8A, 0x45, 0xEA, 0x5F, 0x5E, 0x8B, 0xE5, 0x5D, 0xC3}));
    browser_update_ = plan.hook(layout->browser_update, [this](void *object) noexcept {
        const auto changed = browser_update_.call(object).value_or(false);
        // Galaxy can update rows before this call. Include pending changes so Lua rebuilds the list.
        // The native publisher clears the dirty flag after sorting and filtering.
        return changed || fc::read_field<std::uint8_t>(object, 4) != 0;
    });

    // Each absolute operand is checked against the selected image base, including relocated loads.
    const auto verify_reference = [&plan](std::uint32_t rva, std::initializer_list<std::uint8_t> instruction, std::uint32_t target) {
        plan.check(rva, instruction.size(), fc::exact_bytes(instruction));
        plan.check(plan.at(rva + static_cast<std::uint32_t>(instruction.size())), 4, fc::points_to(plan.at(target)));
    };
    verify_reference(layout->character_lookup.rva, {0x8B, 0x15}, layout->character_storage.rva + 4);
    verify_reference(layout->character_lookup.rva + 0x0E, {0x3B, 0x0D}, layout->character_storage.rva);
    verify_reference(layout->stats_lookup.rva + 0x17, {0x69, 0xC0, 0xF8, 0x01, 0x00, 0x00, 0x03, 0x05}, layout->character_storage.rva + 8);
    verify_reference(layout->kills_lookup.rva, {0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x8B, 0x80}, layout->player_totals.rva);
    verify_reference(layout->deaths_store.rva, {0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x89, 0x90}, layout->player_totals.rva + 4);
    plan.check(layout->points_lookup.rva + 9, 7, fc::exact_bytes({0x0F, 0xBF, 0x81, 0xF0, 0x01, 0x00, 0x00}));
    plan.check(layout->points_lookup.rva + 0x14, 7, fc::exact_bytes({0x0F, 0xBF, 0x81, 0xF2, 0x01, 0x00, 0x00}));
    character_storage_ = plan.access(layout->character_storage);
    player_totals_ = plan.access(layout->player_totals);
}

fc::Result<> Handler::prepare(fc::PrepareContext &context) {
    log_ = context.log();
    game_ = context.game();
    lobby_ = context.lobby();
    auto item = lobby_.item(kItemName, protocol::kVersion);
    if (!item) {
        return std::unexpected(item.error());
    }
    item_ = *item;

    auto bindings = fc::lua::get(context);
    if (!bindings) {
        return std::unexpected(bindings.error());
    }
    lua_ = *bindings;
    // Begin replaces the list and Cancel closes it; both discard the previous selection.
    for (const std::string_view name : {"ScriptCB_BeginSessionList", "ScriptCB_CancelSessionList"}) {
        auto result = lua_.bind(name, fc::lua::BindingMode::Wrap, [this](fc::lua::Frame &frame) {
            selected_.store(0, std::memory_order_release);
            browser_state_.store(0, std::memory_order_release);

            auto original = frame.original();
            if (!original) {
                frame.error(original.error().message);
                return;
            }
            frame.return_values(original->values());
        });
        if (!result) {
            return result;
        }
    }
    return lua_.bind("ScriptCB_GetExtraSessionInfo", fc::lua::BindingMode::Wrap, [this](fc::lua::Frame &frame) {
        browser_info(frame);
    });
}

fc::LobbyId Handler::selected_lobby() const noexcept {
    const auto *object = browser_operation_.read().value_or(nullptr);
    const auto table = browser_vtable_.view();
    if (!object || !table) {
        return {};
    }

    return browser::selected(object, table->data());
}

void Handler::browser_info(fc::lua::Frame &frame) {
    const auto id = frame.global("gOnlineServiceStr").as_string().value_or("") == "Galaxy" ? selected_lobby() : fc::LobbyId{};
    selected_.store(id.value, std::memory_order_release);
    browser_state_.store(frame.state().value, std::memory_order_release);

    // The original builds server details and repaints before this wrapper replaces the player table.
    auto original = frame.original();
    if (!original) {
        frame.error(original.error().message);
        return;
    }

    const auto roster = browser_.load(std::memory_order_acquire);
    if (roster && roster->lobby == id && selected_lobby() == id) {
        // Retail displays the name through IFText_fnSetUString; convert the copied native bytes on this Lua thread.
        auto contents = frame.table();
        for (std::size_t ordinal = 0; ordinal < roster->players.count; ++ordinal) {
            const auto &player = roster->players.players[ordinal];
            auto row = frame.table();
            const auto name = frame.call(frame.global("ScriptCB_tounicode"), {frame.value(player.name.data())});
            if (!name) {
                frame.error(name.error().message);
                return;
            }
            row.set("text", (*name)[0]);

            row.set("kills", frame.value(player.kills));
            row.set("deaths", frame.value(player.deaths));
            row.set("points", frame.value(player.points));
            row.set("heropoints", frame.value(player.hero_points));
            contents.set(ordinal + 1, row);
        }
        frame.global("mpsessionlist_playerlist_contents", contents);

        // Repaint the copied roster after the native callback has painted its own player table.
        const auto screen = frame.global("ifs_mp_sessionlist");
        if (screen.type() == fc::lua::Type::Table) {
            const auto repaint = screen.get("RepaintExtraInfo");
            if (repaint.type() == fc::lua::Type::Function) {
                if (auto result = frame.call(repaint, {screen}); !result) {
                    frame.error(result.error().message);
                }
            }
        }
    }

    frame.return_values(original->values());
}

void Handler::capture_roster() noexcept {
    if (!capture_requested_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const auto state = game_.current();
    if (!state || !state->is_host()) {
        captured_.store(nullptr);
        return;
    }

    try {
        captured_.store(std::make_shared<const CapturedRoster>(CapturedRoster{state->session, read_roster()}), std::memory_order_release);
    } catch (...) {
        captured_.store(nullptr, std::memory_order_release);
    }
}

protocol::Snapshot Handler::read_roster() const noexcept {
    protocol::Snapshot result{};
    const auto count = std::clamp(native_count_.call().value_or(0), 0, static_cast<int>(protocol::kMaximumPlayers));
    const auto storage = character_storage_.read();
    const auto totals = player_totals_.view();
    if (!storage || !totals) {
        return result;
    }

    // Ordinals enumerate sparse native slots. Copy at most 31 bytes per name, retaining no native pointers.
    for (auto ordinal = 0; ordinal < count; ++ordinal) {
        const auto index = native_index_.call(ordinal).value_or(-1);
        if (index < 0 || index >= static_cast<int>(protocol::kMaximumPlayers)) {
            continue;
        }

        const auto name = native_name_.call(index).value_or(nullptr);
        if (!name || !*name) {
            continue;
        }

        auto &player = result.players[result.count++];
        player.index = static_cast<std::uint8_t>(index);
        std::memcpy(player.name.data(), name, std::min(::strnlen_s(name, protocol::kMaximumNameSize + 1), protocol::kMaximumNameSize));
        const auto offset = static_cast<std::size_t>(index) * kPlayerTotalsStride;
        player.kills = fc::read_field<std::int32_t>(totals->data(), offset);
        player.deaths = fc::read_field<std::int32_t>(totals->data(), offset + 4);

        // GameSpy publishes zero scores when no Character exists; retain its network kills/deaths.
        if (storage->characters && storage->stats && index < storage->count) {
            const auto *stats = reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(storage->stats)) +
                                static_cast<std::size_t>(index) * kPlayerStatsStride;
            player.points = fc::read_field<std::int16_t>(stats, kPlayerPointsOffset);
            player.hero_points = fc::read_field<std::int16_t>(stats, kHeroPointsOffset);
        }
    }
    return result;
}

void Handler::refresh(fc::LobbyId id) {
    if (requested_browser_ == id && browser_pending_) {
        return;
    }

    // Replacing the request cancels the previous read for this destination.
    browser_request_ = {};
    browser_pending_ = false;
    requested_browser_ = id;
    if (!id.value) {
        return;
    }

    // Install a copied cache value before requesting fresh data; cached rosters may be stale.
    if (const auto cached = lobby_.cached(id, kItemName, protocol::kVersion)) {
        accept(*cached);
    }

    auto result = lobby_.refresh(id, kItemName, protocol::kVersion, [this](fc::LobbyRecord record) noexcept {
        browser_pending_ = false;
        accept(std::move(record));
    });
    if (result) {
        browser_request_ = std::move(*result);
        browser_pending_ = true;
    } else {
        log_.warning("Browser roster request for lobby {} failed: {}", id.value, result.error().message);
        accept({id, fc::LobbyResult::Failed, protocol::kVersion, {}, {}});
    }
}

void Handler::accept(fc::LobbyRecord record) noexcept {
    try {
        const fc::LobbyId current{selected_.load(std::memory_order_acquire)};
        if (record.lobby != current) {
            return;
        }

        // Missing, failed or malformed records clear the copied roster; the browser keeps the native result.
        const auto decoded = Roster::decode(record, current);
        auto roster = decoded ? std::make_shared<const Roster>(*decoded) : nullptr;
        const auto previous = browser_.load(std::memory_order_acquire);
        if (browser_result_ != record.result || bool(previous) != bool(roster) ||
            (previous && roster && previous->players.count != roster->players.count)) {
            log_.debug("Browser roster for lobby {}: result={}, decoded={}, players={}", current.value, result_name(record.result),
                       decoded.has_value(), decoded ? decoded->players.count : 0);
        }
        browser_result_ = record.result;
        browser_.store(std::move(roster), std::memory_order_release);

        const fc::lua::StateId state{browser_state_.load(std::memory_order_acquire)};
        lua_.defer(state, [this, current](fc::lua::Frame &frame) {
            if (selected_.load(std::memory_order_acquire) != current.value || selected_lobby() != current) {
                return;
            }

            // Run the wrapper on its Lua owner to replace and repaint the player table.
            const auto query = frame.global("ScriptCB_GetExtraSessionInfo");
            if (query.type() == fc::lua::Type::Function) {
                if (auto result = frame.call(query, {}); !result) {
                    frame.error(result.error().message);
                    return;
                }
            }
        });
    } catch (...) {
        browser_.store(nullptr, std::memory_order_release);
    }
}

void Handler::update(const fc::UpdateContext &) noexcept {
    try {
        const auto state = game_.current();
        const auto id = lobby_.current();
        const auto now = std::chrono::steady_clock::now();
        const auto hosting = state && state->is_host() && id.value;

        // Remove the previous host publication when lobby, session or host ownership changes.
        if (!hosting || published_lobby_ != id || published_session_ != state->session) {
            item_.clear();
            published_lobby_ = hosting ? id : fc::LobbyId{};
            published_session_ = hosting ? state->session : 0;
            next_publish_ = {};
            published_count_.reset();
        }

        // The next native game update performs the capture; publication can lag this five-second request.
        if (hosting && now >= next_publish_) {
            capture_requested_.store(true, std::memory_order_release);
            next_publish_ = now + kInterval;
        }

        if (const auto captured = captured_.exchange(nullptr, std::memory_order_acq_rel);
            hosting && captured && captured->session == state->session) {
            std::array<char, protocol::kMaximumValueSize> encoded{};
            std::size_t size{};
            if (protocol::encode(captured->players, encoded, size)) {
                const auto result = item_.publish(std::as_bytes(std::span{encoded}).first(size));
                if (result) {
                    if (published_count_ != captured->players.count) {
                        log_.debug("Host roster accepted for lobby {}: players={}", id.value, captured->players.count);
                        published_count_ = captured->players.count;
                    }
                } else {
                    log_.warning("Host roster publication for lobby {} failed: {}", id.value, result.error().message);
                }
            }
        }

        // Joining can replace the native browser without calling CancelSessionList.
        if (state && state->mode != fc::GameMode::None) {
            selected_.store(0, std::memory_order_release);
            browser_state_.store(0, std::memory_order_release);
        }

        const fc::LobbyId browser{selected_.load(std::memory_order_acquire)};
        const auto due = now >= next_refresh_;
        if (browser != requested_browser_ || (browser.value && due)) {
            if (browser != requested_browser_) {
                browser_.store(nullptr);
                browser_result_.reset();
            }
            refresh(browser);
            next_refresh_ = now + kInterval;
        }
    } catch (...) {
        browser_.store(nullptr);
    }
}

} // namespace fc_plugins::lobby_player_list
