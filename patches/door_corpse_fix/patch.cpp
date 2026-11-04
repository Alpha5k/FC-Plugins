#include "patch.hpp"

#include "layout.hpp"

#include <cstddef>
#include <format>
#include <cstdint>
#include <expected>

namespace fc_plugins::door_corpse_fix {
namespace {

// Removes dead soldiers from the occupancy results that keep a networked door open.
class Handler {
  public:
    // Checks the door's query contract and retains the soldier getter address for type comparison.
    void plan(fc::Plan &plan) {
        const auto layout = select_layout(plan.profile());
        if (!layout) {
            plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
            return;
        }

        plan.check(layout->query_cleanup, 3);

        soldier_getter_ = plan.access<const std::byte>(plan.at(layout->soldier_rtti_getter.rva, layout->soldier_rtti_getter.name), 1);

        // EntityDoor uses this query during the network client-world pass, including a host's client world.
        query_ = plan.hook_call(layout->door_query, [this](const float *center, float radius, GameObject **objects, std::int32_t capacity,
                                                           const GameObject *excluded_object, std::int32_t collision_type) noexcept {
            return query(center, radius, objects, capacity, excluded_object, collision_type);
        });
    }

    // Resolves the getter's prepared code address for exact comparison with each object's vtable slot.
    fc::Result<> prepare(fc::PrepareContext &) {
        const auto code = soldier_getter_.view();
        if (!code) {
            return std::unexpected(code.error());
        }

        soldier_getter_address_ = code->data();
        return {};
    }

  private:
    // Checks the alive flag after confirming the object uses the soldier type getter.
    bool is_dead_soldier(const GameObject *object) const noexcept {
        const auto *vtable = fc::read_field<const std::byte *>(object);
        const auto *getter = fc::read_field<const std::byte *>(vtable, kDerivedRttiSlot * sizeof(const std::byte *));
        if (getter != soldier_getter_address_) {
            return false;
        }

        // Damageable's alive bit is cleared when the native health callback kills the soldier.
        return (fc::read_field<std::uint8_t>(object, kAliveFlagsOffset) & kAliveFlag) == 0;
    }

    // Calls the native query, removes dead soldiers while preserving order, and returns the remaining count.
    std::int32_t query(const float *center, float radius, GameObject **objects, std::int32_t capacity, const GameObject *excluded_object,
                       std::int32_t collision_type) const noexcept {
        const auto count = query_.call(center, radius, objects, capacity, excluded_object, collision_type).value_or(0);

        // The door consumes these borrowed pointers immediately. The query stops at capacity (20 here),
        // so filtering its returned list can also hide living blockers beyond that cutoff.
        std::int32_t accepted{};
        for (std::int32_t index = 0; index < count; ++index) {
            if (!is_dead_soldier(objects[index])) {
                objects[accepted++] = objects[index];
            }
        }

        return accepted;
    }

    fc::NativeFunction<ObjectQuery> query_;

    fc::Memory<const std::byte> soldier_getter_;
    // Prepared getter code address, compared with vtable entries rather than a returned RTTI value.
    const std::byte *soldier_getter_address_{};
};

} // namespace

fc::Patch definition() {
    return fc::Patch{.id = "DoorCorpseFix",
                     .description = "Fixes the bug where corpses can hold doors open on multiplayer clients.",
                     .category = "Multiplayer",
                     .enabled = true,
                     .variants = {fc::variant<Handler>({"BF2.Steam.59EDE353", "BF2.GOG.59EDF52B"}, fc::Role::Client)}};
}

} // namespace fc_plugins::door_corpse_fix
