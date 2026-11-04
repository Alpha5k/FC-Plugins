#include "patch.hpp"

#include "api.hpp"
#include "queue.hpp"
#include "time.hpp"
#include "upper_time.hpp"
#include "../hero_diagnostics/api.hpp"

#include <fc/native.hpp>

#include <WinSock2.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <unordered_map>

namespace fc_plugins::melee_timing {
namespace {

// The native callbacks preserve Win32 and Winsock errors across feature queries and optional observations.
struct AmbientErrors {
    DWORD win32{GetLastError()};
    int winsock{WSAGetLastError()};

    // Restore Win32 last because Winsock can share its error storage.
    ~AmbientErrors() {
        WSASetLastError(winsock);
        SetLastError(win32);
    }
};

// Retail lookups use cdecl and return -1 for a missing local input or parent network player.
using PlayerLookup = std::int32_t(__cdecl *)(std::int32_t) noexcept;

using Update = bool(__thiscall *)(void *, float) noexcept;
using Reset = void(__thiscall *)(void *) noexcept;

struct Layout {
    fc::FunctionLocation<Update> update;
    fc::FunctionLocation<Reset> clear_input;
    fc::FunctionLocation<Reset> destroy;
    fc::CodeLocation produced;
    fc::CodeLocation scan;
    fc::CodeLocation query;
    fc::CodeLocation queried;
    std::array<fc::CodeLocation, 3> winners;
    fc::CodeLocation unqueued;
    std::array<fc::CodeLocation, 2> bounds;
    fc::CodeLocation cutoff;
    fc::CodeLocation cutoff_subtract;
    fc::FunctionLocation<PlayerLookup> local_input;
    fc::FunctionLocation<PlayerLookup> parent_player;
    std::uint32_t parent_operand;
    std::uint32_t parent_table;
};

struct ProfileSites {
    std::string_view profile;
    std::uint32_t update;
    std::uint32_t clear_input;
    std::uint32_t destroy;
    std::uint32_t condition;
    std::uint32_t cutoff;
    std::uint32_t local_input;
    std::uint32_t parent_player;
    std::uint32_t parent_table;
};

constexpr std::array kProfiles{
    ProfileSites{.profile = "BF2.Steam.59EDE353",
                 .update = 0x289290,
                 .clear_input = 0x28B330,
                 .destroy = 0x288430,
                 .condition = 0x28BAA0,
                 .cutoff = 0x289BFA,
                 .local_input = 0x1B73C0,
                 .parent_player = 0x1C3840,
                 .parent_table = 0x1ACDBF4},
    ProfileSites{.profile = "BF2.GOG.59EDF52B",
                 .update = 0x28A320,
                 .clear_input = 0x28C3C0,
                 .destroy = 0x2894B0,
                 .condition = 0x28CB30,
                 .cutoff = 0x28AC8A,
                 .local_input = 0x1B8370,
                 .parent_player = 0x1C47D0,
                 .parent_table = 0x1ACF0A4},
};

// Both retail evaluators decode relative bounds into XMM0/1, keep the clock in XMM5 and the end time at EBP-0x8.
Layout make_layout(const ProfileSites &sites) {
    return {
        .update = {.rva = sites.update, .name = "WeaponMelee::Update"},
        .clear_input = {.rva = sites.clear_input,
                        .name = "WeaponMelee::ClearInput",
                        .evidence = fc::exact_bytes({0xC7, 0x81, 0x20, 0x01, 0, 0, 0, 0, 0, 0})},
        .destroy = {.rva = sites.destroy, .name = "WeaponMelee::~WeaponMelee", .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF})},
        // Update produces inputs before scanning transitions; only the native winning branches promote a query.
        .produced = {.rva = sites.update + 0x1F6,
                     .name = "melee input production completed",
                     .evidence = fc::exact_bytes({0x8B, 0x8B, 0x80, 0x01, 0, 0})},
        .scan = {.rva = sites.update + 0x240, .name = "melee transition scan", .evidence = fc::exact_bytes({0x8B, 0x83, 0x80, 0x01, 0, 0})},
        .query = {.rva = sites.condition,
                  .name = "melee condition query",
                  .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08})},
        .queried = {.rva = sites.condition + 0xE5,
                    .name = "melee successful input timestamp",
                    .evidence = fc::exact_bytes({0xF3, 0x0F, 0x10, 0x06, 0xB0, 0x01})},
        .winners = {{{.rva = sites.update + 0x39F,
                      .name = "melee first winning condition",
                      .evidence = fc::exact_bytes({0x89, 0x7D, 0xE8, 0x89, 0x75, 0xE0})},
                     {.rva = sites.update + 0x57E,
                      .name = "melee second winning condition",
                      .evidence = fc::exact_bytes({0x89, 0x7D, 0xE8, 0x89, 0x75, 0xE0})},
                     {.rva = sites.update + 0x6E0,
                      .name = "melee third winning condition",
                      .evidence = fc::exact_bytes({0x89, 0x75, 0xE8, 0xF3, 0x0F, 0x11, 0x45, 0xF0})}}},
        .unqueued = {.rva = sites.update + 0x34B,
                     .name = "melee unqueued winning condition",
                     .evidence = fc::exact_bytes({0x33, 0xF6, 0x89, 0x7D, 0xE8})},
        .bounds = {{{.rva = sites.condition + 0xAC,
                     .name = "melee upper input bound",
                     .evidence = fc::exact_bytes({0xF3, 0x0F, 0x58, 0xC4, 0x56})},
                    {.rva = sites.condition + 0xB4,
                     .name = "melee lower input bound",
                     .evidence = fc::exact_bytes({0xF3, 0x0F, 0x58, 0xCC, 0x83, 0xE3, 0x0F})}}},
        // Update retains condition time at EBP-0x10, clock in XMM1 and combo age in XMM0.
        .cutoff = {.rva = sites.cutoff,
                   .name = "melee input consumption addition",
                   .evidence = fc::exact_bytes({0xF3, 0x0F, 0x58, 0x4D, 0xF0})},
        .cutoff_subtract = {.rva = sites.cutoff + 5,
                            .name = "melee input consumption subtraction",
                            .evidence = fc::exact_bytes({0xF3, 0x0F, 0x5C, 0xC8})},
        .local_input = {.rva = sites.local_input,
                        .name = "GetJoystickIndex",
                        .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08})},
        // NetPlayer's +0x154 parent selects the owning connection; primary players store -1.
        .parent_player = {.rva = sites.parent_player,
                          .name = "parent network player",
                          .evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x08, 0x00})},
        .parent_operand = sites.parent_player + 0x1D,
        .parent_table = sites.parent_table,
    };
}

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile) {
    for (const auto &sites : kProfiles) {
        if (sites.profile == profile) {
            return make_layout(sites);
        }
    }
    return std::nullopt;
}

