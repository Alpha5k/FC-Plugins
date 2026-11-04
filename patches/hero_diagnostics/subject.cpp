#include "subject.hpp"

#include <cstddef>

namespace fc_plugins::hero_diagnostics {

void SubjectTable::set_joystick_index(fc::NativeFunction<GetJoystickIndex> function) noexcept {
    get_joystick_index_ = function;
}

HeroSubject *SubjectTable::bind(void *weapon) noexcept {
    if (weapon == nullptr) {
        return nullptr;
    }

    // Player-owned melee supplies a Controllable owner and combo definition; only slots in the table's range are tracked.
    auto *owner = native::read<void *>(weapon, native::kWeaponOwnerOffset);
    auto *combo = native::read<void *>(weapon, native::kComboOffset);
    if (owner == nullptr || combo == nullptr) {
        return nullptr;
    }

    const auto player = native::read<std::int32_t>(owner, native::kPlayerOffset);
    if (player < 0 || player >= static_cast<std::int32_t>(subjects_.size())) {
        return nullptr;
    }

    // Weapon ownership points at the Soldier's Controllable subobject rather than the complete EntitySoldier.
    auto *soldier = static_cast<std::byte *>(owner) - native::kControllableOffset;
    auto *animator = role_ == fc::Role::Client ? native::read<void *>(soldier, native::kSoldierAnimatorOffset) : nullptr;
    if (role_ == fc::Role::Client && animator == nullptr) {
        return nullptr;
    }

    std::uint8_t local_player = trace::kNoLocalPlayer;
    if (role_ == fc::Role::Client) {
        const auto local = get_joystick_index_.call(player);
        if (local && *local >= 0 && *local < 2) {
            local_player = static_cast<std::uint8_t>(*local);
        }
    }

    auto &subject = subjects_[static_cast<std::size_t>(player)];
    const bool changed = subject.weapon != weapon || subject.owner != owner || subject.combo != combo || subject.soldier != soldier ||
                         subject.animator != animator || subject.local_player != local_player;
    if (changed) {
        // Pointer or local-controller changes start a new correlation identity and clear cached samples.
        const auto generation = subject.generation + 1;
        subject = {
            .weapon = weapon,
            .owner = owner,
            .combo = combo,
            .soldier = soldier,
            .animator = animator,
            .id = ++next_id_,
            .generation = generation,
            .player = static_cast<std::int16_t>(player),
            .local_player = local_player,
        };
    }

    return current(subject) ? &subject : nullptr;
}

HeroSubject *SubjectTable::find_weapon(const void *weapon) noexcept {
    for (auto &subject : subjects_) {
        if (subject.weapon == weapon && current(subject)) {
            return &subject;
        }
    }

    return nullptr;
}

HeroSubject *SubjectTable::find_player(std::int32_t player) noexcept {
    if (player < 0 || player >= static_cast<std::int32_t>(subjects_.size())) {
        return nullptr;
    }

    auto &subject = subjects_[static_cast<std::size_t>(player)];
    return current(subject) ? &subject : nullptr;
}

HeroSubject *SubjectTable::find_animator(const void *animator) noexcept {
    if (animator == nullptr) {
        return nullptr;
    }

    for (auto &subject : subjects_) {
        if (subject.animator == animator && current(subject)) {
            return &subject;
        }
    }

    return nullptr;
}

HeroSubject *SubjectTable::find_soldier(const void *soldier) noexcept {
    for (auto &subject : subjects_) {
        if (subject.soldier == soldier && current(subject)) {
            return &subject;
        }
    }

    return nullptr;
}

bool SubjectTable::current(const HeroSubject &subject) const noexcept {
    if (subject.weapon == nullptr || subject.owner == nullptr || subject.combo == nullptr || subject.soldier == nullptr ||
        native::read<void *>(subject.weapon, native::kWeaponOwnerOffset) != subject.owner ||
        native::read<void *>(subject.weapon, native::kComboOffset) != subject.combo ||
        native::read<std::int32_t>(subject.owner, native::kPlayerOffset) != subject.player) {
        return false;
    }

    // Server subjects require matching ownership but omit client alive, selection, and animator checks.
    if (role_ == fc::Role::Server) {
        return true;
    }

    const auto local = get_joystick_index_.call(subject.player);
    const auto local_player = local && *local >= 0 && *local < 2 ? static_cast<std::uint8_t>(*local) : trace::kNoLocalPlayer;
    if (local_player != subject.local_player) {
        return false;
    }

    // Client records require the alive bit and the native packed active selection.
    const bool alive = (native::read<std::uint8_t>(subject.soldier, native::kAliveFlagsOffset) & native::kAliveFlag) != 0;
    return subject.animator != nullptr && native::read<void *>(subject.soldier, native::kSoldierAnimatorOffset) == subject.animator &&
           alive && selected_weapon(subject) == subject.weapon;
}

void SubjectTable::clear_input_origins() noexcept {
    for (auto &subject : subjects_) {
        subject.input_turn = -1;
        subject.input_origin = trace::InputOrigin::Unknown;
    }
}

void SubjectTable::retire_weapon(const void *weapon) noexcept {
    for (auto &subject : subjects_) {
        if (subject.weapon == weapon) {
            const auto generation = subject.generation;
            subject = {};
            subject.generation = generation;
        }
    }
}

void SubjectTable::retire_soldier(const void *soldier) noexcept {
    for (auto &subject : subjects_) {
        if (subject.soldier == soldier) {
            const auto generation = subject.generation;
            subject = {};
            subject.generation = generation;
        }
    }
}

bool SubjectTable::live(const HeroSubject &subject) const noexcept {
    if (subject.player < 0 || subject.player >= static_cast<std::int32_t>(subjects_.size())) {
        return false;
    }

    const auto &entry = subjects_[static_cast<std::size_t>(subject.player)];
    return subject.id != 0 && entry.id == subject.id && entry.generation == subject.generation && current(subject);
}

void SubjectTable::reset() noexcept {
    subjects_ = {};
    next_id_ = 0;
}

void *SubjectTable::selected_weapon(const HeroSubject &subject) const noexcept {
    // Sign-extend the active slot's low nibble; native consumers treat negative values as no active weapon.
    const auto packed = native::read<std::uint8_t>(subject.soldier, native::kPackedSelectionOffset);
    auto index = static_cast<std::int32_t>(packed & native::kPackedWeaponIndexMask);
    if ((index & native::kPackedWeaponSign) != 0) {
        index -= native::kPackedWeaponRange;
    }

    return index >= 0 && index < native::kWeaponSlots
               ? native::read<void *>(subject.soldier, native::kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void *))
               : nullptr;
}

} // namespace fc_plugins::hero_diagnostics
