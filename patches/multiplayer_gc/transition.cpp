#include "transition.hpp"
#include "layout.hpp"
#include "transition_layout.hpp"

#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace fc_plugins::multiplayer_gc {
namespace {

constexpr std::uint32_t kHostSize = 0x1590;
constexpr std::uint32_t kJoinSize = 0x1C0;
constexpr std::uint32_t kPlayerStart = 0x1C;
constexpr std::uint32_t kPlayerStride = 0x50;
constexpr std::uint32_t kNetPlayerStride = 0x208;

} // namespace

void Transition::plan(fc::Plan &plan) {
    const auto sites = select_transition_layout(plan.profile());
    const auto shell_sites = select_shell_layout(plan.profile());
    if (!sites || !shell_sites) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }

    // These original Create stores qualify both driver globals independently of their runtime values.
    for (const auto &[offset, target] : std::array{std::pair{0u, sites->host_global}, std::pair{10u, sites->join_global}}) {
        plan.check(sites->driver_clear + offset, 2, fc::exact_bytes({0xC7, 0x05}));
        plan.check(sites->driver_clear + offset + 2, 4, fc::points_to(plan.at(target)));
    }
    plan.check(sites->setup_mode_store, 2, fc::exact_bytes({0xC7, 0x05}));
    plan.check(sites->setup_mode_store + 2, 4, fc::points_to(plan.at(sites->setup_mode)));
    plan.check(sites->teams_reference, 4, fc::points_to(plan.at(sites->teams)));
    // Character::Init records the authenticated native slot before calling the original team membership operation.
    plan.check(sites->character_slot_store, 6, fc::exact_bytes({0x89, 0x87, 0x30, 0x01, 0x00, 0x00}));
    plan.check(sites->character_change_call, 5, fc::direct_call_to(plan.at(sites->character_change)));
    // UpdateStats reads the current mission postload actors before deciding whether to cancel the round.
    plan.check(sites->stats_cancel - 0x12, 4, fc::points_to(plan.at(sites->postload_host)));
    plan.check(sites->stats_cancel - 8, 4, fc::points_to(plan.at(sites->postload_join)));
    host_ = plan.access<void *>(sites->host_global);
    join_ = plan.access<void *>(sites->join_global);
    teams_ = plan.access<void *>(sites->teams);
    postload_host_ = plan.access<void *>(sites->postload_host);
    postload_join_ = plan.access<void *>(sites->postload_join);
    setup_mode_ = plan.access<std::uint32_t>(sites->setup_mode);
    // Shell's editor gate qualifies this shell flag; shell factions and mission team numbers use different mappings.
    shell_state_ = plan.access<std::uint8_t>(shell_sites->chat.shell_state);
    // SetStartTeam writes this array; Init resets the selected flag before mission postload.
    plan.check(sites->set_team + 0x4C, 4, fc::points_to(plan.at(sites->net_players + 0x158)));
    plan.check(sites->player_init + 0x337, 7, fc::exact_bytes({0xC6, 0x82, 0xD5, 0x01, 0, 0, 0}));
    team_selected_ = plan.access<bool>(sites->net_players + 0x1D5, 63 * kNetPlayerStride + 1);
    // Both shell readers change factions through this cdecl call; the override covers later network updates.
    plan.check(shell_sites->read_move + 0x115, 5, fc::direct_call_to(plan.at(sites->set_team)));
    plan.check(shell_sites->read_update + 0x696, 5, fc::direct_call_to(plan.at(sites->set_team)));
    plan.check(sites->set_team + 0x50, 4, fc::exact_bytes({0x8B, 0xE5, 0x5D, 0xC3}));

    allocate_ = plan.function<TransitionAllocate>(sites->allocate, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0x00, 0xFF, 0x75, 0x08}));
    time_ = plan.function<TransitionTime>(sites->time, fc::exact_bytes({0xF3, 0x0F, 0x10, 0x05}));
    connected_ = plan.function<TransitionConnected>(sites->connected, fc::exact_bytes({0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05}));
    // GetLocalIndex is a cdecl wrapper around the game's local-player-to-network-slot array.
    local_slot_ = plan.function<TransitionLocalSlot>(shell_sites->local_slot, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08}));
    plan.check(shell_sites->local_slot + 11, 2, fc::exact_bytes({0x5D, 0xC3}));
    drop_ = plan.function<TransitionDrop>(sites->drop_player, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08}));
    reset_player_ = plan.function<TransitionReset>(sites->player_reset, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC}));
    translate_ = plan.function<TransitionTranslate>(sites->translate_side, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x51}));

    const auto deleting = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC});
    host_delete_ = plan.hook<TransitionDelete>(
        sites->host_delete,
        [this](void *driver, std::uint32_t flags) noexcept {
            const bool captured = (flags & 1u) && driver == host_.read().value_or(nullptr) && capture(driver, true);
            const auto result = host_delete_.call(driver, flags).value_or(nullptr);
            if (captured) {
                auto expected = Stage::Prepared;
                stage_.compare_exchange_strong(expected, Stage::Mission, std::memory_order_acq_rel);
            }
            return result;
        },
        deleting);
    join_delete_ = plan.hook<TransitionDelete>(
        sites->join_delete,
        [this](void *driver, std::uint32_t flags) noexcept {
            const bool captured = (flags & 1u) && driver == join_.read().value_or(nullptr) && capture(driver, false);
            const auto result = join_delete_.call(driver, flags).value_or(nullptr);
            if (captured) {
                auto expected = Stage::Prepared;
                stage_.compare_exchange_strong(expected, Stage::Mission, std::memory_order_acq_rel);
            }
            return result;
        },
        deleting);

    // Include the vtable load so the game's branch for a missing driver lands after the hook.
    // Preserve an uncanceled GC round when playlist exhaustion would otherwise cancel postload.
    plan.hook_instruction(
        sites->stats_cancel,
        [this](fc::CpuContext &cpu) noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (!session || !session->active || stage_.load(std::memory_order_acquire) != Stage::Mission) {
                return;
            }
            if (fc::read_field<bool>(cpu.ecx, saved_host_ ? 0x1420 : 0x18)) {
                disarm();
                return;
            }
            auto expected = Stage::Mission;
            if (stage_.compare_exchange_strong(expected, Stage::Returning, std::memory_order_acq_rel)) {
                cpu.eip += 5;
            }
        },
        fc::exact_bytes({0x8B, 0x01, 0xFF, 0x50, 0x04}));

    // Completed GC battles return to the connected campaign shell through native playlist exhaustion.
    next_mission_ = plan.hook_call<TransitionNextMission>(
        sites->next_mission_call,
        [this]() noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (session && session->active && stage_.load(std::memory_order_acquire) == Stage::Mission) {
                const auto actor = (saved_host_ ? postload_host_ : postload_join_).read().value_or(nullptr);
                if (actor && !fc::read_field<bool>(actor, saved_host_ ? 0x1420 : 0x18)) {
                    return false;
                }
            }
            return next_mission_.call().value_or(false);
        },
        fc::direct_call_to(plan.at(sites->next_mission)));

    // Create(false) still resets mission objects, packets, counters, statistics and chat. Only identities use preserve=true.
    player_init_ = plan.hook_call<TransitionInit>(
        sites->player_init_call,
        [this](void *player, bool preserve) noexcept {
            static_cast<void>(player_init_.call(player, preserve || preserving_));
        },
        fc::direct_call_to(plan.at(sites->player_init)));
    host_init_ = plan.hook_call<TransitionInit>(
        sites->host_init_call,
        [this](void *host, bool preserve) noexcept {
            static_cast<void>(host_init_.call(host, preserve || preserving_));
        },
        fc::direct_call_to(plan.at(sites->host_init)));
    plan.check(sites->host_index_store + 2, 4, fc::points_to(plan.at(sites->host_index)));
    plan.hook_instruction(
        sites->host_index_store,
        [this](fc::CpuContext &cpu) noexcept {
            if (preserving_) {
                cpu.eip += 10;
            }
        },
        fc::masked_bytes({0xC7, 0x05, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}));
    comm_create_ = plan.hook_call<TransitionCommCreate>(
        sites->comm_create_call,
        [this](bool preserve) noexcept {
            static_cast<void>(comm_create_.call(preserve || preserving_));
        },
        fc::direct_call_to(plan.at(sites->comm_create)));
    create_ = plan.hook<TransitionCreate>(
        sites->create,
        [this](bool mission) noexcept {
            preserving_ = !mission && stage_.load(std::memory_order_acquire) == Stage::Returning;
            const bool returning = preserving_;
            static_cast<void>(create_.call(mission));
            preserving_ = false;
            if (returning && stage_.load(std::memory_order_acquire) == Stage::Returning) {
                resume();
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF}));

    // Existing sources bypass BanIP. Both native host drivers call it before admitting an unassociated JOIN.
    ban_ = plan.hook<TransitionBan>(
        sites->ban_ip,
        [this](std::uint32_t source, void *packet) noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (session && session->active && session->host && (session->admission_closed || continuing())) {
                return true;
            }
            return ban_.call(source, packet).value_or(false);
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0xC6, 0x45, 0xFF, 0x00}));
    plan.observe<TransitionReset>(sites->host_update, fc::before([this](void *) noexcept {
                                      remove_unassigned();
                                  }),
                                  fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00}));

    // Mission scripts choose team numbers. Resolve their completed Team sides before CreateLast and player integration.
    postload_ = plan.hook<TransitionPostload>(
        sites->postload,
        [this]() noexcept {
            if (stage_.load(std::memory_order_acquire) == Stage::Mission) {
                resolve_teams();
            }
            return postload_.call().value_or(0);
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8}));
    set_team_ = plan.hook<TransitionTeam>(
        sites->set_team,
        [this](std::uint32_t slot, std::uint32_t requested) noexcept {
            const auto shell = shell_team(static_cast<std::int32_t>(slot));
            const auto fixed = shell ? shell : assigned_team(static_cast<std::int32_t>(slot));
            static_cast<void>(set_team_.call(slot, fixed ? fixed : requested));
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08}));
    change_team_ = plan.hook<TransitionCharacter>(
        sites->character_change,
        [this](void *character, std::uint32_t requested) noexcept {
            const auto fixed = assigned_team(fc::read_field<std::int32_t>(character, 0x130));
            return change_team_.call(character, fixed ? fixed : requested).value_or(false);
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x57}));
    // SelectTeam also chooses the class list and spawn points from its argument.
    // Apply the campaign team here so those choices agree with the character's team.
    spawn_team_ = plan.hook<TransitionSpawnTeam>(
        sites->spawn_team,
        [this](void *display, std::uint32_t requested) noexcept {
            const auto owner = fc::read_field<void *>(display, 0x1FDC);
            // Initialize selects the preview before assigning its Character owner; this field is a local index.
            auto slot = owner ? fc::read_field<std::int32_t>(owner, 0x130) : -1;
            if (!owner) {
                const auto local_index = fc::read_field<std::uint32_t>(display, 0x1FFC);
                // The native lookup has two local-player entries and does not check its index.
                if (local_index < 2) {
                    slot = local_slot_.call(local_index).value_or(-1);
                }
            }
            const auto fixed = assigned_team(slot);
            static_cast<void>(spawn_team_.call(display, fixed ? fixed : requested));
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x57}));
}

