#include "patch.hpp"

#include "corrections.hpp"
#include "format.hpp"
#include "layout.hpp"
#include "phase.hpp"
#include "../hero_animation_fix/api.hpp"
#include "../hero_diagnostics/api.hpp"
#include "../update_scheduling/slot_peers.hpp"

#include <WinSock2.h>
#include <Windows.h>

#include <atomic>
#include <format>
#include <utility>

namespace fc_plugins::soldier_state_sync {
namespace {

// Native calls can poll receives recursively. Invocation storage restores the enclosing stream on return.
struct SendInvocation {
    SendInvocation *previous{};
    SendFormat format;
    std::int32_t destination{};
    fc::PeerId peer;
    void *phase_weapon{};
    bool phase_weapon_written{};
};

struct ReadInvocation {
    ReadInvocation *previous{};
    SendFormat format;
};

struct FragmentInvocation {
    FragmentInvocation *previous{};
    Format format{};
    fc::PeerId peer;
};

struct ThreadState {
    SendInvocation *send{};
    ReadInvocation *read{};
    FragmentInvocation *fragments{};
    bool energy_write{};
    bool energy_read{};
};

// Keeps native invocation state private to each execution thread.
ThreadState &thread_state() noexcept {
    static thread_local ThreadState state;
    return state;
}

// Owns Soldier update formats and the native movement and trigger corrections.
class Handler final {
  public:
    // Plans the exact Soldier callsites and the native fragment format lifecycle.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }
        get_packet_ = plan.function(layout->get_packet);
        free_packet_ = plan.function(layout->free_packet);
        free_group_ = plan.function(layout->free_group);
        remove_packet_ = plan.function(layout->remove_packet);
        write_bits_ = plan.function(layout->write_bits);
        read_bits_ = plan.function(layout->read_bits);
        endpoints_memory_ = plan.access(layout->endpoints);
        melee_vtable_memory_ = plan.access(layout->melee_vtable);
        update_turn_memory_ = plan.access(layout->update_turn);

        // Add the state format to the existing FIFO scan; other type queries keep their second argument.
        original_receive_packet_ = plan.hook(layout->receive_packet, [this](std::int32_t first, std::int32_t second) noexcept {
            if (first == kUpdatePacket && second == 0x1E) {
                second = kStateUpdatePacket;
            }
            return original_receive_packet_.call(first, second).value_or(nullptr);
        });
        // Match Precision in the same FIFO scan, retaining EAX and the native removal path.
        plan.hook_instruction(layout->receive_match, [](fc::CpuContext &context) noexcept {
            if (context.edi == kUpdatePacket && context.edx == kStateUpdatePacket &&
                (context.eax == kPrecisionUpdatePacket || context.eax == kWeaponStateUpdatePacket)) {
                context.eflags |= 1U << 6;
            }
        });
        plan.hook_instruction(layout->receive_type, [](fc::CpuContext &context) noexcept {
            const auto type = context.eax & 0xFFU;
            if (type == kStateUpdatePacket || type == kPrecisionUpdatePacket || type == kWeaponStateUpdatePacket) {
                context.eflags |= 1U;
            }
        });
        for (std::size_t index = 0; index < layout->receive_prefix.size(); ++index) {
            const auto &site = layout->receive_prefix[index];
            // The preceding comparison contains a relocated receive-buffer address.
            plan.access<const std::byte>(site.compare.rva, site.compare.evidence.bytes.size(), site.compare.evidence);
            receive_type_memory_[index] = plan.access<const std::uint8_t>(site.type.rva);
            plan.hook_instruction(site.branch, [this, index](fc::CpuContext &context) noexcept {
                if (*receive_types_[index] == kStateUpdatePacket || *receive_types_[index] == kPrecisionUpdatePacket ||
                    *receive_types_[index] == kWeaponStateUpdatePacket) {
                    context.eflags |= 1U;
                }
            });
        }

