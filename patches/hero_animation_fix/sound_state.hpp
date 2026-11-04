#pragma once

#include <fc/native.hpp>

#include <cstdint>

namespace fc_plugins::hero_animation_fix {

// Captures the active ambient assignment before the retail secondary FireAudio empty-source copy.
struct AmbientAssignment {
    void *properties{};
    std::uint32_t descriptor{};
    std::uint8_t type{};
    std::uint16_t controller{};
    std::uint8_t controller_flags{};
    std::uint8_t desired_mask{};
    bool qualified{};

    // Caller qualifies a melee object before reading the melee flags and its property wrappers.
    static AmbientAssignment capture(const void *weapon, bool secondary) noexcept {
        AmbientAssignment state;
        if (!secondary) {
            return state;
        }
        state.properties = fc::read_field<void *>(weapon, 0x64);
        if (state.properties == nullptr) {
            return state;
        }
        state.descriptor = fc::read_field<std::uint32_t>(weapon, 0xD8);
        state.type = fc::read_field<std::uint8_t>(weapon, 0xDC);
        state.controller = fc::read_field<std::uint16_t>(weapon, 0xCC);
        state.controller_flags = fc::read_field<std::uint8_t>(weapon, 0xCE);
        state.desired_mask = fc::read_field<std::uint8_t>(weapon, 0x17C) & 0xF0U;
        state.qualified = state.descriptor != 0 && state.type == 1 && (state.controller_flags & 1U) != 0 &&
                          (fc::read_field<std::uint8_t>(weapon, 0x179) & 2U) != 0 && state.desired_mask != 0 && state.intended() &&
                          state.empty_source();
        return state;
    }

    // Restores pointer/type alone when the empty assignment leaves the same active controller and intended ambient.
    bool restore(void *weapon) const noexcept {
        if (!qualified || fc::read_field<void *>(weapon, 0x64) != properties || !intended() || !empty_source() ||
            fc::read_field<std::uint32_t>(weapon, 0xD8) != 0 || fc::read_field<std::uint8_t>(weapon, 0xDC) != 0 ||
            fc::read_field<std::uint16_t>(weapon, 0xCC) != controller || fc::read_field<std::uint8_t>(weapon, 0xCE) != controller_flags ||
            (fc::read_field<std::uint8_t>(weapon, 0x179) & 2U) == 0 ||
            (fc::read_field<std::uint8_t>(weapon, 0x17C) & 0xF0U) != desired_mask) {
            return false;
        }
        fc::write_field(weapon, 0xD8, descriptor);
        fc::write_field(weapon, 0xDC, type);
        return true;
    }

  private:
    // UpdateEffects' ordinary ambient source is the property wrapper at +264 in both retail images.
    bool intended() const noexcept {
        return fc::read_field<std::uint32_t>(properties, 0x264) == descriptor && fc::read_field<std::uint8_t>(properties, 0x268) == type;
    }

    // FireAudio assigns property +120 to weapon +D8, including the descriptor's type byte.
    bool empty_source() const noexcept {
        return fc::read_field<std::uint32_t>(properties, 0x120) == 0 && fc::read_field<std::uint8_t>(properties, 0x124) == 0;
    }
};

} // namespace fc_plugins::hero_animation_fix