void Transition::configure(TransitionSession session) {
    session_.store(std::make_shared<const TransitionSession>(std::move(session)), std::memory_order_release);
}

void Transition::update(std::uint32_t current_setup) noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!shell_state_.read().value_or(0) || !session || !session->active) {
        shell_session_.reset();
        return;
    }
    if (stage_.load(std::memory_order_acquire) == Stage::Prepared && battle_roles_ &&
        (battle_setup_id_ != current_setup || !battle_roles_->matches(session->players))) {
        auto expected = Stage::Prepared;
        stage_.compare_exchange_strong(expected, Stage::Idle, std::memory_order_acq_rel);
    }
    if (session == shell_session_) {
        return;
    }
    const auto local = local_slot_.call(0).value_or(-1);
    // Shell serializers call SetStartTeam for later changes; only a newly published assignment needs seeding.
    for (const auto &player : session->players) {
        if (player.slot < 64 && player.faction >= 1 && player.faction <= 2 && (session->host || player.slot == local)) {
            static_cast<void>(set_team_.call(player.slot, player.faction));
        }
    }
    shell_session_ = session;
}

fc::Result<> Transition::prepare(std::uint32_t setup_id, std::string_view faction_one, std::string_view faction_two,
                                 const std::optional<BattleRoles> &roles) {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session || !session->active) {
        return std::unexpected(fc::Error{"Join the campaign before preparing a battle."});
    }
    if (roles && !roles->matches(session->players)) {
        return std::unexpected(fc::Error{"Supply a battle role for every admitted player slot."});
    }
    const auto driver = (session->host ? host_ : join_).read().value_or(nullptr);
    if (!established(driver, session->host)) {
        return std::unexpected(fc::Error{"The native lobby has not completed its connection and file transfer setup."});
    }
    const std::array names{std::string{faction_one}, std::string{faction_two}};
    std::array<std::uint32_t, 2> sides{};
    for (std::size_t index = 0; index < names.size(); ++index) {
        sides[index] = translate_.call(names[index].c_str()).value_or(0);
    }
    if (sides[0] < 1 || sides[0] > 4 || sides[1] < 1 || sides[1] > 4 || sides[0] == sides[1]) {
        return std::unexpected(fc::Error{"The battle must name two different campaign factions."});
    }
    failure_.store(Failure::None, std::memory_order_release);
    faction_sides_ = sides;
    faction_teams_ = {};
    battle_setup_id_ = setup_id;
    battle_roles_ = roles;
    // Publish the prepared side data in one atomic step; a later disarm cannot be overwritten by another arming store.
    stage_.store(Stage::Prepared, std::memory_order_release);
    remove_unassigned();
    return {};
}

