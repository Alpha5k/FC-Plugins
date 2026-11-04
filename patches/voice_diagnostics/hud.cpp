#include "handler.hpp"

#include <algorithm>
#include <format>

namespace fc_plugins::voice_diagnostics {

thread_local Handler::StatusInvocation *Handler::current_status_{};
thread_local Handler::HudInvocation *Handler::current_hud_{};
thread_local Handler::DrawInvocation *Handler::current_draw_{};

namespace {

// VoiceChatGroup embeds four rows, each with an ID followed by a native text element.
constexpr std::size_t kRows = 4;
constexpr std::size_t kRowOffset = 0xB0;
constexpr std::size_t kRowStride = 0xD0;
constexpr std::size_t kTextOffset = 0x10;

// Native player IDs fit the FC state table; the record retains the full native count separately.
std::uint64_t player_bit(std::uint8_t player) noexcept {
    return player < 64 ? std::uint64_t{1} << player : 0;
}

} // namespace

void Handler::plan_hud(fc::Plan &plan) {
    const auto layout = select_hud_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    for (std::size_t index = 0; index < hud_memory_.size(); ++index) {
        const auto field = static_cast<HudGlobal>(index);
        const auto bytes = field == HudGlobal::ForcedHide || field == HudGlobal::RenderSuppressed || field == HudGlobal::InterfaceEnabled ||
                                   field == HudGlobal::ViewportFlags
                               ? 1
                               : 4;
        hud_memory_[index] = plan.access<const std::byte>(layout->globals[index], bytes);
    }

    plan.observe(layout->create, fc::after([this](void *group, void *status, void *result) noexcept {
                     hud_generation_.fetch_add(1, std::memory_order_relaxed);
                     constructors_.fetch_add(1, std::memory_order_relaxed);
                     trace::HudCreatedRecord record{.header = header(trace::RecordKind::HudCreated),
                                                    .group = pointer_id(group),
                                                    .status = pointer_id(status),
                                                    .result = pointer_id(result)};
                     write(record);
                 }));

    // Parent observations include early returns that prevent VoiceChatGroup::Update from running.
    plan.observe<StatusInvocation>(
        layout->status_update, fc::before([this](void *status, float delta, StatusInvocation &scope) noexcept {
            scope.previous = current_status_;
            current_status_ = &scope;
            status_updates_.fetch_add(1, std::memory_order_relaxed);
            scope.record = {.header = {.kind = trace::RecordKind::HudStatus},
                            .status = pointer_id(status),
                            .delta = delta,
                            .frame = next_hud_frame_.fetch_add(1, std::memory_order_relaxed)};
        }),
        fc::after([this](void *status, float, bool, const StatusInvocation &scope) noexcept {
            current_status_ = scope.previous;
            if (!trace_.enabled()) {
                return;
            }
            auto record = scope.record;
            const auto *characters = hud_global<const std::byte *>(HudGlobal::Characters);
            record.character_array = pointer_id(characters);
            record.character_count = hud_global<std::uint32_t>(HudGlobal::CharacterCount);
            record.cached_character = fc::read_field<std::uint32_t>(status, 0x1114);
            record.selected_slot = record.spectated_player != -1 ? record.spectated_player : record.local_player;
            if (characters && record.selected_slot >= 0 && static_cast<std::uint32_t>(record.selected_slot) < record.character_count) {
                record.stored_player = fc::read_field<std::int32_t>(characters, record.selected_slot * 0x1B0 + 0x130);
            }
            record.shown = fc::read_field<std::uint8_t>(status, 0x1118);
            record.viewport_enabled = fc::read_field<std::uint8_t>(status, 0x20);
            record.forced_hide = hud_global<std::uint8_t>(HudGlobal::ForcedHide);
            record.render_suppressed = hud_global<std::uint8_t>(HudGlobal::RenderSuppressed);
            record.interface_enabled = hud_global<std::uint8_t>(HudGlobal::InterfaceEnabled);
            write_hud(record);
        }));

    plan.observe_call(layout->status_local, fc::after([](std::int32_t, std::int32_t player) noexcept {
                          if (current_status_) {
                              current_status_->record.local_player = player;
                          }
                      }));
    plan.observe_call(layout->status_spectated, fc::after([](std::int32_t, std::int32_t player) noexcept {
                          if (current_status_) {
                              current_status_->record.spectated_player = player;
                          }
                      }));
    plan.observe_call(layout->menu_hidden, fc::after([](bool hidden) noexcept {
                          if (current_status_) {
                              current_status_->record.menu_hidden = hidden;
                          }
                      }));
    plan.observe_instruction(layout->character, fc::before([](const fc::CpuContext &cpu) noexcept {
                                 if (current_status_) {
                                     const auto *character = reinterpret_cast<const void *>(cpu.edi);
                                     current_status_->record.character = pointer_id(character);
                                     current_status_->record.actor = fc::read_field<std::uint32_t>(character, 0x148);
                                 }
                             }));

    plan.observe(layout->bind, fc::after([this](void *group, std::int32_t viewport) noexcept {
                     hud_generation_.fetch_add(1, std::memory_order_relaxed);
                     trace::HudCreatedRecord record{.header = header(trace::RecordKind::HudCreated),
                                                    .group = pointer_id(group),
                                                    .status = fc::read_field<std::uint32_t>(group, 0xA0),
                                                    .viewport = viewport,
                                                    .phase = 1};
                     write(record);
                     binds_.fetch_add(1, std::memory_order_relaxed);
                     for (std::uint8_t row = 0; row < kRows; ++row) {
                         observe_row(group, row, 0);
                     }
                 }));

    plan.observe<HudInvocation>(
        layout->update, fc::before([this](void *group, HudInvocation &scope) noexcept {
            scope.previous = current_hud_;
            current_hud_ = &scope;
            hud_updates_.fetch_add(1, std::memory_order_relaxed);
            const auto frame = current_status_ ? current_status_->record.frame : next_hud_frame_.fetch_add(1, std::memory_order_relaxed);
            scope.record = {.header = {.kind = trace::RecordKind::HudScan}, .group = pointer_id(group), .frame = frame};
            if (current_status_) {
                current_status_->record.hud_updated = 1;
            }
        }),
        fc::after([this](void *group, const HudInvocation &scope) noexcept {
            auto record = scope.record;
            write_hud(record);
            for (std::uint8_t row = 0; row < kRows; ++row) {
                observe_row(group, row, 2);
            }
            current_hud_ = scope.previous;
        }));

    plan.observe_call(layout->node_count, fc::after([this](void *, std::uint8_t count) noexcept {
                          if (!current_hud_) {
                              return;
                          }
                          auto &record = current_hud_->record;
                          record.node_count = count;
                          const auto *characters = hud_global<const std::byte *>(HudGlobal::Characters);
                          const auto characters_count = hud_global<std::uint32_t>(HudGlobal::CharacterCount);
                          record.character_count = characters_count;
                          if (characters) {
                              for (std::uint32_t player = 0; player < std::min<std::uint32_t>({count, characters_count, 64}); ++player) {
                                  if (fc::read_field<std::int32_t>(characters, player * 0x1B0 + 0x130) == static_cast<int>(player)) {
                                      record.valid_characters |= player_bit(static_cast<std::uint8_t>(player));
                                  }
                              }
                          }
                      }));
    plan.observe_call(layout->hud_local, fc::after([](std::int32_t, std::int32_t player) noexcept {
                          if (current_hud_) {
                              current_hud_->record.local_player = player;
                          }
                      }));
    plan.observe_call(layout->sending, fc::after([](void *, std::uint8_t player, bool result) noexcept {
                          if (!current_hud_) {
                              return;
                          }
                          current_hud_->record.sending_tested |= player_bit(player);
                          if (result) {
                              current_hud_->record.sending |= player_bit(player);
                          }
                      }));
    plan.observe_call(layout->permission, fc::after([](void *, std::uint8_t, std::uint8_t player, bool result) noexcept {
                          if (!current_hud_) {
                              return;
                          }
                          current_hud_->record.permission_tested |= player_bit(player);
                          if (result) {
                              current_hud_->record.permitted |= player_bit(player);
                          }
                      }));
    plan.observe(layout->add_players, fc::after([this](void *group) noexcept {
                     for (std::uint8_t row = 0; row < kRows; ++row) {
                         observe_row(group, row, 1);
                     }
                 }));

    plan.observe_instruction(layout->timestamps, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 if (!trace_.enabled() || !current_hud_) {
                                     return;
                                 }
                                 const auto group = current_hud_->record.group;
                                 trace::HudTimeRecord record{.header = header(trace::RecordKind::HudTime),
                                                             .group = group,
                                                             .frame = current_hud_->record.frame,
                                                             .player = static_cast<std::int32_t>(cpu.edx),
                                                             .now = fc::read_field<float>(&cpu.xmm[1].low, 0),
                                                             .last_receive = fc::read_field<float>(&cpu.xmm[0].low, 0),
                                                             .row = static_cast<std::uint8_t>((cpu.edi - group - kRowOffset) / kRowStride)};
                                 write(record);
                             }));
    plan.observe(layout->node_update, fc::after([this](void *node) noexcept {
                     if (!trace_.enabled()) {
                         return;
                     }
                     trace::LocalActivityRecord record{.header = {.kind = trace::RecordKind::LocalActivity},
                                                       .node = pointer_id(node),
                                                       .now = hud_global<float>(HudGlobal::Clock),
                                                       .last_send = fc::read_field<float>(node, 0x20),
                                                       .player = fc::read_field<std::uint8_t>(node, 0x3C)};
                     write_hud(record, record.player);
                 }));

    plan.observe_instruction(layout->render_gate, fc::before([this](const fc::CpuContext &) noexcept {
                                 renders_.fetch_add(1, std::memory_order_relaxed);
                                 if (!trace_.enabled()) {
                                     return;
                                 }
                                 trace::UiRenderRecord record{.header = {.kind = trace::RecordKind::UiRender},
                                                              .viewport_count = hud_global<std::uint32_t>(HudGlobal::ViewportCount),
                                                              .suppressed = hud_global<std::uint8_t>(HudGlobal::RenderSuppressed),
                                                              .interface_enabled = hud_global<std::uint8_t>(HudGlobal::InterfaceEnabled)};
                                 write_hud(record, record.viewport == 0xFFFFFFFF ? 0 : record.viewport + 1);
                                 // The native count bounds both arrays. Record each viewport root and its interface flag.
                                 for (std::uint32_t viewport = 0; viewport < record.viewport_count; ++viewport) {
                                     record.header = {.kind = trace::RecordKind::UiRender};
                                     record.viewport = viewport;
                                     record.viewport_enabled = fc::read_field<std::uint8_t>(
                                         hud_data_[static_cast<std::size_t>(HudGlobal::ViewportFlags)], viewport);
                                     record.viewport_root = fc::read_field<std::uint32_t>(
                                         hud_data_[static_cast<std::size_t>(HudGlobal::ViewportRoots)], viewport * sizeof(std::uint32_t));
                                     write_hud(record, record.viewport == 0xFFFFFFFF ? 0 : record.viewport + 1);
                                 }
                             }));

    if (settings_.capture != CaptureMode::Full) {
        return;
    }

    // Only voice-group drawing establishes this scope. Shared text drawing can then match its four direct children.
    plan.observe<DrawInvocation>(
        layout->group_draw, fc::before([this](void *group, const float *matrix, const std::uint8_t *color, DrawInvocation &scope) noexcept {
            scope.previous = current_draw_;
            scope.group = fc::read_field<std::uintptr_t>(group, 0) ==
                                  reinterpret_cast<std::uintptr_t>(hud_data_[static_cast<std::size_t>(HudGlobal::VoiceVtable)])
                              ? group
                              : nullptr;
            current_draw_ = &scope;
            if (scope.group) {
                for (std::uint8_t row = 0; row < kRows; ++row) {
                    observe_row(group, row, 3, matrix, color);
                }
            }
        }),
        fc::after([](void *, const float *, const std::uint8_t *, const DrawInvocation &scope) noexcept {
            current_draw_ = scope.previous;
        }));

    plan.observe(layout->text_draw, fc::before([this](void *text, const float *matrix, const std::uint8_t *color) noexcept {
                     const auto row = drawing_row(text);
                     if (row >= 0) {
                         text_draws_.fetch_add(1, std::memory_order_relaxed);
                         observe_row(current_draw_->group, static_cast<std::uint8_t>(row), 4, matrix, color);
                     }
                 }));
    plan.observe_instruction(layout->glyph_submit, fc::before([this](const fc::CpuContext &cpu) noexcept {
                                 if (!trace_.enabled()) {
                                     return;
                                 }
                                 const auto *text = reinterpret_cast<const void *>(cpu.edi);
                                 if (drawing_row(text) < 0) {
                                     return;
                                 }
                                 submissions_.fetch_add(1, std::memory_order_relaxed);
                                 const auto *stack = reinterpret_cast<const std::byte *>(cpu.esp);
                                 const auto *matrix = fc::read_field<const float *>(stack, 8);
                                 const auto *color = fc::read_field<const std::uint8_t *>(stack, 12);
                                 const auto *counts = fc::read_field<const std::uint32_t *>(text, 0x7C);
                                 trace::HudSubmitRecord record{.header = header(trace::RecordKind::HudSubmit),
                                                               .text = pointer_id(text),
                                                               .vertices = fc::read_field<std::uint32_t>(stack, 0),
                                                               .material = fc::read_field<std::uint32_t>(stack, 4),
                                                               .page = cpu.esi,
                                                               .glyph_count = counts[cpu.esi],
                                                               .color = fc::read_field<std::uint32_t>(color, 0),
                                                               .x = matrix[12],
                                                               .y = matrix[13],
                                                               .scale_x = matrix[0],
                                                               .scale_y = matrix[5],
                                                               .shear_x = matrix[1],
                                                               .shear_y = matrix[4]};
                                 write(record);
                             }));
}

