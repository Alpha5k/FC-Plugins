#pragma once

#include "api.hpp"
#include "schema.hpp"

namespace fc_plugins::voice_diagnostics::trace {

// Periodic counters distinguish a silent producer from a missing or rejected trace record.
struct UiHealthRecord {
    RecordHeader header{};
    std::uint32_t status_updates{}, hud_updates{}, binds{}, renders{}, text_draws{}, submissions{}, glyph_queries{}, views{},
        constructors{};
    std::uint64_t write_failures{}, accepted{}, written{}, dropped{};
    std::uint8_t output_failed{}, file_limit_reached{}, capture{}, role{}, owner_observed{};
    std::uint64_t suppressed_hud{};
};

struct UiQueryRecord {
    RecordHeader header{};
    std::uint32_t hud_frame{}; // Zero outside the gameplay HUD.
    UiObservation value{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const UiQueryRecord &) const = default;
};

struct ViewRecord {
    RecordHeader header{};
    ViewObservation value{};
};

// The native parent can suppress the voice group before its update runs.
struct HudStatusRecord {
    RecordHeader header{};
    std::uint64_t status{}, character{}, actor{}, character_array{}, cached_character{};
    std::uint32_t character_count{};
    std::int32_t selected_slot{-1}, stored_player{-1};
    std::int32_t local_player{-1}, spectated_player{-1};
    float delta{};
    std::uint32_t frame{};
    std::uint8_t shown{}, viewport_enabled{}, forced_hide{}, menu_hidden{2}, hud_updated{};
    std::uint8_t render_suppressed{}, interface_enabled{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const HudStatusRecord &) const = default;
};

// Masks describe actual calls reached during AddPlayers, including false answers.
struct HudScanRecord {
    RecordHeader header{};
    std::uint64_t group{};
    std::uint64_t valid_characters{}, sending_tested{}, sending{}, permission_tested{}, permitted{};
    std::uint32_t frame{}, node_count{}, character_count{};
    std::int32_t local_player{-1};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const HudScanRecord &) const = default;
};

// Captures every row after binding, selection, fading and at actual drawing boundaries.
struct HudRowRecord {
    RecordHeader header{};
    std::uint64_t group{}, text{}, font{}, group_owner{};
    std::uint32_t frame{}, flags{}, color{}, group_flags{}, group_color{}, text_units{}, first_page_glyphs{};
    std::int32_t player{-1};
    float x{}, y{}, scale_x{}, scale_y{}, transform_x{}, transform_y{}, transform_xy{}, transform_yx{};
    std::uint16_t width{}, height{}, first_character{}, line_count{}, line_width{};
    std::uint16_t layout_flags{};
    std::uint8_t has_counts{}, row{}, phase{}; // 0 bind, 1 selected, 2 faded, 3 group draw, 4 text entry.
    std::uint64_t first_ms{}, last_ms{};       // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const HudRowRecord &) const = default;
};

// Copies the exact native clock values immediately before UpdateLines subtracts them.
struct HudTimeRecord {
    RecordHeader header{};
    std::uint64_t group{};
    std::uint32_t frame{};
    std::int32_t player{};
    float now{}, last_receive{};
    std::uint8_t row{};
};

struct LocalActivityRecord {
    RecordHeader header{};
    std::uint64_t node{};
    float now{}, last_send{};
    std::uint8_t player{};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const LocalActivityRecord &) const = default;
};

// A submission is recorded only after native glyph vertices were prepared for this font page.
struct HudSubmitRecord {
    RecordHeader header{};
    std::uint64_t text{}, vertices{}, material{};
    std::uint32_t page{}, glyph_count{}, color{};
    float x{}, y{}, scale_x{}, scale_y{}, shear_x{}, shear_y{};
};

// Captures the native render gate even when all interface drawing is suppressed.
struct UiRenderRecord {
    RecordHeader header{};
    std::uint64_t viewport_root{};
    std::uint32_t viewport_count{}, viewport{0xFFFFFFFF};
    std::uint8_t suppressed{}, interface_enabled{}, viewport_enabled{2};
    std::uint64_t first_ms{}, last_ms{}; // GetTickCount64 endpoints; interior callback times are omitted.
    std::uint32_t observations{1};
    float last_clock{};
    bool operator==(const UiRenderRecord &) const = default;
};

struct HudCreatedRecord {
    RecordHeader header{};
    std::uint64_t group{}, status{}, result{};
    std::int32_t viewport{-1};
    std::uint8_t phase{}; // 0 constructed, 1 bound.
};

static_assert(kValidRecord<UiHealthRecord>);
static_assert(kValidRecord<UiQueryRecord>);
static_assert(kValidRecord<ViewRecord>);
static_assert(kValidRecord<HudStatusRecord>);
static_assert(kValidRecord<HudScanRecord>);
static_assert(kValidRecord<HudRowRecord>);
static_assert(kValidRecord<HudTimeRecord>);
static_assert(kValidRecord<LocalActivityRecord>);
static_assert(kValidRecord<HudSubmitRecord>);
static_assert(kValidRecord<UiRenderRecord>);
static_assert(kValidRecord<HudCreatedRecord>);

} // namespace fc_plugins::voice_diagnostics::trace