fc::Result<> Transition::check_launch(std::uint32_t current_setup) const {
    if (!battle_roles_) {
        return {};
    }
    const auto session = session_.load(std::memory_order_acquire);
    if (stage_.load(std::memory_order_acquire) != Stage::Prepared || !session || !session->active || !session->host ||
        battle_setup_id_ != current_setup || !battle_roles_->matches(session->players)) {
        return std::unexpected(fc::Error{"The battle roster changed. Prepare the battle again."});
    }
    for (const auto &player : session->players) {
        if (battle_roles_->role(player.slot) && !connected_.call(player.slot).value_or(false)) {
            return std::unexpected(fc::Error{"A player assigned to this battle disconnected. Prepare the battle again."});
        }
    }
    return {};
}

void Transition::disarm() noexcept {
    stage_.store(Stage::Idle, std::memory_order_release);
}

bool Transition::shell() const noexcept {
    return stage_.load(std::memory_order_acquire) == Stage::Shell;
}

bool Transition::mission() const noexcept {
    const auto stage = stage_.load(std::memory_order_acquire);
    return stage == Stage::Mission || stage == Stage::Returning;
}

bool Transition::host_mission() const noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    return session && session->active && session->host && stage_.load(std::memory_order_acquire) == Stage::Mission;
}

