#pragma once

#include "history.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <optional>
#include <vector>

namespace fc_plugins::hero_animation_fix {

// Identifies an observed native entry, including distinct entries within the same simulation turn.
struct PresentationEntry {
    std::int32_t turn{-1};
    std::uint32_t order{};
    std::int32_t source{-1};
    std::int32_t target{-1};
    bool authoritative{};
    friend bool operator==(const PresentationEntry &, const PresentationEntry &) = default;
};

// Copies SetWeaponComboState's presentation arguments; no weapon state or animator clocks are retained.
struct AnimationRequest {
    std::int32_t mode{};
    bool restart{};
    std::uint8_t primary{0xA4};
    std::uint8_t secondary{0xA4};
    bool full_body{};
    friend bool operator==(const AnimationRequest &, const AnimationRequest &) = default;
};

// Copies the owner's current traversal and completed lane state without reconstructing presentation from endpoints.
struct PresentationSnapshot {
    std::optional<PresentationEntry> current;
    std::optional<PresentationEntry> rendering;
    std::optional<AnimationRequest> displayed;
    AnimationRequest requested;
    std::uint8_t lanes{};
    bool retaining{};
};

// Retains observed entries, completed audio calls and selected animations for the native input horizon.
class PresentationHistory final {
  public:
    // A changed source or target replaces that occurrence and releases retained presentation.
    bool enter(const HeroIdentity &identity, const PresentationEntry &key, std::int32_t confirmation_turn = -1) noexcept {
        auto &slot = bind(identity);
        std::erase_if(slot.entries, [key](const Entry &entry) {
            return key.turn > entry.key.turn && static_cast<std::uint32_t>(key.turn) - entry.key.turn >= 32;
        });
        slot.current = key;
        for (auto &entry : slot.entries) {
            if (entry.key.turn == key.turn && entry.key.order == key.order && entry.key.authoritative == key.authoritative) {
                if (entry.key == key) {
                    const bool recorded_frontier = entry.confirmation_turn >= 0;
                    if (confirmation_turn >= 0) {
                        entry.confirmation_turn = confirmation_turn;
                    }
                    return recorded_frontier;
                }
                entry = {.key = key, .confirmation_turn = confirmation_turn};
                slot.retaining = false;
                return false;
            }
        }
        slot.retaining = false;
        try {
            slot.entries.push_back({.key = key, .confirmation_turn = confirmation_turn});
        } catch (const std::bad_alloc &) {
            // Unrecorded entries remain native and cannot suppress a later request.
        }
        return false;
    }

    // Adjacent authority samples continue an entry only when the native mode clock advances by one retail turn.
    // Every native EnterState branch resets that clock; gaps and phase changes cannot establish continuation.
    std::optional<PresentationEntry> authority_entry(const HeroIdentity &identity, std::int32_t turn, std::int32_t source,
                                                     std::int32_t target, std::int32_t mode, float age, bool native_entry,
                                                     float turn_duration,
                                                     std::optional<PresentationEntry> matched = std::nullopt) noexcept {
        auto &slot = bind(identity);
        const auto &previous = slot.authoritative;
        const bool continuing = previous.entry && previous.turn >= 0 && turn == previous.turn + 1 && previous.target == target &&
                                previous.mode == mode && previous.age > 0.0F && age == previous.age + turn_duration;
        auto entry = matched ? matched : continuing ? previous.entry : std::nullopt;
        if (!entry && native_entry) {
            entry =
                PresentationEntry{.turn = turn, .order = slot.authority_order++, .source = source, .target = target, .authoritative = true};
            enter(identity, *entry);
        }
        slot.authoritative = {.turn = turn, .target = target, .mode = mode, .age = age, .entry = entry};
        return entry;
    }

    // Requires one observed authority-prefix occurrence; assets and combo IDs alone are insufficient.
    std::optional<PresentationEntry> confirmed(const HeroIdentity &identity, std::int32_t turn, std::int32_t state,
                                               std::uint32_t *cardinality = nullptr) noexcept {
        auto &slot = bind(identity);
        std::optional<PresentationEntry> match;
        std::uint32_t count{};
        for (const auto &entry : slot.entries) {
            if (entry.confirmation_turn == turn && entry.key.target == state) {
                ++count;
                match = entry.key;
            }
        }
        if (cardinality != nullptr) {
            *cardinality = count;
        }
        return count == 1 ? match : std::nullopt;
    }

    // Reads exact owner tokens and lane completion for an already bound identity.
    PresentationSnapshot snapshot(const HeroIdentity &identity) const noexcept {
        if (!identity.valid()) {
            return {};
        }
        const auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        if (slot.identity != identity) {
            return {};
        }
        std::uint8_t lanes{};
        for (const auto &entry : slot.entries) {
            if (slot.current && entry.key == *slot.current) {
                lanes = entry.lanes;
                break;
            }
        }
        return {.current = slot.current,
                .rendering = slot.rendering,
                .displayed = slot.displayed,
                .requested = slot.requested,
                .lanes = lanes,
                .retaining = slot.retaining};
    }

    // Keeps newer historical presentation or an exact native confirmation; corrections release retention.
    void authority(const HeroIdentity &identity, AuthorityAction action,
                   std::optional<PresentationEntry> confirmed_entry = std::nullopt) noexcept {
        auto &slot = bind(identity);
        slot.retaining = action == AuthorityAction::Historical && slot.displayed.has_value();
        // Authority-only tokens own completed audio; only observed prediction entries participate in animation retention.
        slot.current =
            action == AuthorityAction::Native && confirmed_entry && !confirmed_entry->authoritative ? confirmed_entry : std::nullopt;
        slot.order_turn = -1;
        slot.order = 0;
    }