        // Native Disconnect only cleans receive queues. Retire tagged delayed outgoing packets on the native
        // owning thread before a slot loses or changes its identity; roster refreshes preserve queued packets.
        plan.observe(layout->disconnect, fc::before([this](std::uint32_t slot) noexcept {
                         discard_tagged_packets(slot);
                     }));
        plan.observe(layout->connect, fc::before([this](std::uint32_t slot, const void *address) noexcept {
                         if (slot >= 64) {
                             return;
                         }
                         const auto *record = endpoints_ + slot * 0x60;
                         if (*record != std::byte{} &&
                             fc::read_field<std::uint64_t>(record, 0x20) == fc::read_field<std::uint64_t>(address, 0)) {
                             return;
                         }
                         discard_tagged_packets(slot);
                     }));
        next_group_memory_ =
            plan.access<const std::byte>(layout->next_group.rva, layout->next_group.evidence.bytes.size(), layout->next_group.evidence);

        // Shared initial-state groups are constructed elsewhere, with curDst=-1, and remain legacy.
        plan.observe<SendInvocation>(
            layout->send_update, fc::before([this](std::int32_t destination, SendInvocation &invocation) noexcept {
                auto &state = thread_state();
                invocation.previous = state.send;
                invocation.destination = destination;
                const fc::PeerId peer{destination >= 0 ? peers_.current(static_cast<std::size_t>(destination)) : 0};
                invocation.peer = peer;
                if (peer.value != 0 && peer != multiplayer_.local()) {
                    invocation.format.format = select_format(feature_.agreement(peer) == fc::Agreement::Compatible,
                                                             state_feature_.agreement(peer) == fc::Agreement::Compatible,
                                                             precision_feature_.agreement(peer) == fc::Agreement::Compatible,
                                                             weapon_state_feature_.agreement(peer) == fc::Agreement::Compatible);
                }
                state.send = &invocation;
            }),
            fc::after([](std::int32_t, const SendInvocation &invocation) noexcept {
                thread_state().send = invocation.previous;
            }));
        plan.observe(layout->write_update, fc::before([](void *group) noexcept {
                         if (auto *send = thread_state().send) {
                             send->format.bind(group);
                         }
                     }));
        plan.hook_instruction(layout->write_velocity, [](fc::CpuContext &context) noexcept {
            const auto *send = thread_state().send;
            if (send != nullptr && send->format.applies(reinterpret_cast<void *>(context.ecx))) {
                fc::write_field<std::int32_t>(context.esp, 0, 32);
            }
        });

        plan.hook_instruction(layout->write_state_clock, [](fc::CpuContext &context) noexcept {
            const auto *send = thread_state().send;
            if (send != nullptr && send->format.applies(reinterpret_cast<void *>(context.ecx))) {
                fc::write_field(context.esp, 0, state_clock_bits(send->format.format));
            }
        });

        // Precision applies to every shared Trigger call in its bound update group.
        original_write_trigger_ = plan.hook(layout->write_trigger, [this](void *group, std::uint32_t *trigger) noexcept {
            const auto *send = thread_state().send;
            if (send == nullptr || !precise_gameplay(send->format.format) || !send->format.applies(group)) {
                static_cast<void>(original_write_trigger_.call(group, trigger));
                return;
            }
            static_cast<void>(write_bits_.call(group, trigger_code(*trigger), 26));
        });
        original_read_trigger_ = plan.hook(layout->read_trigger, [this](void *group, std::uint32_t *trigger) noexcept {
            const auto *read = thread_state().read;
            if (read == nullptr || !precise_gameplay(read->format.format) || !read->format.applies(group)) {
                static_cast<void>(original_read_trigger_.call(group, trigger));
                return;
            }
            std::uint32_t code{};
            static_cast<void>(read_bits_.call(group, &code, 26));
            *trigger = restore_trigger(*trigger, code & 0x1FU, code >> 5);
        });