fc::Result<> Transition::check() const {
    switch (failure_.load(std::memory_order_acquire)) {
    case Failure::Allocate:
        return std::unexpected(fc::Error{"The game could not allocate its connected campaign lobby."});
    case Failure::TeamSides:
        return std::unexpected(fc::Error{"The mission's native teams do not match the campaign factions."});
    case Failure::None:
        return {};
    }
    return {};
}

bool Transition::established(void *driver, bool host) const noexcept {
    if (!driver) {
        return false;
    }
    if (host) {
        if (fc::read_field<bool>(driver, 0x18) || fc::read_field<bool>(driver, 0x141C) ||
            fc::read_field<std::int32_t>(driver, 0x158C) != -1) {
            return false;
        }
        for (std::uint32_t slot = 0; slot < 64; ++slot) {
            // files=null makes SendStateSrc's unused curFile bytes irrelevant; no file allocation may cross heaps.
            if (fc::read_field<void *>(driver, kPlayerStart + slot * kPlayerStride + 0x34)) {
                return false;
            }
        }
        return true;
    }
    return !fc::read_field<void *>(driver, 0x14) && !fc::read_field<bool>(driver, 0xA4) && fc::read_field<bool>(driver, 0xA5) &&
           !fc::read_field<bool>(driver, 0x148) && !fc::read_field<bool>(driver, 0x1B0) && fc::read_field<bool>(driver, 0x1A0) &&
           fc::read_field<std::int32_t>(driver, 0x1AC) == -1;
}

