#pragma once

#include "layout.hpp"
#include "schema.hpp"

#include <fc/plugin.hpp>
#include <fc/native.hpp>

#include <array>
#include <cstdint>

namespace fc_plugins::hero_diagnostics {

// Correlates one player-slot entry using borrowed game objects and a diagnostic generation.
struct HeroSubject {
    void *weapon{};
    void *owner{};
    void *combo{};
    void *soldier{};
    void *animator{};

    std::uint32_t id{};
    std::uint32_t generation{};

    // Native player slot, independent of transport peer IDs.
    std::int16_t player{-1};
    std::uint8_t local_player{trace::kNoLocalPlayer};

    // Queue acceptance permits dependent records; later output loss can still remove the Subject record.
    bool announced{};
    bool asset_announced{};
    std::uint32_t simulation_scope{};
    std::uint32_t pose_scope{};

    native::PlayerMove last_move{};
    native::PlayerMove last_consumed_move{};

    // Qualified IsTurnReady publication/fill markers; late replay invalidates this attribution.
    std::int32_t input_turn{-1};
    trace::InputOrigin input_origin{};

    std::array<std::array<float, 3>, 2> last_display_position{};
    std::array<bool, 2> display_seen{};
    bool move_seen{};
    bool consumed_move_seen{};
};

// Stores one melee identity per native player slot; address comparisons cannot establish allocation lifetime.
// Callers serialize access and keep sampled game objects alive for each call.
class SubjectTable final {
  public:
    explicit SubjectTable(fc::Role role) noexcept : role_(role) {}

    // Supplies the client helper used to distinguish local player slots from remote players.
    void set_joystick_index(fc::NativeFunction<GetJoystickIndex> function) noexcept;

    // Binds a live borrowed melee weapon and starts a new ID/generation when its object tuple changes.
    HeroSubject *bind(void *weapon) noexcept;

    // Lookups recheck ownership and selection; native reset/destruction observers retire cached objects.
    HeroSubject *find_weapon(const void *weapon) noexcept;
    HeroSubject *find_player(std::int32_t player) noexcept;
    HeroSubject *find_animator(const void *animator) noexcept;
    HeroSubject *find_soldier(const void *soldier) noexcept;

    // Visits current selected local identities without binding additional game objects.
    template <class Callback> void for_each_local(Callback &&callback) noexcept {
        for (auto &subject : subjects_) {
            if (subject.local_player != trace::kNoLocalPlayer && current(subject)) {
                callback(subject);
            }
        }
    }

    // Clears exact server input attribution when native late replay selects another history cursor.
    void clear_input_origins() noexcept;

    // Checks cached ownership; client subjects also require a matching animator, alive bit, and selected weapon.
    bool current(const HeroSubject &subject) const noexcept;

    // Retires matching identities before native reset or destruction without dereferencing cached objects.
    void retire_weapon(const void *weapon) noexcept;
    void retire_soldier(const void *soldier) noexcept;

    // Checks a saved invocation identity before any borrowed native field is sampled again.
    bool live(const HeroSubject &subject) const noexcept;

    // Clears identities and sample history before native observations start.
    void reset() noexcept;

  private:
    // Resolves the packed active slot; a negative value means no active weapon.
    void *selected_weapon(const HeroSubject &subject) const noexcept;

    fc::Role role_{};
    fc::NativeFunction<GetJoystickIndex> get_joystick_index_{};

    std::array<HeroSubject, native::kPlayerCapacity> subjects_{};
    std::uint32_t next_id_{};
};

} // namespace fc_plugins::hero_diagnostics