// Preserves native predicates and queue capacity while correcting event ages and winning-input consumption.
class QueueHandler {
    struct Invocation {
        Invocation *previous{};
        void *weapon{};
        WinningInput winner;
        bool precise{};
    };

  public:
    // Qualifies the retail instructions and preserves diagnostics at the native consumption call.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        local_input_ = plan.function(layout->local_input);
        parent_player_ = plan.function(layout->parent_player);
        // Windows relocates the unaligned absolute operand in the parent's native array lookup.
        plan.check(layout->parent_operand, 4, fc::points_to(plan.at(layout->parent_table)));
        // The CALL immediately follows this subtraction; reading its own byte would conflict with its diagnostic hook.
        subtract_memory_ = plan.access<const std::byte>(layout->cutoff_subtract.rva, 4, layout->cutoff_subtract.evidence);

        // FC shares this observer with Hero's sole modifier at the same native Update entry.
        plan.observe<Invocation>(layout->update, fc::before([this](void *weapon, float, Invocation &invocation) noexcept {
                                     const AmbientErrors errors;
                                     invocation = {.previous = invocation_, .weapon = weapon, .precise = applies(weapon, precision_, true)};
                                     invocation_ = &invocation;
                                     if (invocation.precise) {
                                         timing(weapon).project(weapon);
                                     } else {
                                         queues_.erase(weapon);
                                     }
                                 }),
                                 fc::after([this](void *weapon, float, bool, const Invocation &invocation) noexcept {
                                     const AmbientErrors errors;
                                     if (invocation.precise) {
                                         timing(weapon).advance(weapon);
                                     }
                                     invocation_ = invocation.previous;
                                 }));
        for (const auto *site : {&layout->clear_input, &layout->destroy}) {
            plan.observe(*site, fc::after([this](void *weapon) noexcept {
                const AmbientErrors errors;
                queues_.erase(weapon);
            }));
        }
        plan.observe_instruction(layout->produced, fc::before([this](const fc::CpuContext &context) noexcept {
                                     if (invocation_ != nullptr && invocation_->precise) {
                                         timing(reinterpret_cast<void *>(context.ebx)).produced(reinterpret_cast<void *>(context.ebx));
                                     }
                                 }));
        plan.observe_instruction(layout->scan, fc::before([this](const fc::CpuContext &) noexcept {
                                     if (invocation_ != nullptr) {
                                         invocation_->winner = {};
                                     }
                                 }));
        plan.observe_instruction(layout->query, fc::before([this](const fc::CpuContext &) noexcept {
                                     if (invocation_ != nullptr) {
                                         invocation_->winner.query.reset();
                                     }
                                 }));
        plan.observe_instruction(layout->queried, fc::before([this](const fc::CpuContext &context) noexcept {
                                     if (invocation_ != nullptr) {
                                         invocation_->winner.query = fc::read_field<float>(context.esi);
                                     }
                                 }));
        for (const auto &site : layout->winners) {
            plan.observe_instruction(site, fc::before([this](const fc::CpuContext &) noexcept {
                                         if (invocation_ != nullptr) {
                                             invocation_->winner.select();
                                         }
                                     }));
        }
        plan.observe_instruction(layout->unqueued, fc::before([this](const fc::CpuContext &) noexcept {
                                     if (invocation_ != nullptr) {
                                         invocation_->winner.selected.reset();
                                     }
                                 }));