bool Transition::capture(void *driver, bool host) noexcept {
    if (stage_.load(std::memory_order_acquire) != Stage::Prepared || !established(driver, host)) {
        return false;
    }
    saved_host_ = host;
    std::memcpy(driver_.data(), driver, host ? kHostSize : kJoinSize);
    return true;
}

void Transition::resume() noexcept {
    auto *driver = allocate_.call(saved_host_ ? kHostSize : kJoinSize).value_or(nullptr);
    if (!driver) {
        failure_.store(Failure::Allocate, std::memory_order_release);
        return;
    }
    std::memcpy(driver, driver_.data(), saved_host_ ? kHostSize : kJoinSize);
    const auto now = time_.call().value_or(0.0f);
    fc::write_field(driver, 0x4, now);
    const auto update = saved_host_ ? 0x1530 : 0x150;
    // Every native mission/packet pool was reset. Old launch, boot and roster masks must not cross into shell traffic.
    fc::write_field(driver, update, false);
    fc::write_field(driver, update + 1, false);
    fc::write_field(driver, update + 2, false);
    fc::write_field<std::uint32_t>(driver, update + 4, 0);
    fc::write_field<std::uint64_t>(driver, update + 8, 0);
    fc::write_field<std::uint64_t>(driver, update + 0x18, 0);
    fc::write_field<std::uint64_t>(driver, update + 0x20, 0);
    fc::write_field<std::uint64_t>(driver, update + 0x28, 0);
    if (saved_host_) {
        std::uint64_t current{};
        std::uint32_t count{};
        for (std::uint32_t slot = 0; slot < 64; ++slot) {
            auto *player = static_cast<std::byte *>(driver) + kPlayerStart + slot * kPlayerStride;
            if (connected_.call(slot).value_or(false)) {
                current |= std::uint64_t{1} << slot;
                count += fc::read_field<bool>(player) ? 1u : 0u;
            } else {
                static_cast<void>(reset_player_.call(player));
            }
            fc::write_field<float>(driver, 0x1428 + slot * 4, 0.0f);
        }
        fc::write_field(driver, 0xC, count);
        fc::write_field(driver, 0x10, now);
        fc::write_field(driver, 0x14, false);
        fc::write_field<std::uint64_t>(driver, 0x1420, 0);
        fc::write_field(driver, 0x1528, current);
        static_cast<void>(host_.write(driver));
    } else {
        fc::write_field(driver, 0x10, false);
        fc::write_field(driver, 0x11, false);
        fc::write_field(driver, 0x1B8, now);
        static_cast<void>(join_.write(driver));
    }
    // GM_LOBBY is zero in both native host constructors. Retail ChangeRegisterGame is an empty RET stub.
    static_cast<void>(setup_mode_.write(0));
    shell_session_.reset();
    // Cancellation may arrive on the control thread during Create; never publish a successful return over it.
    auto expected = Stage::Returning;
    stage_.compare_exchange_strong(expected, Stage::Shell, std::memory_order_acq_rel);
}