fc::Result<> Handler::prepare_hud() {
    for (std::size_t index = 0; index < hud_memory_.size(); ++index) {
        auto view = hud_memory_[index].view();
        if (!view) {
            return std::unexpected(view.error());
        }
        hud_data_[index] = view->data();
    }
    return {};
}

void Handler::observe_row(void *group, std::uint8_t row, std::uint8_t phase, const float *matrix, const std::uint8_t *color) noexcept {
    if (!trace_.enabled()) {
        return;
    }
    const auto *slot = static_cast<const std::byte *>(group) + kRowOffset + row * kRowStride;
    const auto *text = slot + kTextOffset;
    const auto *string = fc::read_field<const std::uint16_t *>(text, 0xB4);
    const auto *counts = fc::read_field<const std::uint32_t *>(text, 0x7C);
    trace::HudRowRecord record{.header = {.kind = trace::RecordKind::HudRow},
                               .group = pointer_id(group),
                               .text = pointer_id(text),
                               .font = fc::read_field<std::uint32_t>(text, 0x70),
                               .group_owner = fc::read_field<std::uint32_t>(group, 0x1C),
                               .frame = current_hud_ ? current_hud_->record.frame : 0,
                               .flags = fc::read_field<std::uint32_t>(text, 0x14),
                               .color = color ? fc::read_field<std::uint32_t>(color, 0) : fc::read_field<std::uint32_t>(text, 0x2C),
                               .group_flags = fc::read_field<std::uint32_t>(group, 0x14),
                               .group_color = fc::read_field<std::uint32_t>(group, 0x2C),
                               .text_units = fc::read_field<std::uint32_t>(text, 0xB8),
                               .first_page_glyphs = counts ? counts[0] : 0,
                               .player = fc::read_field<std::int32_t>(slot, 0),
                               .x = matrix ? matrix[12] : fc::read_field<float>(text, 0x60),
                               .y = matrix ? matrix[13] : fc::read_field<float>(text, 0x64),
                               .scale_x = fc::read_field<float>(text, 0x98),
                               .scale_y = fc::read_field<float>(text, 0x9C),
                               .transform_x = matrix ? matrix[0] : fc::read_field<float>(text, 0x30),
                               .transform_y = matrix ? matrix[5] : fc::read_field<float>(text, 0x44),
                               .transform_xy = matrix ? matrix[1] : fc::read_field<float>(text, 0x34),
                               .transform_yx = matrix ? matrix[4] : fc::read_field<float>(text, 0x40),
                               .width = fc::read_field<std::uint16_t>(text, 0x92),
                               .height = fc::read_field<std::uint16_t>(text, 0x94),
                               .first_character = string ? string[0] : std::uint16_t{},
                               .line_count = fc::read_field<std::uint16_t>(text, 0xA2),
                               .line_width = fc::read_field<std::uint16_t>(text, 0xA4),
                               .layout_flags = fc::read_field<std::uint16_t>(text, 0xAE),
                               .has_counts = counts != nullptr,
                               .row = row,
                               .phase = phase};
    write_hud(record, row * 5 + phase);
}