        // Keep native overheat and range handling. The scoped Soldier call alone selects Fixed's raw float path.
        original_write_energy_ = plan.hook_call<Energy>(
            plan.at(layout->write_energy.rva, layout->write_energy.name),
            [this](void *energy, void *group) noexcept {
                auto &state = thread_state();
                const auto *send = state.send;
                const auto previous = std::exchange(state.energy_write, send != nullptr && precise_gameplay(send->format.format) &&
                                                                            send->format.applies(group));
                static_cast<void>(original_write_energy_.call(energy, group));
                state.energy_write = previous;
            },
            layout->write_energy.evidence);
        original_read_energy_ = plan.hook_call<Energy>(
            plan.at(layout->read_energy.rva, layout->read_energy.name),
            [this](void *energy, void *group) noexcept {
                auto &state = thread_state();
                const auto *read = state.read;
                const auto previous = std::exchange(state.energy_read, read != nullptr && precise_gameplay(read->format.format) &&
                                                                           read->format.applies(group));
                static_cast<void>(original_read_energy_.call(energy, group));
                state.energy_read = previous;
            },
            layout->read_energy.evidence);
        plan.hook_instruction(layout->energy_write_count, [](fc::CpuContext &context) noexcept {
            if (thread_state().energy_write) {
                fc::write_field<std::int32_t>(context.esp, 0, 32);
            }
        });
        plan.hook_instruction(layout->energy_read_count, [](fc::CpuContext &context) noexcept {
            if (thread_state().energy_read) {
                fc::write_field<std::int32_t>(context.esp, 0, 32);
            }
        });

        // Scope heat exactly at Weapon's conditional EnergyBar call; its overheat handling stays native.
        original_write_heat_ = plan.hook_call<Energy>(
            plan.at(layout->write_heat.rva, layout->write_heat.name),
            [this](void *energy, void *group) noexcept {
                auto &state = thread_state();
                const auto *send = state.send;
                const auto previous = std::exchange(state.energy_write, send != nullptr && send->format.format == Format::WeaponState &&
                                                                            send->format.applies(group));
                static_cast<void>(original_write_heat_.call(energy, group));
                state.energy_write = previous;
            },
            layout->write_heat.evidence);
        original_read_heat_ = plan.hook_call<Energy>(
            plan.at(layout->read_heat.rva, layout->read_heat.name),
            [this](void *energy, void *group) noexcept {
                auto &state = thread_state();
                const auto *read = state.read;
                const auto previous = std::exchange(state.energy_read, read != nullptr && read->format.format == Format::WeaponState &&
                                                                           read->format.applies(group));
                static_cast<void>(original_read_heat_.call(energy, group));
                state.energy_read = previous;
            },
            layout->read_heat.evidence);

        // Reduced updates can omit weapon state. The extra phase needs the combo from that state.
        plan.observe(layout->write_soldier, fc::before([this](void *soldier, void *group, void *) noexcept {
                         auto *send = thread_state().send;
                         if (send != nullptr && send->format.applies(group)) {
                             send->phase_weapon = active_melee(soldier, melee_vtable_);
                             send->phase_weapon_written = false;
                         }
                     }));
        plan.observe(layout->write_melee, fc::after([](void *weapon, void *group) noexcept {
                         auto *send = thread_state().send;
                         if (send != nullptr && send->format.applies(group) && send->phase_weapon == weapon) {
                             send->phase_weapon_written = true;
                         }
                     }));