void Transition::resolve_teams() noexcept {
    const auto teams = teams_.read().value_or(nullptr);
    faction_teams_ = {};
    if (teams) {
        for (std::uint32_t number = 1; number <= 2; ++number) {
            const auto team = fc::read_field<void *>(teams, number * sizeof(void *));
            if (!team) {
                continue;
            }
            const auto side = fc::read_field<std::uint32_t>(team, 0x64);
            for (std::size_t faction = 0; faction < faction_sides_.size(); ++faction) {
                if (side == faction_sides_[faction]) {
                    faction_teams_[faction] = number;
                }
            }
        }
    }
    if (!faction_teams_[0] || !faction_teams_[1] || faction_teams_[0] == faction_teams_[1]) {
        failure_.store(Failure::TeamSides, std::memory_order_release);
        return;
    }
    const auto session = session_.load(std::memory_order_acquire);
    if (session) {
        for (const auto &player : session->players) {
            const auto team = assigned_team(player.slot);
            if (team && connected_.call(player.slot).value_or(false)) {
                static_cast<void>(set_team_.call(player.slot, team));
                // Metagame rules lock teams. Mark the accepted choice so native auto-assign and requests retain it.
                static_cast<void>(team_selected_.write(true, player.slot * kNetPlayerStride));
            }
        }
    }
}

void Transition::remove_unassigned() noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session || !session->active || !session->host || (!session->admission_closed && !continuing())) {
        return;
    }
    auto *driver = host_.read().value_or(nullptr);
    if (!driver) {
        return;
    }
    std::uint64_t accepted{};
    for (const auto &player : session->players) {
        if (player.slot < 64 && player.faction >= 1 && player.faction <= 2) {
            accepted |= std::uint64_t{1} << player.slot;
        }
    }
    for (std::uint32_t slot = 0; slot < 64; ++slot) {
        if (!(accepted & (std::uint64_t{1} << slot)) && connected_.call(slot).value_or(false)) {
            static_cast<void>(drop_.call(driver, slot));
            fc::write_field(driver, 8, fc::read_field<std::uint32_t>(driver, 8) + 1);
        }
    }
}

std::uint32_t Transition::assigned_team(std::int32_t slot) const noexcept {
    if (slot < 0 || slot >= 64 || stage_.load(std::memory_order_acquire) != Stage::Mission) {
        return 0;
    }
    const auto session = session_.load(std::memory_order_acquire);
    if (session && session->active) {
        for (const auto &player : session->players) {
            if (player.slot == slot) {
                return battle_team(player, faction_teams_, battle_roles_);
            }
        }
    }
    return 0;
}

std::uint32_t Transition::shell_team(std::int32_t slot) const noexcept {
    if (slot < 0 || slot >= 64 || !shell_state_.read().value_or(0)) {
        return 0;
    }
    const auto session = session_.load(std::memory_order_acquire);
    if (session && session->active) {
        for (const auto &player : session->players) {
            if (player.slot == slot && player.faction >= 1 && player.faction <= 2) {
                return player.faction;
            }
        }
    }
    return 0;
}

bool Transition::continuing() const noexcept {
    const auto stage = stage_.load(std::memory_order_acquire);
    return stage == Stage::Prepared || stage == Stage::Mission || stage == Stage::Returning;
}

} // namespace fc_plugins::multiplayer_gc
