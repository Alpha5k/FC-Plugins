#pragma once

#include "protocol.hpp"

#include <fc/native.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fc_plugins::multiplayer_gc {

// Advances RemoveSession's iterator before native code can delete its separately retained current row.
inline void advance_result_iterator(void *frame) noexcept {
    const auto *node = fc::read_field<const void *>(frame, -0xC);
    fc::write_field<const void *>(frame, -0xC, fc::read_field<const void *>(node, 4));
}

// Visits valid native rows in sessionIdx order, including rows hidden by the browser's filters.
template <class Visit> void visit_browser_rows(void *browser, Visit visit) {
    const auto count = fc::read_field<std::int32_t>(browser, 0x1C);
    if (count < 0 || count > 2000) {
        return;
    }
    auto *node = fc::read_field<void *>(browser, 0x10);
    std::int32_t ordinal{};
    for (int visited = 0; node && visited < count; ++visited) {
        auto *row = fc::read_field<void *>(node, 0xC);
        if (!row) {
            break;
        }
        if (fc::read_field<std::uint8_t>(row, 0x10)) {
            visit(row, ordinal++);
        }
        node = fc::read_field<void *>(node, 4);
    }
}

// Lets retail copy type/password/version/ping for a qualified GC row whose advertised mission is empty.
template <class Lookup> std::uint32_t gc_map_condition(std::uint32_t condition, bool selected, const void *row, Lookup lookup) {
    if (!condition && selected && !fc::read_field<std::uint8_t>(row, 0x1C) && lookup(fc::read_field<std::uint64_t>(row, 0x14))) {
        return 1;
    }
    return condition;
}

struct BrowserFilters {
    std::string map, mode, era;
};

struct BrowserSortState {
    std::uint32_t mode{2};
    std::uint8_t direction{1};
};

// Saves both native sort fields when crossing the ordinary/GC source boundary; repeated selection changes neither field.
inline std::optional<BrowserSortState> browser_sort_transition(bool previous, bool active, BrowserSortState current,
                                                               std::array<BrowserSortState, 2> &sources) noexcept {
    if (previous == active) {
        return std::nullopt;
    }
    sources[previous ? 1 : 0] = current;
    return sources[active ? 1 : 0];
}

// Adds metadata predicates to stock name/count/ping flags, then writes contiguous native sort indices.
// The returned ordinal is the first visible row; callers use the original selection callback when reset was requested.
template <class Lookup>
std::optional<std::int32_t> filter_browser_rows(void *browser, const BrowserFilters &filters, std::uint32_t sort, bool ascending,
                                                Lookup lookup) {
    struct Row {
        void *native;
        std::int32_t ordinal;
        SessionInfo info;
    };

    std::vector<Row> rows;
    visit_browser_rows(browser, [&](void *row, std::int32_t ordinal) {
        if (fc::read_field<std::uint8_t>(row, 0xD7)) {
            return;
        }
        auto info = fc::read_field<std::uint8_t>(row, 0x1C) ? std::nullopt : lookup(fc::read_field<std::uint64_t>(row, 0x14));
        const bool visible = info && fc::read_field<std::int32_t>(row, 0xD0) > 0 && (filters.map.empty() || filters.map == info->map) &&
                             (filters.mode.empty() || filters.mode == (info->cooperative ? "Co-op" : "Versus")) &&
                             (filters.era.empty() || filters.era == info->era);
        fc::write_field<std::uint8_t>(row, 0xD7, !visible);
        if (visible) {
            rows.push_back({row, ordinal, std::move(*info)});
        }
    });
    std::stable_sort(rows.begin(), rows.end(), [sort, ascending](const Row &left, const Row &right) {
        const auto priority_left = fc::read_field<std::uint8_t>(left.native, 0xF0) != 0;
        const auto priority_right = fc::read_field<std::uint8_t>(right.native, 0xF0) != 0;
        if (priority_left != priority_right) {
            return priority_left;
        }
        int compared{};
        if (sort == 5) {
            compared = left.info.map.compare(right.info.map);
        } else if (sort == 6) {
            compared = left.info.era.compare(right.info.era);
        } else if (sort == 7) {
            compared = std::string_view(left.info.cooperative ? "Co-op" : "Versus").compare(right.info.cooperative ? "Co-op" : "Versus");
        }
        if (compared) {
            return ascending ? compared < 0 : compared > 0;
        }
        return fc::read_field<std::uint16_t>(left.native, 0xD4) < fc::read_field<std::uint16_t>(right.native, 0xD4);
    });
    for (std::size_t index = 0; index < rows.size(); ++index) {
        fc::write_field<std::uint16_t>(rows[index].native, 0xD4, static_cast<std::uint16_t>(index));
    }
    fc::write_field<std::int32_t>(browser, 8, static_cast<std::int32_t>(rows.size()));
    return rows.empty() ? std::nullopt : std::optional{rows.front().ordinal};
}

using ResortSessions = fc::NativeCall<void(std::uint8_t) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using SelectSession =
    fc::NativeCall<void(void *, std::int32_t) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

using GalaxyInstance = void *(__cdecl *)() noexcept;
using GalaxyGetter = fc::NativeCall<void *(void *) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using GalaxyValue =
    fc::NativeCall<void(void *, std::uint64_t, const char *, char *, std::uint32_t) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<8>, fc::abi::Stack<12>, fc::abi::Stack<16>>,
                                fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

// Reads only the cached named GC Lobby item while the native GC browser is explicitly selected.
class Discovery {
  public:
    // Qualifies retail result/selection and the existing Galaxy facade's cached-value calls.
    void plan(fc::Plan &input);

    // Selects GC result filtering and restores that source's native sort pair on the invoking Lua thread.
    void select(bool active) noexcept;

    // Reads a zero-based native session ordinal, or the selected row when omitted, on the native/Lua owner thread.
    std::optional<SessionInfo> session_info(std::optional<std::int32_t> index = std::nullopt) const noexcept;

    // Copies GC-only filter values on the Lua/native owning thread before stock ApplyFilters triggers resorting.
    void filters(BrowserFilters values) {
        filters_ = std::move(values);
    }

  private:
    // Reads the cached version-three FC1 participant count, host limit and campaign summary.
    std::optional<SessionInfo> read(std::uint64_t lobby) const noexcept;

    std::atomic_bool selected_{};
    fc::NativeFunction<GalaxyInstance> instance_;
    fc::NativeFunction<GalaxyGetter> matchmaking_;
    fc::NativeFunction<GalaxyValue> value_;
    fc::Memory<const std::byte> factory_table_, matchmaking_table_, browser_table_;
    fc::Memory<void *const> browser_;
    BrowserFilters filters_;
    fc::NativeFunction<ResortSessions> resort_;
    fc::NativeFunction<SelectSession> select_session_;
    fc::Memory<std::uint32_t> sort_;
    fc::Memory<std::uint8_t> direction_;
    std::array<BrowserSortState, 2> source_sorts_{};
};
} // namespace fc_plugins::multiplayer_gc