        plan.observe(layout->write_soldier, fc::after([this](void *soldier, void *group, void *) noexcept {
                         const auto *send = thread_state().send;
                         if (send == nullptr || send->format.format != Format::WeaponState || !send->format.applies(group)) {
                             return;
                         }
                         auto *weapon = active_melee(soldier, melee_vtable_);
                         const auto profile = weapon != nullptr ? phase_profile(weapon) : std::nullopt;
                         auto phase = send->phase_weapon_written && profile ? capture_phase(weapon, *profile) : std::nullopt;
                         static_cast<void>(write_bits_.call(group, phase.has_value(), 1));
                         if (!phase) {
                             return;
                         }
                         // Each owner must support precise input timing; the payload records whether ages were captured.
                         phase->precise_ages = queue_timing_ready_.load(std::memory_order_acquire) &&
                                               queue_timing_.compatible(send->peer.value) && queue_timing_.capture(weapon, &phase->ages);
                         write_phase(*profile, *phase, [this, group](std::uint32_t value, std::int32_t bits) noexcept {
                             static_cast<void>(write_bits_.call(group, value, bits));
                         });
                     }));
        plan.observe(layout->read_soldier, fc::after([this](void *soldier, void *group, void *) noexcept {
                         const auto *read = thread_state().read;
                         const auto format = read != nullptr ? read->format.format : Format::Native;
                         if (read == nullptr || format != Format::WeaponState || !read->format.applies(group)) {
                             phase_decision(soldier, nullptr, format, 0, 0);
                             return;
                         }
                         const auto get = [this, group](std::int32_t bits) noexcept {
                             std::uint32_t value{};
                             static_cast<void>(read_bits_.call(group, &value, bits));
                             return value;
                         };
                         if (get(1) == 0) {
                             phase_decision(soldier, nullptr, format, 1, 0);
                             return;
                         }
                         // Consume the phase even when this Soldier has no compatible local melee profile.
                         const auto phase = read_phase(get);

                         // Native ReadObject has completed its Select/Deselect callbacks and every weapon read.
                         auto *weapon = active_melee(soldier, melee_vtable_);
                         const auto profile = weapon != nullptr ? phase_profile(weapon) : std::nullopt;
                         if (!profile) {
                             phase_decision(soldier, weapon, format, 2, 1U | (phase ? 4U : 0U));
                             return;
                         }
                         if (!phase || !restore_phase(weapon, *profile, *phase)) {
                             phase_decision(soldier, weapon, format, 3, 3U | (phase ? 4U : 0U));
                             return;
                         }
                         if (queue_timing_ready_.load(std::memory_order_acquire)) {
                             queue_timing_.restore(weapon, phase->precise_ages ? &phase->ages : nullptr);
                         }
                         if (hero_replay_ready_.load(std::memory_order_acquire)) {
                             hero_replay_.received(soldier, *update_turn_);
                         }
                         phase_decision(soldier, weapon, format, 4, 7U | (phase->precise_ages ? 8U : 0U));
                     }));

        original_send_group_ = plan.hook(layout->send_group, [this](std::int32_t destination, std::int32_t type, void *group) noexcept {
            auto &state = thread_state();
            const auto *send = state.send;
            FragmentInvocation invocation{.previous = state.fragments,
                                          .format = type == kUpdatePacket && send != nullptr && destination == send->destination &&
                                                            send->format.applies(group)
                                                        ? send->format.format
                                                        : Format::Native};
            // An oversized raw payload cannot be relabeled legacy. Release it without emitting any fragment.
            const auto count = fc::read_field<std::int32_t>(group, 0x0C);
            if (invocation.format != Format::Native && (count <= 0 || count > kIndexMask)) {
                static_cast<void>(free_group_.call(group));
                return;
            }
            invocation.peer = send != nullptr ? send->peer : fc::PeerId{};
            state.fragments = &invocation;
            static_cast<void>(original_send_group_.call(destination, type, group));
            state.fragments = invocation.previous;
        });
        original_send_fragment_ = plan.hook_call<GroupSend>(
            plan.at(layout->send_fragment.rva, layout->send_fragment.name),
            [this](std::int32_t destination, std::int32_t type, void *packet) noexcept {
                const auto *fragments = thread_state().fragments;
                if (fragments != nullptr && fragments->format != Format::Native) {
                    // DoRecvs between fragments can retire or replace the destination. Fail the frozen connection's
                    // remaining fragments instead of submitting raw data to the slot's new occupant.
                    const auto &feature = fragments->format == Format::WeaponState ? weapon_state_feature_
                                          : fragments->format == Format::Precision ? precision_feature_
                                          : fragments->format == Format::State32   ? state_feature_
                                                                                   : feature_;
                    if (feature.agreement(fragments->peer) != fc::Agreement::Compatible) {
                        static_cast<void>(free_packet_.call(packet));
                        return;
                    }
                    if (fragments->format == Format::WeaponState) {
                        type = kWeaponStateUpdatePacket;
                    } else if (fragments->format == Format::Precision) {
                        type = kPrecisionUpdatePacket;
                    } else if (fragments->format == Format::State32) {
                        type = kStateUpdatePacket;
                    }
                    const auto index = fc::read_field<std::uint8_t>(packet, 0x10);
                    fc::write_field<std::uint8_t>(packet, 0x10, index | kRawMagnitude);
                }
                static_cast<void>(original_send_fragment_.call(destination, type, packet));
            },
            layout->send_fragment.evidence);