int Handler::drawing_row(const void *text) noexcept {
    if (!current_draw_ || !current_draw_->group) {
        return -1;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(current_draw_->group) + kRowOffset + kTextOffset;
    const auto address = reinterpret_cast<std::uintptr_t>(text);
    if (address < first || (address - first) % kRowStride != 0) {
        return -1;
    }
    const auto row = (address - first) / kRowStride;
    return row < kRows ? static_cast<int>(row) : -1;
}

void Handler::observe_ui(const UiObservation *observation) noexcept {
    if (!observation) {
        return;
    }
    if (observation->query == UiQuery::Glyph) {
        glyph_queries_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!trace_.enabled()) {
        return;
    }
    trace::UiQueryRecord record{
        .header = {.kind = trace::RecordKind::UiQuery}, .hud_frame = current_hud_ ? current_hud_->record.frame : 0, .value = *observation};
    write_hud(record, (observation->context ^ static_cast<std::uint64_t>(observation->query) * 64 + observation->player) % 128);
}

void Handler::observe_view(const ViewObservation *observation) noexcept {
    if (!observation) {
        return;
    }
    owner_observed_.store(true, std::memory_order_relaxed);
    views_.fetch_add(1, std::memory_order_relaxed);
    if (!trace_.enabled()) {
        return;
    }
    trace::ViewRecord record{.header = header(trace::RecordKind::View), .value = *observation};
    write(record);
}

void Handler::update(const fc::UpdateContext &) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_health_) {
        return;
    }
    next_health_ = now + std::chrono::seconds{1};
    write_hud(trace::HudRowRecord{}, 128);
    write_hud(trace::HudStatusRecord{}, 128);
    write_hud(trace::HudScanRecord{}, 128);
    write_hud(trace::UiQueryRecord{}, 128);
    write_hud(trace::UiRenderRecord{}, 128);
    write_hud(trace::LocalActivityRecord{}, 128);
    write_hud(trace::HudActivityRecord{}, 128);
    write_hud(trace::NetworkAdmissionRecord{}, 128);
    write_hud(trace::StreamQueueRecord{}, 128);
    write_hud(trace::CodecOutcomeRecord{}, 128);
    const auto health = trace_.health();
    trace::UiHealthRecord record{.header = header(trace::RecordKind::UiHealth),
                                 .status_updates = status_updates_.load(std::memory_order_relaxed),
                                 .hud_updates = hud_updates_.load(std::memory_order_relaxed),
                                 .binds = binds_.load(std::memory_order_relaxed),
                                 .renders = renders_.load(std::memory_order_relaxed),
                                 .text_draws = text_draws_.load(std::memory_order_relaxed),
                                 .submissions = submissions_.load(std::memory_order_relaxed),
                                 .glyph_queries = glyph_queries_.load(std::memory_order_relaxed),
                                 .views = views_.load(std::memory_order_relaxed),
                                 .constructors = constructors_.load(std::memory_order_relaxed),
                                 .write_failures = write_failures_.load(std::memory_order_relaxed),
                                 .accepted = health.accepted,
                                 .written = health.written,
                                 .dropped = health.dropped,
                                 .output_failed = health.output_failed,
                                 .file_limit_reached = health.file_limit_reached,
                                 .capture = static_cast<std::uint8_t>(settings_.capture),
                                 .role = static_cast<std::uint8_t>(role_),
                                 .owner_observed = owner_observed_.load(std::memory_order_relaxed),
                                 .suppressed_hud = suppressed_hud_.load(std::memory_order_relaxed)};
    write(record);
}

void Handler::write_status(fc::StatusContext &context) const noexcept {
    const auto health = trace_.health();
    context.line("Records: {}; dropped: {}", health.written, health.dropped);
}

} // namespace fc_plugins::voice_diagnostics