    // Gives each entry a prediction-turn ordinal, reset by the next authority read.
    std::uint32_t next_order(const HeroIdentity &identity, std::int32_t turn) noexcept {
        auto &slot = bind(identity);
        if (slot.order_turn != turn) {
            slot.order_turn = turn;
            slot.order = 0;
        }
        return slot.order++;
    }

    // Tests completed calls from this exact entry and audio leaf occurrence.
    bool audio_presented(const HeroIdentity &identity, const PresentationEntry &key, std::uint8_t kind, std::uint32_t order,
                         std::uint64_t variant) noexcept {
        const auto *entry = find(bind(identity), key);
        return entry != nullptr && std::ranges::any_of(entry->sounds, [=](const Sound &sound) {
                   return sound.kind == kind && sound.order == order && sound.variant == variant;
               });
    }

    // Records a leaf after its native call completes; pending or failed calls remain eligible.
    void present_audio(const HeroIdentity &identity, const PresentationEntry &key, std::uint8_t kind, std::uint32_t order,
                       std::uint64_t variant) noexcept {
        auto *entry = find(bind(identity), key);
        if (entry == nullptr || audio_presented(identity, key, kind, order, variant)) {
            return;
        }
        try {
            entry->sounds.push_back({.kind = kind, .order = order, .variant = variant});
        } catch (const std::bad_alloc &) {
            // Unknown audio occurrences follow native behavior on the next traversal.
        }
    }

    // Uses native presentation unless a confirmed prefix or completed replay would restart it.
    AnimationRequest animation(const HeroIdentity &identity, AnimationRequest request) noexcept {
        auto &slot = bind(identity);
        auto *entry = slot.current ? find(slot, *slot.current) : nullptr;
        if (entry != nullptr && entry->request) {
            auto previous = *entry->request;
            previous.restart = request.restart;
            if (previous != request) {
                // A corrected native request can change animation or full-body behavior without changing combo ID.
                entry->lanes = 0;
                slot.retaining = false;
            }
        }
        // A tracked pending request must reach its selectors even while an older selected request is retained.
        if (slot.displayed && ((slot.retaining && entry == nullptr) || (entry != nullptr && entry->lanes == 3))) {
            request = *slot.displayed;
            request.restart = false;
            slot.rendering.reset();
        } else {
            slot.rendering = slot.current;
            if (entry != nullptr) {
                entry->request = request;
            }
            if (request.mode == 0) {
                // Native idle ends the combo presentation even when no combo descriptor is selected.
                slot.displayed.reset();
            }
        }
        slot.requested = request;
        return request;
    }

    // Records a lane after native selection installs or keeps the requested clip.
    void present_animation(const HeroIdentity &identity, std::uint8_t lane, bool consumed) noexcept {
        auto &slot = bind(identity);
        if (!consumed || !slot.rendering) {
            return;
        }
        auto *entry = find(slot, *slot.rendering);
        if (entry != nullptr) {
            entry->lanes |= lane;
            if (entry->lanes == 3) {
                slot.displayed = slot.requested;
            }
        }
    }

    // Resets traversal ordinals without treating pending requests as presented.
    void rewind(std::uintptr_t weapon) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.weapon == weapon) {
                slot.order_turn = -1;
                slot.order = 0;
            }
        }
    }

    // Retires the native lifetime on selection reset, destruction or failed calls.
    void clear_weapon(std::uintptr_t weapon) noexcept {
        for (auto &slot : slots_) {
            if (slot.identity.weapon == weapon) {
                slot = {};
            }
        }
    }

    // Retires all borrowed identities at the FC session boundary.
    void clear_all() noexcept {
        slots_ = {};
    }

  private:
    struct Sound {
        std::uint8_t kind{};
        std::uint32_t order{};
        std::uint64_t variant{};
    };

    struct Entry {
        PresentationEntry key;
        // Local input-frontier or remote update-frontier coordinate used only by authority-prefix matching.
        std::int32_t confirmation_turn{-1};
        std::vector<Sound> sounds;
        std::optional<AnimationRequest> request;
        std::uint8_t lanes{};
    };

    struct Slot {
        struct Authoritative {
            std::int32_t turn{-1};
            std::int32_t target{-1};
            std::int32_t mode{-1};
            float age{};
            std::optional<PresentationEntry> entry;
        };

        HeroIdentity identity;
        std::vector<Entry> entries;
        std::optional<PresentationEntry> current;
        std::optional<PresentationEntry> rendering;
        std::optional<AnimationRequest> displayed;
        AnimationRequest requested;
        std::int32_t order_turn{-1};
        std::uint32_t order{};
        // Native authority calls retain independent ordinals across reader/prediction traversal resets.
        std::uint32_t authority_order{};
        Authoritative authoritative;
        bool retaining{};
    };

    // Replaces a player's presentation when its selected weapon or owner changes.
    Slot &bind(const HeroIdentity &identity) noexcept {
        auto &slot = slots_[static_cast<std::size_t>(identity.player_id)];
        if (slot.identity != identity) {
            slot = {.identity = identity};
        }
        return slot;
    }

    // Finds the exact observed entry without a state-only fallback.
    static Entry *find(Slot &slot, const PresentationEntry &key) noexcept {
        const auto entry = std::ranges::find(slot.entries, key, &Entry::key);
        return entry == slot.entries.end() ? nullptr : &*entry;
    }

    std::array<Slot, kNetworkPlayerCount> slots_{};
};

} // namespace fc_plugins::hero_animation_fix
