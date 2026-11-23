#include "discovery.hpp"
#include "layout.hpp"

#include <array>
#include <cstring>
#include <format>
#include <string_view>

namespace fc_plugins::multiplayer_gc {

void Discovery::plan(fc::Plan &input) {
    auto game = input.for_image("game");
    const auto layout = select_shell_layout(game.profile());
    if (!layout) {
        game.fail(std::format("No native layout for image profile '{}'", game.profile()));
        return;
    }
    // The GC tab searches for its own metadata. Ordinary browsing retains the stock cancellation filter.
    game.hook_instruction(
        layout->search_filter,
        [this](fc::CpuContext &cpu) noexcept {
            if (selected_.load(std::memory_order_acquire)) {
                const auto stack = fc::stack_pointer(cpu);
                fc::write_field<const char *>(stack, 0, "FC.MultiplayerGC");
                fc::write_field<const char *>(stack, 4, "");
            }
        },
        fc::exact_bytes({0x8B, 0x10, 0x8B, 0xC8, 0xFF, 0x52, 0x0C}));

    // GC uses the cancellation flag to hide from stock searches. Keep parsing all its ordinary fields.
    game.hook_instruction(
        layout->reader_cancelled,
        [this](fc::CpuContext &cpu) noexcept {
            if (!selected_.load(std::memory_order_acquire)) {
                return;
            }
            const auto *id = fc::read_field<const std::uint64_t *>(fc::frame_pointer(cpu), 8);
            if (id && read(fc::read_field<std::uint64_t>(id))) {
                cpu.eax &= ~0xFFu;
            }
        },
        fc::exact_bytes({0x8A, 0xD8, 0x83, 0xC4, 0x08}));

    // Use the accepted campaign count after the native membership and password checks.
    game.hook_instruction(
        layout->reader_count,
        [this](fc::CpuContext &cpu) noexcept {
            if (!selected_.load(std::memory_order_acquire)) {
                return;
            }
            const auto *id = fc::read_field<const std::uint64_t *>(fc::frame_pointer(cpu), 8);
            const auto info = id ? read(fc::read_field<std::uint64_t>(id)) : std::nullopt;
            // An explicit GC list includes only qualified GC metadata, even when a stock row advertises a positive count.
            fc::write_field<std::uint32_t>(fc::stack_pointer(cpu), 0x78, info ? info->count : 0u);
        },
        fc::exact_bytes({0x83, 0x7C, 0x24, 0x78, 0x00}));

    // The standalone EXE hotfix already advances this iterator. Apply the runtime repair only to stock code.
    constexpr std::array<std::uint8_t, 42> kFixedRemoval{
        0x8B, 0x45, 0xF4, 0x8B, 0x48, 0x0C, 0x85, 0xC9, 0x74, 0x4E, 0x89, 0x4D, 0xFC, 0x8B, 0x50, 0x04, 0x89, 0x55, 0xF4, 0x8D, 0x95,
        0x68, 0xFE, 0xFF, 0xFF, 0x52, 0xE8, 0x07, 0xE9, 0xFF, 0xFF, 0x0F, 0xB6, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const auto removal = game.read_array<std::uint8_t>(layout->reader_remove_advance - 0x2A, kFixedRemoval.size());
    if (!removal) {
        game.fail("Could not read the lobby removal loop.");
        return;
    }
    if (!std::ranges::equal(*removal, kFixedRemoval)) {
        game.hook_instruction(
            layout->reader_remove_advance,
            [](fc::CpuContext &cpu) noexcept {
                advance_result_iterator(reinterpret_cast<void *>(fc::frame_pointer(cpu)));
            },
            fc::exact_bytes({0x85, 0xC0, 0x74, 0x28, 0x8B, 0x4D, 0xFC}));
    }

    struct BrowserProfile {
        std::string_view profile;
        std::uint32_t operation, table;
    };

    // Existing CNetFindGame evidence qualifies the same +0x64 selection and valid Galaxy row layout.
    static constexpr std::array kProfiles{
        BrowserProfile{.profile = "BF2.Steam.59EDE353", .operation = 0x1AAED40, .table = 0x3AC340},
        BrowserProfile{.profile = "BF2.GOG.59EDF52B", .operation = 0x1AB01F4, .table = 0x3AD2AC},
    };
    for (const auto &profile : kProfiles) {
        if (profile.profile == game.profile()) {
            browser_ = game.access<void *const>(profile.operation);
            browser_table_ = game.access<const std::byte>(profile.table);
        }
    }
    // The empty-map branch also skips the stock type that GetSessionData requires before Lua can see the row.
    game.hook_instruction(
        layout->browser_map_gate,
        [this](fc::CpuContext &cpu) noexcept {
            const auto *row = fc::read_field<const void *>(fc::frame_pointer(cpu), -4);
            cpu.eax = gc_map_condition(cpu.eax, selected_.load(std::memory_order_acquire), row, [this](std::uint64_t lobby) {
                return read(lobby);
            });
        },
        fc::exact_bytes({0x85, 0xC0, 0x74, 0x63}));

    sort_ = game.access<std::uint32_t>(layout->browser_sort);
    direction_ = game.access<std::uint8_t>(layout->browser_direction);
    game.check(layout->browser_sort_reference, 4, fc::points_to(game.at(layout->browser_sort)));
    game.check(layout->browser_direction_reference, 4, fc::points_to(game.at(layout->browser_direction)));
    // Selection retains the original callback's extra-info request and native ordinal semantics.
    select_session_ = game.function<SelectSession>(layout->browser_select);
    for (const auto &profile : kProfiles) {
        if (profile.profile == game.profile()) {
            game.check(profile.table + 0x4C, 4, fc::points_to(game.at(layout->browser_select)));
        }
    }
    resort_ = game.hook<ResortSessions>(
        layout->browser_resort,
        [this](std::uint8_t reset) noexcept {
            auto *browser = const_cast<void *>(browser_.read().value_or(nullptr));
            const auto table = browser_table_.view();
            if (!selected_.load(std::memory_order_acquire) || !browser || !table ||
                fc::read_field<const void *>(browser) != table->data()) {
                static_cast<void>(resort_.call(reset));
                return;
            }
            const auto sort = sort_.read().value_or(2);
            const bool campaign_sort = sort >= 5 && sort <= 7;
            // Empty missions cannot enter stock mode parsing. Its Ping/name comparator supplies the same fallback order.
            if (campaign_sort) {
                static_cast<void>(sort_.write(4));
            }
            const auto result = resort_.call(std::uint8_t{});
            if (campaign_sort) {
                static_cast<void>(sort_.write(sort));
            }
            if (!result) {
                return;
            }
            try {
                const auto first =
                    filter_browser_rows(browser, filters_, sort, direction_.read().value_or(1) != 0, [this](std::uint64_t lobby) {
                        return read(lobby);
                    });
                if (reset && first) {
                    static_cast<void>(select_session_.call(browser, *first));
                }
            } catch (...) {
                // Shell callbacks cannot propagate allocation failures from the temporary metadata rows.
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0xB8, 0xD8, 0xFA, 0x00, 0x00}));
    auto galaxy = input.for_image("galaxy");
    if (galaxy.profile() != "Galaxy.59E63011") {
        galaxy.fail(std::format("No native layout for image profile '{}'", galaxy.profile()));
        return;
    }
    // These relocation-stable facade thunks and their vtable slots are already qualified by core Galaxy integration.
    instance_ = galaxy.function<GalaxyInstance>(0x2FBD, fc::exact_bytes({0xE9, 0x4E, 0x16, 0x0A, 0x00}));
    matchmaking_ = galaxy.function<GalaxyGetter>(0x1645, fc::exact_bytes({0xE9, 0xE6, 0x05, 0x07, 0x00}));
    value_ = galaxy.function<GalaxyValue>(0x2FC2, fc::exact_bytes({0xE9, 0xF9, 0x9B, 0x07, 0x00}));
    factory_table_ = galaxy.access<const std::byte>(0x36F09C);
    matchmaking_table_ = galaxy.access<const std::byte>(0x36F680);
    galaxy.check(0x36F09C + 8 * 4, 4, fc::points_to(galaxy.at(0x1645)));
    galaxy.check(0x36F680 + 19 * 4, 4, fc::points_to(galaxy.at(0x2FC2)));
}

void Discovery::select(bool active) noexcept {
    const auto previous = selected_.load(std::memory_order_acquire);
    if (previous == active) {
        return;
    }
    const auto mode = sort_.read();
    const auto direction = direction_.read();
    if (mode && direction) {
        const auto restore = browser_sort_transition(previous, active, {*mode, *direction}, source_sorts_);
        if (restore) {
            // SetSessionSortMode toggles direction for the same mode; source restoration must copy the pair directly.
            static_cast<void>(sort_.write(restore->mode));
            static_cast<void>(direction_.write(restore->direction));
        }
    }
    selected_.store(active, std::memory_order_release);
}

std::optional<SessionInfo> Discovery::read(std::uint64_t lobby) const noexcept {
    if (!lobby) {
        return std::nullopt;
    }
    const auto factory_table = factory_table_.view();
    const auto matchmaking_table = matchmaking_table_.view();
    auto *factory = instance_.call().value_or(nullptr);
    if (!factory || !factory_table || fc::read_field<const void *>(factory) != factory_table->data()) {
        return std::nullopt;
    }
    auto *matchmaking = matchmaking_.call(factory).value_or(nullptr);
    if (!matchmaking || !matchmaking_table || fc::read_field<const void *>(matchmaking) != matchmaking_table->data()) {
        return std::nullopt;
    }
    std::array<char, 4096> text{};
    if (!value_.call(matchmaking, lobby, "FC.MultiplayerGC", text.data(), static_cast<std::uint32_t>(text.size()))) {
        return std::nullopt;
    }
    const std::string_view value{text.data(), ::strnlen_s(text.data(), text.size())};
    // Shell result callbacks cannot propagate allocation failures from the owned summary strings.
    try {
        return decode_metadata(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SessionInfo> Discovery::session_info(std::optional<std::int32_t> index) const noexcept {
    if (!selected_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    auto *browser = const_cast<void *>(browser_.read().value_or(nullptr));
    const auto table = browser_table_.view();
    if (!browser || !table || fc::read_field<const void *>(browser) != table->data()) {
        return std::nullopt;
    }
    const auto selected = index.value_or(fc::read_field<std::int32_t>(browser, 0x64));
    const auto count = fc::read_field<std::int32_t>(browser, 0x1C);
    // AddSession's native capacity bounds traversal, including the terminal node without a payload.
    if (selected < 0 || selected >= count || count > 2000) {
        return std::nullopt;
    }
    std::optional<SessionInfo> result;
    visit_browser_rows(browser, [&](void *row, std::int32_t ordinal) {
        if (ordinal == selected && !fc::read_field<std::uint8_t>(row, 0x1C)) {
            result = read(fc::read_field<std::uint64_t>(row, 0x14));
        }
    });
    return result;
}
} // namespace fc_plugins::multiplayer_gc
