#include "handler.hpp"
#include "pipeline_layout.hpp"

#include <bit>

namespace fc_plugins::network_diagnostics {

void Handler::plan_replication(fc::Plan &plan) {
    const auto locations = select_pipeline_layout(plan.profile());
    if (role_ == fc::Role::Client) {
        const auto client = *select_client_layout(plan.profile());
        plan.observe<EventInvocation>(client.read_net_event, fc::before([this](void *packet, EventInvocation &invocation) noexcept {
                                          auto &state = thread_state();
                                          invocation.previous = state.decoded_event;
                                          invocation.record = {
                                              .source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                              .destination = -1,
                                              .outcome = 20,
                                              .bits_before = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX,
                                              .packet = native::pointer_id(packet)};
                                          if (state.read_update != nullptr) {
                                              invocation.record.ordinal = ++state.read_update->ordinary_ordinal;
                                          }
                                          state.decoded_event = &invocation;
                                      }),
                                      fc::after([this](void *, const EventInvocation &invocation) noexcept {
                                          auto &state = thread_state();
                                          auto record = invocation.record;
                                          record.bits_after = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX;
                                          state.decoded_event = invocation.previous;
                                          write(trace::RecordKind::ReplicationEvent, record, record.packet,
                                                settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial);
                                      }));
        plan.observe_instruction(locations.ordinary_type, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().decoded_event; invocation != nullptr) {
                                         invocation->record.type = native::read<std::uint32_t>(context.ebp, -0x5C);
                                         invocation->record.outcome = invocation->record.type == 0 ? 21U : 20U;
                                     }
                                 }));
        const auto gate = [this, &plan](fc::CodeLocation site, std::uint32_t bit) {
            plan.observe_instruction(site, fc::before([this, bit](const fc::CpuContext &context) noexcept {
                                         observe_event_gate(bit, context);
                                     }));
        };
        gate(locations.ordinary_owner_gate, 0);
        gate(locations.ordinary_local_gate, 1);
        gate(locations.ordinary_weapon_gate, 2);
        gate(locations.ordinary_duplicate_gate, 3);
        plan.observe_instruction(locations.ordinary_weapon_result, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().decoded_event; invocation != nullptr) {
                                         invocation->record.related_object = native::read<std::uint32_t>(context.ebp, -0x74);
                                         invocation->record.gate_visited |= 1U << 4;
                                         invocation->record.gate_results |=
                                             static_cast<std::uint32_t>(invocation->record.related_object != 0) << 4;
                                     }
                                 }));
        plan.observe_instruction(locations.ordinary_class_gate, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().decoded_event; invocation != nullptr) {
                                         invocation->record.related_class = native::read<std::uint32_t>(context.ebp, -0x1C);
                                         invocation->record.outcome = invocation->record.related_class != 0 ? 23U : 22U;
                                     }
                                 }));
        plan.observe_instruction(locations.ordinary_dispatch_finish, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().decoded_event;
                                         invocation != nullptr && invocation->record.outcome == 23) {
                                         invocation->record.outcome = 24;
                                         invocation->record.built_object = context.eax;
                                     }
                                 }));

        // A decoded reliable row is emitted after its native action dispatch or duplicate-suppression branch completes.
        plan.observe_instruction(locations.reliable_wire_type, fc::before([this](const fc::CpuContext &context) noexcept {
                                     auto &state = thread_state();
                                     state.reliable_event = {
                                         .event = context.ebp - 0x58,
                                         .sequence = native::read<std::uint32_t>(context.ebp, -8),
                                         .type = native::read<std::uint32_t>(context.ebp, -0x18),
                                         .source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                         .destination = -1,
                                         .outcome = 30,
                                         .bits_before = state.read_update != nullptr && state.read_update->bits != UINT32_MAX
                                                            ? state.read_update->bits - 4
                                                            : UINT32_MAX,
                                         .packet = native::read<std::uint32_t>(context.ebx, 8),
                                         .ordinal = native::read<std::uint32_t>(context.ebp, -0x0C)};
                                 }));
        plan.observe_instruction(locations.reliable_decode_end, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &record = thread_state().reliable_event;
                                     record.gate_visited = 1;
                                     record.gate_results = native::read<std::uint8_t>(context.ebp, -1);
                                     record.outcome = record.type > 13 ? 33U : record.gate_results != 0 ? 31U : 32U;
                                 }));
        plan.observe_instruction(locations.reliable_action_end, fc::before([this](const fc::CpuContext &) noexcept {
                                     auto &state = thread_state();
                                     auto record = state.reliable_event;
                                     record.bits_after = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX;
                                     write(trace::RecordKind::ReplicationEvent, record, record.packet,
                                           settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial);
                                 }));
        // Preserve the actual handle result before native code clears a stale owner reference.
        plan.observe_instruction(locations.reliable_projectile_handle, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &record = thread_state().reliable_event;
                                     const auto reference = native::read<std::uint32_t>(context.ebp, -0x14);
                                     record.target_object = native::read<std::uint32_t>(reference);
                                     record.target_lifetime = context.eax;
                                     record.expected_lifetime = native::read<std::uint32_t>(reference, 4);
                                     record.gate_visited |= 1U << 5;
                                     record.gate_results |= static_cast<std::uint32_t>(record.target_lifetime == record.expected_lifetime)
                                                            << 5;
                                 }));
        plan.observe_instruction(locations.reliable_projectile_admission, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &record = thread_state().reliable_event;
                                     const auto descriptor = native::read<std::uint32_t>(context.ebp, -0x10);
                                     const auto owner = native::read<std::uint32_t>(context.ebp, -0x18);
                                     record.related_object = native::read<std::uint32_t>(descriptor, 0x14);
                                     if ((record.gate_visited & (1U << 5)) == 0) {
                                         record.target_object = owner;
                                     }
                                     record.gate_visited |= 7U << 6;
                                     record.gate_results |= static_cast<std::uint32_t>(native::read<std::uint32_t>(descriptor, 0x18) != 0)
                                                            << 6;
                                     record.gate_results |= static_cast<std::uint32_t>(record.related_object != 0) << 7;
                                     record.gate_results |= static_cast<std::uint32_t>(owner != 0) << 8;
                                     record.outcome = 35;
                                 }));
        plan.observe_instruction(locations.reliable_projectile_build, fc::before([](const fc::CpuContext &context) noexcept {
                                     auto &record = thread_state().reliable_event;
                                     record.related_class = native::read<std::uint32_t>(context.ebp, -0x34);
                                     record.built_object = context.eax;
                                     record.gate_visited |= 1U << 9;
                                     record.gate_results |= static_cast<std::uint32_t>(context.eax != 0) << 9;
                                     record.outcome = 36;
                                 }));
        plan.observe_instruction(locations.reliable_projectile_complete, fc::before([](const fc::CpuContext &) noexcept {
                                     thread_state().reliable_event.outcome = 34;
                                 }));
        if (settings_.capture != CaptureMode::Full) {
            return;
        }
        // These callbacks follow the actual virtual object decoder, retention callback, and native removal.
        plan.observe<ObjectInvocation>(
            locations.read_create, fc::before([](void *packet, std::uint32_t wire_id, ObjectInvocation &invocation) noexcept {
                auto &state = thread_state();
                invocation.previous = state.object_reader;
                invocation.record = {.source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                     .outcome = 20,
                                     .packet = native::pointer_id(packet),
                                     .bits_before = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX,
                                     .wire_id = wire_id};
                state.object_reader = &invocation;
            }),
            fc::after([this](void *, std::uint32_t, void *object, const ObjectInvocation &invocation) noexcept {
                auto &state = thread_state();
                auto record = invocation.record;
                record.object = native::pointer_id(object);
                record.native_id = native::read<std::uint32_t>(object, 0x204);
                record.bits_after = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX;
                state.object_reader = invocation.previous;
                write(trace::RecordKind::ReplicationObject, record, record.object, trace::End);
            }));
        plan.observe_instruction(locations.read_create_finish, fc::before([](const fc::CpuContext &context) noexcept {
                                     if (auto *invocation = thread_state().object_reader; invocation != nullptr) {
                                         invocation->record.native_class = native::read<std::uint32_t>(context.ebp, -8);
                                     }
                                 }));
        plan.observe<ObjectInvocation>(locations.read_delta, fc::before([](void *packet, ObjectInvocation &invocation) noexcept {
                                           auto &state = thread_state();
                                           invocation.previous = state.object_reader;
                                           invocation.record = {
                                               .source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                               .packet = native::pointer_id(packet),
                                               .bits_before = state.read_update != nullptr ? state.read_update->bits : 0};
                                           state.object_reader = &invocation;
                                       }),
                                       fc::after([this](void *, bool result, const ObjectInvocation &invocation) noexcept {
                                           auto &state = thread_state();
                                           auto record = invocation.record;
                                           record.bits_after = state.read_update != nullptr ? state.read_update->bits : UINT32_MAX;
                                           state.object_reader = invocation.previous;
                                           if (result) {
                                               write(trace::RecordKind::ReplicationObject, record, record.object, trace::End);
                                           }
                                       }));
        plan.observe_instruction(locations.read_delta_special_finish, fc::before([this](const fc::CpuContext &context) noexcept {
                                     observe_read_object(context, true);
                                 }));
        plan.observe_instruction(locations.read_delta_normal_finish, fc::before([this](const fc::CpuContext &context) noexcept {
                                     observe_read_object(context, false);
                                 }));
        plan.observe_instruction(locations.read_retained_finish, fc::before([this](const fc::CpuContext &context) noexcept {
                                     const auto &state = thread_state();
                                     const auto object = native::read<std::uint32_t>(context.ebp, -0x3C);
                                     const auto net_object = native::read<std::uint32_t>(context.ebp, -0x30);
                                     const trace::ReplicationObjectRecord record{
                                         .object = object,
                                         .native_id = native::read<std::uint32_t>(object, 0x204),
                                         .source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                         .outcome = 23,
                                         .packet = native::read<std::uint32_t>(context.ebp, 8),
                                         .last_turn = native::read<std::uint32_t>(net_object, 8),
                                         .wire_id = native::read<std::uint32_t>(context.ebp, -0x10),
                                         .map = native::read<std::uint32_t>(context.ebp, -0x1C)};
                                     write(trace::RecordKind::ReplicationObject, record, object, trace::End);
                                 }));
        plan.observe<ObjectInvocation>(
            locations.remove_object, fc::before([](void *net_object, bool destroy, ObjectInvocation &invocation) noexcept {
                const auto object = native::read<std::uint32_t>(net_object);
                const auto &state = thread_state();
                invocation.record = {.object = object,
                                     .native_id = native::read<std::uint32_t>(object, 0x204),
                                     .source_turn = state.read_update != nullptr ? state.read_update->record.advertised_turn : 0,
                                     .outcome = 24,
                                     .packet = state.read_update != nullptr ? state.read_update->record.group : 0,
                                     .last_turn = native::read<std::uint32_t>(net_object, 8),
                                     .priority = destroy,
                                     .wire_id = native::read<std::uint32_t>(net_object, 4)};
            }),
            fc::after([this](void *, bool, const ObjectInvocation &invocation) noexcept {
                write(trace::RecordKind::ReplicationObject, invocation.record, invocation.record.object, trace::End);
            }));
        return;
    }

    ordinary_ring_memory_ = plan.access(locations.ordinary_ring);
    reliable_ring_memory_ = plan.access(locations.reliable_ring);
    reliable_head_memory_ = plan.access(locations.reliable_head);
    reliable_free_memory_ = plan.access(locations.reliable_free_cursor);
    if (settings_.capture == CaptureMode::Full) {
        object_list_memory_ = plan.access(locations.object_list);
        object_count_memory_ = plan.access(locations.object_count);
    }
    const auto server = *select_server_layout(plan.profile());

    if (settings_.capture == CaptureMode::Full) {
        // Full observes the selected list while its WriteObjects stack storage remains alive.
        plan.observe(server.write_objects, fc::before([](void *) noexcept {
                         thread_state().selected_objects = nullptr;
                         thread_state().selected_object_count = 0;
                     }),
                     fc::after([](void *) noexcept {
                         thread_state().selected_objects = nullptr;
                         thread_state().selected_object_count = 0;
                     }));
    }

    plan.observe(server.send_events, fc::before([this](void *packet) noexcept {
                     auto &state = thread_state();
                     const auto destination = *current_destination_;
                     const auto cursor = native::read<std::uint32_t>(player_states_ + destination * native::kPlayerStateStride, 0x38);
                     state.ordinary_first = cursor;
                     state.ordinary_head = *event_head_;
                     state.ordinary_active = true;
                     state.ordinary_ordinal = 0;
                     for (auto index = cursor; index != state.ordinary_head; index = (index + 1) & 127U) {
                         const auto *event = ordinary_ring_ + index * 80;
                         state.ordinary_events[index] = {.event = native::pointer_id(event),
                                                         .type = native::read<std::uint32_t>(event),
                                                         .source_turn = state.ordinary_created_turns[index],
                                                         .destination = destination,
                                                         .outcome = 5,
                                                         .cursor = index,
                                                         .head = state.ordinary_head,
                                                         .packet = native::pointer_id(packet),
                                                         .generation = state.ordinary_generations[index]};
                     }
                 }),
                 fc::after([this](void *) noexcept {
                     auto &state = thread_state();
                     for (auto index = state.ordinary_first; index != state.ordinary_head; index = (index + 1) & 127U) {
                         auto &record = state.ordinary_events[index];
                         if (record.outcome == 6) {
                             record.outcome = 4;
                         }
                         write(trace::RecordKind::ReplicationEvent, record, record.event,
                               settings_.capture == CaptureMode::Full ? trace::None : trace::Partial);
                     }
                     state.ordinary_active = false;
                 }));
    plan.observe_instruction(locations.event_relevance, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 if (state.ordinary_active) {
                                     auto &record = state.ordinary_events[native::read<std::uint32_t>(context.ebp, -0x10)];
                                     record.relevance = native::read<float>(context.ebp, -0x1C);
                                     record.outcome = record.relevance >= 0.0F ? 6U : 3U;
                                 }
                             }));
    plan.observe(locations.event_writer, fc::before([this](void *event, void *) noexcept {
                     auto &state = thread_state();
                     const auto index = static_cast<std::uint32_t>((static_cast<std::byte *>(event) - ordinary_ring_) / 80);
                     auto &record = state.ordinary_events[index];
                     record.bits_before = state.destination != nullptr ? state.destination->bits : UINT32_MAX;
                     record.ordinal = ++state.ordinary_ordinal;
                 }),
                 fc::after([this](void *event, void *) noexcept {
                     auto &state = thread_state();
                     const auto index = static_cast<std::uint32_t>((static_cast<std::byte *>(event) - ordinary_ring_) / 80);
                     auto &record = state.ordinary_events[index];
                     record.bits_after = state.destination != nullptr ? state.destination->bits : UINT32_MAX;
                     record.outcome = record.type == 0 ? 2U : 7U;
                 }));

    plan.observe(locations.reliable_allocate, fc::after([this](void *event) noexcept {
                     auto &state = thread_state();
                     const auto index = static_cast<std::uint32_t>((static_cast<std::byte *>(event) - reliable_ring_) / 64);
                     ++state.reliable_generations[index];
                     state.reliable_created_turns[index] = static_cast<std::uint32_t>(*host_turn_);
                     const trace::ReplicationEventRecord record{.event = native::pointer_id(event),
                                                                .type = 0xFFFFFFFFU,
                                                                .source_turn = state.reliable_created_turns[index],
                                                                .outcome = 10,
                                                                .cursor = index,
                                                                .head = *reliable_head_,
                                                                .generation = state.reliable_generations[index],
                                                                .recipients = native::read<std::uint64_t>(event, 8)};
                     write(trace::RecordKind::ReplicationEvent, record, record.event,
                           settings_.capture == CaptureMode::Full ? trace::Begin : trace::Begin | trace::Partial);
                 }));
    plan.observe_instruction(locations.reliable_entry, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto index = native::read<std::uint32_t>(context.ebp, -0x10);
                                 const auto event = native::read<std::uint32_t>(context.ebp, -8);
                                 const auto destination = *current_destination_;
                                 const auto *player = player_states_ + destination * native::kPlayerStateStride;
                                 state.reliable_event = {.event = event,
                                                         .sequence = native::read<std::uint32_t>(player, 0x18),
                                                         .type = native::read<std::uint32_t>(event),
                                                         .source_turn = state.reliable_created_turns[index],
                                                         .destination = destination,
                                                         .cursor = index,
                                                         .head = native::read<std::uint32_t>(context.ebp, -0x24),
                                                         .bits_before = state.destination != nullptr ? state.destination->bits : UINT32_MAX,
                                                         .packet = native::read<std::uint32_t>(context.ebp, 8),
                                                         .generation = state.reliable_generations[index],
                                                         .ordinal = native::read<std::uint32_t>(context.ebp, -0x18) + 1,
                                                         .recipients = native::read<std::uint64_t>(event, 8)};
                             }));
    plan.observe_instruction(locations.reliable_finish, fc::before([this](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();
                                 auto record = state.reliable_event;
                                 record.bits_after = state.destination != nullptr ? state.destination->bits : UINT32_MAX;
                                 const auto selected = (record.recipients & (std::uint64_t{1} << record.destination)) != 0;
                                 record.outcome = !selected ? 12U : record.type <= 13 ? 11U : 13U;
                                 write(trace::RecordKind::ReplicationEvent, record, record.event,
                                       settings_.capture == CaptureMode::Full ? trace::None : trace::Partial);
                             }));
    plan.observe_instruction(locations.reliable_retire, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto event = native::read<std::uint32_t>(context.ebp, -0x14);
                                 const auto index = static_cast<std::uint32_t>((event - native::pointer_id(reliable_ring_)) / 64);
                                 const auto player = native::read<std::uint32_t>(context.ebp, -4);
                                 const trace::ReplicationEventRecord record{.event = event,
                                                                            .sequence = native::read<std::uint32_t>(player, 0x18),
                                                                            .type = native::read<std::uint32_t>(event),
                                                                            .source_turn = state.reliable_created_turns[index],
                                                                            .destination = *current_destination_,
                                                                            .outcome = 14,
                                                                            .cursor = index,
                                                                            .head = *reliable_head_,
                                                                            .generation = state.reliable_generations[index],
                                                                            .recipients = native::read<std::uint64_t>(event, 8)};
                                 write(trace::RecordKind::ReplicationEvent, record, record.event,
                                       settings_.capture == CaptureMode::Full ? trace::Accepted : trace::Accepted | trace::Partial);
                             }));
    plan.observe<EventInvocation>(locations.reliable_free, fc::before([this](std::uint32_t, EventInvocation &invocation) noexcept {
                                      invocation.record.cursor = *reliable_free_;
                                  }),
                                  fc::after([this](std::uint32_t, const EventInvocation &invocation) noexcept {
                                      auto &state = thread_state();
                                      for (auto index = invocation.record.cursor; index != *reliable_free_; index = (index + 1) & 127U) {
                                          const auto *event = reliable_ring_ + index * 64;
                                          const trace::ReplicationEventRecord record{.event = native::pointer_id(event),
                                                                                     .type = native::read<std::uint32_t>(event),
                                                                                     .source_turn = state.reliable_created_turns[index],
                                                                                     .outcome = 15,
                                                                                     .cursor = index,
                                                                                     .head = *reliable_head_,
                                                                                     .generation = state.reliable_generations[index],
                                                                                     .recipients = native::read<std::uint64_t>(event, 8)};
                                          write(trace::RecordKind::ReplicationEvent, record, record.event,
                                                settings_.capture == CaptureMode::Full ? trace::End : trace::End | trace::Partial);
                                      }
                                  }));

    if (settings_.capture != CaptureMode::Full) {
        return;
    }

    const auto serializer = [this, &plan](fc::FunctionLocation<ObjectSerializer> site, std::uint32_t outcome) {
        plan.observe<ReplicationInvocation>(
            site, fc::before([this, outcome](void *packet, void *object, ReplicationInvocation &invocation) noexcept {
                auto &state = thread_state();
                invocation.record = {.object = native::pointer_id(object),
                                     .native_id = native::read<std::uint32_t>(object, 0x204),
                                     .object_type = state.object_type,
                                     .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                     .destination = *current_destination_,
                                     .outcome = outcome,
                                     .packet = native::pointer_id(packet),
                                     .bits_before = state.destination != nullptr ? state.destination->bits : UINT32_MAX,
                                     .priority = selected_object_priority(native::pointer_id(object)),
                                     .wire_id = state.object_wire_id,
                                     .map = state.object_map};
                state.object_serializer = &invocation;
            }),
            fc::after([this](void *, void *, const ReplicationInvocation &invocation) noexcept {
                auto &state = thread_state();
                auto record = invocation.record;
                record.bits_after = state.destination != nullptr ? state.destination->bits : UINT32_MAX;
                state.object_serializer = nullptr;
                write(trace::RecordKind::ReplicationObject, record, record.object);
            }));
    };
    serializer(locations.object_create, 1);
    serializer(locations.object_delta, 2);
    plan.observe_instruction(locations.object_delta_type, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().object_serializer; invocation != nullptr) {
                                     invocation->record.object_type = native::read<std::uint32_t>(context.ebp, -0x14);
                                     invocation->record.outcome = native::read<std::uint8_t>(context.ebp, -1) != 0 ? 6U : 2U;
                                 }
                             }));
    plan.observe_instruction(locations.object_create_wire, fc::before([](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 state.object_wire_id = native::read<std::uint32_t>(native::read<std::uint32_t>(context.ebp, -0x28));
                                 state.object_map = native::read<std::uint32_t>(context.ebp, -0x40);
                                 state.object_type = native::read<std::uint32_t>(context.ebp, -0x17C);
                             }));
    plan.observe_instruction(locations.object_delta_wire, fc::before([](const fc::CpuContext &) noexcept {
                                 auto &state = thread_state();
                                 state.object_wire_id = 0xFFFFFFFFU;
                                 state.object_map = 0;
                                 state.object_type = 0xFFFFFFFFU;
                             }));
    plan.observe_instruction(locations.delta_normal_identity, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().object_serializer; invocation != nullptr) {
                                     invocation->record.wire_id = native::read<std::uint32_t>(context.ebp, -0x0C);
                                     invocation->record.map = native::read<std::uint32_t>(context.ebp, -0x10);
                                 }
                             }));
    plan.observe_instruction(locations.delta_special_identity, fc::before([](const fc::CpuContext &context) noexcept {
                                 if (auto *invocation = thread_state().object_serializer; invocation != nullptr) {
                                     const auto native_state = native::read<std::uint32_t>(context.ebp, -0x1C);
                                     invocation->record.wire_id = native::read<std::uint32_t>(native_state, 0x0C);
                                 }
                             }));

    // Record rejection and eviction at native decisions; successful rows carry rank in their serializer or deferral record.
    plan.observe<ObjectInvocation>(locations.select_object,
                                   fc::before([this](void *object, std::uint32_t insertion, ObjectInvocation &invocation) noexcept {
                                       auto &state = thread_state();
                                       invocation.previous = state.object_selection;
                                       invocation.record = {.object = native::pointer_id(object),
                                                            .native_id = native::read<std::uint32_t>(object, 0x204),
                                                            .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                                            .destination = *current_destination_,
                                                            .outcome = 12,
                                                            .priority = insertion,
                                                            .age_before = *object_count_};
                                       state.object_selection = &invocation;
                                   }),
                                   fc::after([this](void *, std::uint32_t, const ObjectInvocation &invocation) noexcept {
                                       auto record = invocation.record;
                                       record.age_after = *object_count_;
                                       thread_state().object_selection = invocation.previous;
                                       if (record.outcome == 13) {
                                           write(trace::RecordKind::ReplicationObject, record, record.object);
                                       }
                                   }));
    plan.observe_instruction(locations.select_type_rejected, fc::before([](const fc::CpuContext &) noexcept {
                                 thread_state().object_selection->record.outcome = 13;
                             }));
    plan.observe_instruction(locations.select_duplicate, fc::before([](const fc::CpuContext &) noexcept {
                                 thread_state().object_selection->record.outcome = 14;
                             }));
    plan.observe_instruction(locations.select_capacity, fc::before([this](const fc::CpuContext &) noexcept {
                                 if (*object_count_ == 64) {
                                     const auto object = native::read<std::uint32_t>(*object_list_, 63 * 4);
                                     if (object != 0) {
                                         const trace::ReplicationObjectRecord record{
                                             .object = object,
                                             .native_id = native::read<std::uint32_t>(object, 0x204),
                                             .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                             .destination = *current_destination_,
                                             .outcome = 15,
                                             .priority = thread_state().object_selection->record.priority};
                                         write(trace::RecordKind::ReplicationObject, record, object);
                                     }
                                 }
                             }));
    plan.observe_instruction(locations.select_health, fc::before([this](const fc::CpuContext &context) noexcept {
                                 const auto object = native::read<std::uint32_t>(context.ebp, -0x0C);
                                 const trace::ReplicationObjectRecord record{
                                     .object = object,
                                     .native_id = native::read<std::uint32_t>(object, 0x204),
                                     .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                     .destination = *current_destination_,
                                     .outcome = 17,
                                     .priority = static_cast<std::uint32_t>(context.xmm[1].low),
                                     .relevance = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low))};
                                 write(trace::RecordKind::ReplicationObject, record, object);
                             }));
    plan.observe(locations.select_objects, fc::after([this](void *list, void *) noexcept {
                     auto &state = thread_state();
                     state.selected_objects = list;
                     state.selected_object_count = *object_count_;
                 }));
    plan.observe_instruction(locations.object_deferred, fc::before([this](const fc::CpuContext &context) noexcept {
                                 auto &state = thread_state();
                                 const auto object = native::read<std::uint32_t>(context.ebp, -0x198);
                                 const auto object_state = native::read<std::uint32_t>(context.ebp, -0x9C);
                                 const auto wire_id = native::read<std::uint32_t>(object_state);
                                 const auto *player = player_states_ + *current_destination_ * native::kPlayerStateStride;
                                 const auto age = native::read<std::uint8_t>(player, 0x170 + wire_id);
                                 const trace::ReplicationObjectRecord record{.object = object,
                                                                             .native_id = native::read<std::uint32_t>(object, 0x204),
                                                                             .object_type = 0xFFFFFFFFU,
                                                                             .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                                                             .destination = *current_destination_,
                                                                             .outcome = 3,
                                                                             .packet = native::read<std::uint32_t>(context.ebp, 8),
                                                                             .bits_before =
                                                                                 state.destination != nullptr ? state.destination->bits : 0,
                                                                             .budget = native::read<std::uint32_t>(context.ebp, -0x94),
                                                                             .last_turn = native::read<std::uint32_t>(player, 0x20),
                                                                             .priority = selected_object_priority(object),
                                                                             .wire_id = wire_id,
                                                                             .age_before = age,
                                                                             .age_after = context.ecx & 0xFFU};
                                 write(trace::RecordKind::ReplicationObject, record, object);
                             }));
    const auto deleted = [this, &plan](fc::CodeLocation site, bool secondary) {
        plan.observe_instruction(site, fc::before([this, secondary](const fc::CpuContext &context) noexcept {
                                     const trace::ReplicationObjectRecord record{
                                         .source_turn = static_cast<std::uint32_t>(*host_turn_),
                                         .destination = *current_destination_,
                                         .outcome = native::read<std::uint8_t>(context.ebp, secondary ? -0x41 : -0x42) != 0 ? 4U : 5U,
                                         .packet = native::read<std::uint32_t>(context.ebp, 8),
                                         .bits_after =
                                             thread_state().destination != nullptr ? thread_state().destination->bits : UINT32_MAX,
                                         .wire_id = native::read<std::uint32_t>(context.ebp, secondary ? -0x64 : -0x74),
                                         .map = native::read<std::uint32_t>(context.ebp, -0x40)};
                                     write(trace::RecordKind::ReplicationObject, record, record.wire_id, trace::End);
                                 }));
    };
    deleted(locations.object_deleted, false);
    deleted(locations.object_deleted_secondary, true);
}