        original_get_group_ = plan.hook(layout->get_group, [this](void *group, void *packet) noexcept {
            const auto type = fc::read_field<std::uint8_t>(packet, 0x0E);
            const auto index = fc::read_field<std::uint8_t>(packet, 0x10);
            if (!valid_update_fragment(type, index, fc::read_field<std::uint8_t>(packet, 0x11))) {
                static_cast<void>(free_packet_.call(packet));
                return false;
            }
            return original_get_group_.call(group, packet).value_or(false);
        });
        // Preserve the stored tag while allowing index zero to use the native one-fragment fast path.
        plan.write(layout->single_fragment, fc::byte_array<0xF6, 0x43, 0x10, 0x7F>());
        plan.hook_instruction(layout->group_identity, [this](fc::CpuContext &context) noexcept {
            const auto type = fc::read_field<std::uint8_t>(context.ebx, 0x0E);
            if (!update_packet(type)) {
                return;
            }
            const auto incoming = fc::read_field<std::uint8_t>(context.ebx, 0x10);
            const auto pending = fc::read_field<std::uint8_t>(context.eax, 0x10);
            if (!same_group_format(type, incoming, fc::read_field<std::uint8_t>(context.ebx, 0x11),
                                   fc::read_field<std::uint8_t>(context.eax, 0x0E), pending,
                                   fc::read_field<std::uint8_t>(context.eax, 0x11))) {
                context.eip = reinterpret_cast<std::uint32_t>(next_group_);
            }
        });
        for (const auto &site : layout->order) {
            plan.hook_instruction(site, [](fc::CpuContext &context) noexcept {
                // Emulate MOVZX/CMP together so both native branch destinations remain outside the detours.
                context.ecx = fc::read_field<std::uint8_t>(context.eax, 0x10);
                if (update_packet(fc::read_field<std::uint8_t>(context.eax, 0x0E))) {
                    context.ecx &= kIndexMask;
                }
                // Only the adjacent JZ/JNZ consumes these comparison flags; subsequent calls replace them.
                context.eflags = (context.eflags & ~(1U << 6)) | (context.ecx == context.edi ? 1U << 6 : 0U);
                // Skip the copied MOVZX/CMP; the trampoline then returns to the native branch.
                context.eip += 6;
            });
        }

        // Retained groups, turn peeks, and LateUpdateRecovery all keep the original decoded header.
        plan.observe<ReadInvocation>(layout->read_update, fc::before([this](void *group, ReadInvocation &invocation) noexcept {
                                         auto &state = thread_state();
                                         invocation.previous = state.read;
                                         invocation.format.group = group;
                                         const auto packet = get_packet_.call(fc::read_field<std::int32_t>(group, 4)).value_or(nullptr);
                                         invocation.format.format = packet != nullptr
                                                                        ? packet_format(fc::read_field<std::uint8_t>(packet, 0x0E),
                                                                                        fc::read_field<std::uint8_t>(packet, 0x10))
                                                                        : Format::Native;
                                         state.read = &invocation;
                                     }),
                                     fc::after([](void *, const ReadInvocation &invocation) noexcept {
                                         thread_state().read = invocation.previous;
                                     }));
        plan.hook_instruction(layout->read_velocity, [](fc::CpuContext &context) noexcept {
            const auto *read = thread_state().read;
            if (read != nullptr && read->format.applies(reinterpret_cast<void *>(context.ecx))) {
                fc::write_field<std::int32_t>(context.esp, 0, 32);
            }
        });
        plan.hook_instruction(layout->read_state_clock, [](fc::CpuContext &context) noexcept {
            const auto *read = thread_state().read;
            if (read != nullptr && read->format.applies(reinterpret_cast<void *>(context.ecx))) {
                fc::write_field(context.esp, 4, state_clock_bits(read->format.format));
            }
        });

        ground_rotation_.plan(plan);
        plan_trigger(plan);
    }