        for (std::size_t index{}; index < layout->bounds.size(); ++index) {
            plan.hook_instruction(layout->bounds[index], [this, index](fc::CpuContext &context) noexcept {
                const AmbientErrors errors;
                auto *weapon = reinterpret_cast<void *>(context.edi);
                const bool corrected = applies(weapon, feature_) || applies(weapon, precision_, true);
                const float clock = fc::read_field<float>(&context.xmm[5].low);
                const float relative = fc::read_field<float>(&context.xmm[index].low);
                const float age = fc::read_field<float>(context.ebp, -8);
                const float native = relative + fc::read_field<float>(&context.xmm[4].low);
                const float effective = corrected ? absolute_time(clock, relative, age) : native;
                record(weapon, clock, relative, age, native, effective, static_cast<std::uint8_t>(index), corrected);
                if (corrected) {
                    fc::write_field(&context.xmm[index].low, 0, effective);
                    // Skip just ADDSS in the trampoline; its copied PUSH or AND still executes.
                    context.eip += 4;
                }
            });
        }
        plan.hook_instruction(layout->cutoff, [this](fc::CpuContext &context) noexcept {
            const AmbientErrors errors;
            auto *weapon = reinterpret_cast<void *>(context.ebx);
            const bool corrected = applies(weapon, feature_) || applies(weapon, precision_, true);
            const float clock = fc::read_field<float>(&context.xmm[1].low);
            const float relative = fc::read_field<float>(context.ebp, -0x10);
            const float age = fc::read_field<float>(&context.xmm[0].low);
            const float native = (clock + relative) - age;
            const bool selected =
                invocation_ != nullptr && invocation_->weapon == weapon && invocation_->precise && invocation_->winner.selected.has_value();
            const float effective = selected ? *invocation_->winner.selected : corrected ? absolute_time(clock, relative, age) : native;
            record(weapon, clock, relative, age, native, effective, 2, corrected, selected,
                   invocation_ != nullptr && invocation_->weapon == weapon && invocation_->precise,
                   invocation_ != nullptr && invocation_->weapon == weapon && invocation_->winner.selected.has_value());
            if (corrected) {
                fc::write_field(&context.xmm[1].low, 0, effective);
                // Return directly to the native CALL so its observer sees the corrected cutoff.
                context.eip = reinterpret_cast<std::uint32_t>(consume_);
            }
        });
    }

    // Publishes support on both endpoints and retains optional passive timing observations.
    fc::Result<> prepare(fc::PrepareContext &context) {
        const auto subtract = subtract_memory_.view();
        if (!subtract) {
            return std::unexpected(subtract.error());
        }
        consume_ = subtract->data() + subtract->size();
        game_ = context.game();
        multiplayer_ = context.multiplayer();
        const auto feature = multiplayer_.feature({"FC.MeleeInputTiming", fc::FeatureRole::Both});
        if (!feature) {
            return std::unexpected(feature.error());
        }
        feature_ = *feature;
        const auto precision = multiplayer_.feature({"FC.MeleeInputPrecision", fc::FeatureRole::Both});
        if (!precision) {
            return std::unexpected(precision.error());
        }
        precision_ = *precision;
        const auto peers = feature_.on_peer([this](fc::PeerId peer, fc::Agreement agreement) noexcept {
            if (agreement == fc::Agreement::Unavailable) {
                for (auto &slot : peers_) {
                    auto expected = peer.value;
                    slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
                }
                return;
            }
            const auto info = multiplayer_.info(peer);
            if (info && info->slot && *info->slot < peers_.size()) {
                peers_[*info->slot].store(peer.value, std::memory_order_release);
            }
        });
        if (!peers) {
            return peers;
        }
        if (auto result = context.publish(api_contract(), QueueTimingApi{.compatible = fc::bind<&QueueHandler::compatible>(*this),
                                                                         .capture = fc::bind<&QueueHandler::capture>(*this),
                                                                         .restore = fc::bind<&QueueHandler::restore>(*this)});
            !result) {
            return result;
        }
        return context.on_available("Diagnostics.HeroDiagnostics", hero_diagnostics::api_contract(),
                                    [this](hero_diagnostics::HeroDiagnosticsApi api) noexcept {
                                        diagnostics_ = api;
                                        diagnostics_ready_.store(true, std::memory_order_release);
                                    });
    }

  private:
    // Binds only current queue ages; native ClearInput and destruction retire the borrowed pointer.
    QueueTiming &timing(void *weapon) {
        return queues_.try_emplace(weapon, weapon).first->second;
    }

    // Exposes agreement without allowing a packet-only consumer to advertise queue precision.
    std::uint32_t compatible(std::uint64_t peer) const noexcept {
        const AmbientErrors errors;
        return peer != 0 && precision_.agreement(fc::PeerId{peer}) == fc::Agreement::Compatible;
    }

    // Exports existing logical ages; a new record requires precise gameplay to apply.
    std::uint32_t capture(void *weapon, QueueAges *ages) noexcept {
        const AmbientErrors errors;
        // A decoded packet owns its ages even if agreement changes before the checkpoint captures them.
        const auto saved = queues_.find(weapon);
        if (saved != queues_.end()) {
            *ages = saved->second.capture(weapon);
            return 1;
        }
        if (!applies(weapon, precision_, true)) {
            return 0;
        }
        *ages = timing(weapon).capture(weapon);
        return true;
    }

    // Packet and checkpoint owners restore native rows before importing their corresponding exact ages.
    void restore(void *weapon, const QueueAges *ages) noexcept {
        const AmbientErrors errors;
        if (ages != nullptr) {
            timing(weapon).restore(weapon, *ages);
        } else {
            queues_.erase(weapon);
        }
    }

    // Local and host-owned AI use fixed timing. Player prediction and authority require the same peer agreement.
    bool applies(void *weapon, const fc::Feature &feature, bool remote = false) const noexcept {
        const auto state = game_.current();
        if (!state) {
            return false;
        }
        if (state->mode == fc::GameMode::Local) {
            return true;
        }
        if (state->mode != fc::GameMode::Client && !state->is_host()) {
            return false;
        }
        const auto owner = fc::read_field<const void *>(weapon, 0x6C);
        const auto player = fc::read_field<std::int32_t>(owner, 0xD4);
        if (player < 0) {
            return state->is_host();
        }
        const auto local = local_input_.call(player);
        if (state->mode == fc::GameMode::Client) {
            // Remote queue precision additionally requires ages qualified by the received owner phase.
            const auto host = multiplayer_.host();
            return ((local && *local >= 0) || (remote && queues_.contains(weapon))) && host.value != 0 &&
                   feature.agreement(host) == fc::Agreement::Compatible;
        }
        if (local && *local >= 0) {
            return true;
        }
        const auto parent = parent_player_.call(player);
        if (!parent) {
            return false;
        }
        // Native SendToClients excludes child slots; its parent lookup maps them to the owning connection.
        const auto slot = *parent >= 0 ? *parent : player;
        if (slot < 0 || static_cast<std::size_t>(slot) >= peers_.size()) {
            return false;
        }
        const fc::PeerId peer{peers_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire)};
        return peer.value != 0 && feature.agreement(peer) == fc::Agreement::Compatible;
    }

    // Values report clock, relative time, age, native absolute time and effective absolute time; result marks correction.
    void record(void *weapon, float clock, float relative, float age, float native, float effective, std::uint8_t stage, bool corrected,
                bool selected = false, bool precise = false, bool winner_present = false) const noexcept {
        if (diagnostics_ready_.load(std::memory_order_acquire)) {
            const hero_diagnostics::OwnerDecision decision{.values = {clock, relative, age, native, effective},
                                                           .words = {static_cast<std::uint32_t>(selected),
                                                                     static_cast<std::uint32_t>(precise),
                                                                     static_cast<std::uint32_t>(winner_present)},
                                                           .kind = hero_diagnostics::CauseKind::InputTiming,
                                                           .stage = stage,
                                                           .result = static_cast<std::uint8_t>(corrected)};
            diagnostics_.owner_decision(weapon, &decision);
        }
    }

    fc::NativeFunction<PlayerLookup> local_input_;
    fc::NativeFunction<PlayerLookup> parent_player_;
    fc::Memory<const std::byte> subtract_memory_;
    const std::byte *consume_{};
    fc::Feature feature_;
    fc::Feature precision_;
    Invocation *invocation_{};
    std::unordered_map<void *, QueueTiming> queues_;
    std::array<std::atomic_uint64_t, 64> peers_{};

  protected:
    fc::Game game_;
    fc::Multiplayer multiplayer_;
    hero_diagnostics::HeroDiagnosticsApi diagnostics_{};
    std::atomic_bool diagnostics_ready_{};
};