std::uint32_t Handler::selected_object_priority(std::uint32_t object) const noexcept {
    const auto &state = thread_state();
    for (std::uint32_t index = 0; index != state.selected_object_count; ++index) {
        if (native::read<std::uint32_t>(state.selected_objects, index * 4) == object) {
            return index;
        }
    }
    return 0xFFFFFFFFU;
}

void Handler::observe_event_gate(std::uint32_t gate, const fc::CpuContext &context) noexcept {
    if (auto *invocation = thread_state().decoded_event; invocation != nullptr) {
        invocation->record.gate_visited |= 1U << gate;
        invocation->record.gate_results |= static_cast<std::uint32_t>((context.eax & 0xFFU) != 0) << gate;
    }
}

void Handler::observe_read_object(const fc::CpuContext &context, bool special) noexcept {
    if (auto *invocation = thread_state().object_reader; invocation != nullptr) {
        auto &record = invocation->record;
        record.object = native::read<std::uint32_t>(context.ebp, special ? -0x28 : -0x14);
        record.native_id = native::read<std::uint32_t>(record.object, 0x204);
        record.wire_id = native::read<std::uint32_t>(context.ebp, special ? -0x4C : -0x1C);
        record.outcome = special ? 22U : 21U;
        if (!special) {
            record.map = native::read<std::uint32_t>(context.ebp, -0x30);
        }
    }
}

} // namespace fc_plugins::network_diagnostics