    // Resolves continuations and publishes support for all retained formats.
    fc::Result<> prepare(fc::PrepareContext &context) {
        const auto next = next_group_memory_.view();
        if (!next) {
            return std::unexpected(next.error());
        }
        next_group_ = next->data();
        const auto endpoints = endpoints_memory_.view();
        if (!endpoints) {
            return std::unexpected(endpoints.error());
        }
        endpoints_ = endpoints->data();
        for (std::size_t index = 0; index < receive_type_memory_.size(); ++index) {
            const auto type = receive_type_memory_[index].view();
            if (!type) {
                return std::unexpected(type.error());
            }
            receive_types_[index] = type->data();
        }
        const auto vtable = melee_vtable_memory_.view();
        if (!vtable) {
            return std::unexpected(vtable.error());
        }
        melee_vtable_ = vtable->data();
        const auto turn = update_turn_memory_.view();
        if (!turn) {
            return std::unexpected(turn.error());
        }
        update_turn_ = turn->data();
        if (auto result = context.on_available("MeleeTiming", melee_timing::api_contract(),
                                               [this](const melee_timing::QueueTimingApi &api) noexcept {
                                                   queue_timing_ = api;
                                                   queue_timing_ready_.store(true, std::memory_order_release);
                                               });
            !result) {
            return result;
        }
        if (auto result = context.on_available("HeroAnimationFix", hero_animation_fix::api_contract(),
                                               [this](const hero_animation_fix::ReplayApi &api) noexcept {
                                                   hero_replay_ = api;
                                                   hero_replay_ready_.store(true, std::memory_order_release);
                                               });
            !result) {
            return result;
        }
        if (auto result = context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                               [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                                   diagnostics_ = api;
                                                   diagnostics_ready_.store(true, std::memory_order_release);
                                               });
            !result) {
            return result;
        }
        multiplayer_ = context.multiplayer();
        const auto feature = multiplayer_.feature({"FC.SoldierVelocity32", fc::FeatureRole::Both});
        if (!feature) {
            return std::unexpected(feature.error());
        }
        feature_ = *feature;
        const auto state_feature = multiplayer_.feature({"FC.SoldierState32", fc::FeatureRole::Both});
        if (!state_feature) {
            return std::unexpected(state_feature.error());
        }
        state_feature_ = *state_feature;
        const auto precision_feature = multiplayer_.feature({"FC.SoldierPrecision", fc::FeatureRole::Both});
        if (!precision_feature) {
            return std::unexpected(precision_feature.error());
        }
        precision_feature_ = *precision_feature;
        const auto weapon_state_feature = multiplayer_.feature({"FC.SoldierWeaponState", fc::FeatureRole::Both});
        if (!weapon_state_feature) {
            return std::unexpected(weapon_state_feature.error());
        }
        weapon_state_feature_ = *weapon_state_feature;
        if (auto result = ground_rotation_.prepare(context); !result) {
            return result;
        }

