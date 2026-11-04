#pragma once

#include <fc/native.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fc_plugins::door_corpse_fix {

struct GameObject;

// The query passes six stack arguments and returns up to capacity borrowed GameObject pointers.
// Center and radius use game world-coordinate units; the door consumes the pointers during this update.
using ObjectQuery = std::int32_t(__cdecl *)(const float *center, float radius, GameObject **objects, std::int32_t capacity,
                                            const GameObject *excluded_object, std::int32_t collision_type) noexcept;

// GetDerivedRtti is vtable slot 1; matching its code address identifies the exact soldier type.
inline constexpr std::size_t kDerivedRttiSlot = 1;
// Damageable's flags at Soldier + 0x140 + 0xBC, read only for the exact soldier getter.
inline constexpr std::size_t kAliveFlagsOffset = 0x1FC;
// Damageable::mIsAlive is bit 3 of that flags byte.
inline constexpr std::uint8_t kAliveFlag = 0x08;

// Identifies the one door query, its callee and the soldier type getter in a selected image.
struct Layout {
    fc::CallLocation<ObjectQuery> door_query;
    fc::CodeLocation query_cleanup;

    fc::CodeLocation soldier_rtti_getter;
};

// Returns the native layout for an exact image profile, or nullopt if unsupported.
std::optional<Layout> select_layout(std::string_view profile);

} // namespace fc_plugins::door_corpse_fix