struct UpperTimeLayout {
    fc::CodeLocation compare;
    fc::CodeLocation upper_branch;
};

struct UpperTimeSites {
    std::string_view profile;
    std::uint32_t compare;
};

constexpr std::array kUpperTimeProfiles{
    UpperTimeSites{.profile = "BF2.Steam.59EDE353", .compare = 0x0028B9D5},
    UpperTimeSites{.profile = "BF2.GOG.59EDF52B", .compare = 0x0028CA65},
};

// Qualifies COMISS XMM3,XMM0 and the six-byte JA to the preliminary condition's failure return.
UpperTimeLayout make_upper_time_layout(const UpperTimeSites &sites) {
    return {
        .compare = {.rva = sites.compare, .name = "melee condition upper time comparison", .evidence = fc::exact_bytes({0x0F, 0x2F, 0xD8})},
        .upper_branch = {.rva = sites.compare + 3,
                         .name = "melee condition upper time rejection",
                         .evidence = fc::exact_bytes({0x0F, 0x87, 0xB9, 0x00, 0x00, 0x00})},
    };
}

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<UpperTimeLayout> select_upper_time_layout(std::string_view profile) {
    for (const auto &sites : kUpperTimeProfiles) {
        if (sites.profile == profile) {
            return make_upper_time_layout(sites);
        }
    }
    return std::nullopt;
}