        return feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            if (agreement == fc::Agreement::Unavailable) {
                peers_.withdraw(peer.value);
                return;
            }
            const auto info = multiplayer_.info(peer);
            if (info && info->slot) {
                peers_.publish(*info->slot, peer.value);
            }
        });
    }

  private:
    // Reports final import status; bits 0..3 mark phase, supported profile, successful parse and precise ages.
    void phase_decision(void *soldier, void *weapon, Format format, std::uint8_t reason, std::uint32_t flags) const noexcept {
        if (!diagnostics_ready_.load(std::memory_order_acquire)) {
            return;
        }
        if (weapon == nullptr) {
            const auto slot = fc::read_field<std::uint8_t>(soldier, 0x742) & 0x0FU;
            if (slot < 8) {
                weapon = fc::read_field<void *>(soldier, 0x720 + 4 * slot);
            }
        }
        hero_diagnostics::PhysicsDecision decision{.words = {static_cast<std::uint32_t>(format), static_cast<std::uint32_t>(*update_turn_),
                                                             static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(weapon)), flags},
                                                   .operation = hero_diagnostics::PhysicsOperation::WeaponPhase,
                                                   .reason = reason,
                                                   .result = static_cast<std::uint8_t>(reason == 4)};
        if (reason == 4) {
            decision.values[0] = fc::read_field<float>(weapon, 0xB4);
            decision.values[1] = fc::read_field<float>(weapon, 0x130);
            decision.values[2] = fc::read_field<float>(weapon, 0x188);
            decision.values[3] = fc::read_field<float>(weapon, 0x118);
        }
        const auto win32 = GetLastError();
        const auto winsock = WSAGetLastError();
        diagnostics_.physics_decision(soldier, 1, &decision);
        WSASetLastError(winsock);
        SetLastError(win32);
    }

    hero_diagnostics::HeroDiagnosticsApi diagnostics_;
    std::atomic_bool diagnostics_ready_{};

    // Remove only this format's delayed packets; native legacy queue contents retain their existing behavior.
    void discard_tagged_packets(std::uint32_t slot) noexcept {
        if (slot >= 64) {
            return;
        }
        auto *group = endpoints_ + slot * 0x60 + 4;
        auto index = fc::read_field<std::int32_t>(group, 4);
        const auto count = fc::read_field<std::int32_t>(group, 0x0C);
        for (std::int32_t i = 0; i < count && index >= 0; ++i) {
            auto *packet = get_packet_.call(index).value_or(nullptr);
            if (packet == nullptr) {
                break;
            }
            index = fc::read_field<std::int16_t>(packet, 0);
            if (raw_magnitude(fc::read_field<std::uint8_t>(packet, 0x0E), fc::read_field<std::uint8_t>(packet, 0x10))) {
                static_cast<void>(remove_packet_.call(group, packet));
                static_cast<void>(free_packet_.call(packet));
            }
        }
    }

    fc::Multiplayer multiplayer_;
    fc::Feature feature_;
    fc::Feature state_feature_;
    fc::Feature precision_feature_;
    fc::Feature weapon_state_feature_;
    GroundRotation ground_rotation_;
    melee_timing::QueueTimingApi queue_timing_;
    hero_animation_fix::ReplayApi hero_replay_;
    std::atomic<bool> queue_timing_ready_{};
    std::atomic<bool> hero_replay_ready_{};
    fc::Memory<const std::byte> melee_vtable_memory_;
    const std::byte *melee_vtable_{};
    fc::Memory<const std::int32_t> update_turn_memory_;
    const std::int32_t *update_turn_{};
    update_scheduling::SlotPeers<64> peers_;
    fc::NativeFunction<GroupSend> original_send_group_;
    fc::NativeFunction<GroupSend> original_send_fragment_;
    fc::NativeFunction<GetGroup> original_get_group_;
    fc::NativeFunction<ReceivePacket> original_receive_packet_;
    fc::NativeFunction<GetPacket> get_packet_;
    fc::NativeFunction<FreePacket> free_packet_;
    fc::NativeFunction<FreeGroup> free_group_;
    fc::NativeFunction<RemovePacket> remove_packet_;
    fc::NativeFunction<Trigger> original_write_trigger_;
    fc::NativeFunction<Trigger> original_read_trigger_;
    fc::NativeFunction<Energy> original_write_energy_;
    fc::NativeFunction<Energy> original_read_energy_;
    fc::NativeFunction<Energy> original_write_heat_;
    fc::NativeFunction<Energy> original_read_heat_;
    fc::NativeFunction<WriteBits> write_bits_;
    fc::NativeFunction<ReadBits> read_bits_;
    fc::Memory<std::byte> endpoints_memory_;
    std::byte *endpoints_{};
    fc::Memory<const std::byte> next_group_memory_;
    const std::byte *next_group_{};
    std::array<fc::Memory<const std::uint8_t>, 2> receive_type_memory_;
    std::array<const std::uint8_t *, 2> receive_types_{};
};

} // namespace

fc::Patch definition(fc::Role role) {
    fc::Patch patch{.id = "SoldierStateSync",
                    .description = "Keeps player movement and weapon actions in sync in multiplayer.",
                    .category = "Multiplayer",
                    .enabled = true};
    if (role == fc::Role::Client) {
        patch.variants.push_back(fc::variant<Handler>({"BF2.Steam.59EDE353"}, role));
    }
    patch.variants.push_back(fc::variant<Handler>({"BF2.GOG.59EDF52B"}, role));
    return patch;
}

} // namespace fc_plugins::soldier_state_sync