// Adds the preliminary upper-time correction to precise queue timing for qualified variants.
class Handler final : public QueueHandler {
  public:
    // Hooks the complete JA after COMISS; the native JNS enters COMISS and never enters the displaced branch.
    void plan(fc::Plan &plan) {
        QueueHandler::plan(plan);

        const auto layout = select_upper_time_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan.check(layout->compare.rva, 3, layout->compare.evidence);
        plan.hook_instruction(layout->upper_branch, [this](fc::CpuContext &context) noexcept {
            const auto elapsed = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[3].low));
            const auto upper = std::bit_cast<float>(static_cast<std::uint32_t>(context.xmm[0].low));
            const auto native_flags = context.eflags;
            context.eflags = correct_upper_comparison(native_flags, elapsed, upper, upper_time_applies());
            // Before observers retain native flags; record an intervention only when the resumed branch changes.
            if (context.eflags != native_flags && diagnostics_ready_.load(std::memory_order_acquire)) {
                const hero_diagnostics::OwnerDecision decision{.values = {elapsed, upper},
                                                               .words = {native_flags, context.eflags, context.edi},
                                                               .kind = hero_diagnostics::CauseKind::ConditionTime,
                                                               .result = 1};
                diagnostics_.owner_decision(reinterpret_cast<void *>(context.esi), &decision);
            }
        });
    }

    // Registers the independent upper-time feature after preparing precise queue timing.
    fc::Result<> prepare(fc::PrepareContext &context) {
        if (auto result = QueueHandler::prepare(context); !result) {
            return result;
        }
        const auto feature = multiplayer_.feature({"FC.MeleeTimingFix", fc::FeatureRole::Both});
        if (!feature) {
            return std::unexpected(feature.error());
        }
        upper_time_feature_ = *feature;
        return {};
    }

  private:
    // Local and hosted games use corrected timing. Joining clients require agreement with their current host.
    bool upper_time_applies() const noexcept {
        const auto state = game_.current();
        if (!state) {
            return false;
        }
        if (state->mode == fc::GameMode::Local || state->mode == fc::GameMode::Host || state->mode == fc::GameMode::Dedicated) {
            return true;
        }
        if (state->mode == fc::GameMode::Client) {
            const auto host = multiplayer_.host();
            return host.value != 0 && upper_time_feature_.agreement(host) == fc::Agreement::Compatible;
        }
        return false;
    }

    fc::Feature upper_time_feature_;
};

} // namespace

fc::Patch definition(fc::Role role) {
    fc::Patch patch{.id = "MeleeTiming",
                    .description = "Fixes melee input acceptance, consumption and rounding at combo timing boundaries.",
                    .category = "Multiplayer",
                    .enabled = true,
                    .configurable = true};

    // Steam servers retain queue timing alone; the upper-time correction is qualified for Steam clients and GOG.
    if (role != fc::Role::Server) {
        patch.variants.push_back(fc::variant<Handler>({"BF2.Steam.59EDE353"}, fc::Role::Client));
    }
    if (role != fc::Role::Client) {
        patch.variants.push_back(fc::variant<QueueHandler>({"BF2.Steam.59EDE353"}, fc::Role::Server));
    }
    patch.variants.push_back(fc::variant<Handler>({"BF2.GOG.59EDF52B"}, role));
    return patch;
}

} // namespace fc_plugins::melee_timing
