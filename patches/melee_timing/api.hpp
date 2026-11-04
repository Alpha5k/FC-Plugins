#pragma once

#include <fc/patch_api.hpp>

#include <cstdint>
#include <string_view>

namespace fc_plugins::melee_timing {

// Precise elapsed seconds for occupied native inputs, in logical queue order.
struct QueueAges {
    double values[8]{};
};

// Describes the eight ages copied synchronously between queue, packet and checkpoint owners.
inline fc::RecordDescription fc_description(std::type_identity<QueueAges>) {
    return fc::describe<QueueAges>("QueueAges.v1", &QueueAges::values);
}

// Methods run on the native weapon thread; the game owns each borrowed weapon pointer.
struct QueueTimingApi {
    static constexpr std::string_view id = "QueueTimingApi.v1";

    // Returns 1 when this precise gameplay correction agrees with the peer, otherwise 0.
    fc::Function<std::uint32_t(std::uint64_t peer) noexcept> compatible;

    // Copies existing ages despite later agreement changes; returns 0 if no record exists and precision is inactive.
    fc::Function<std::uint32_t(void *weapon, QueueAges *ages) noexcept> capture;
    // Restores ages after native rows; nullptr retires precise timing when a packet or checkpoint has no ages.
    fc::Function<void(void *weapon, const QueueAges *ages) noexcept> restore;
};

// Requires the agreement query and both synchronous queue methods with the same age representation.
inline fc::ApiContract<QueueTimingApi> api_contract() {
    return fc::api<QueueTimingApi>(QueueTimingApi::id, &QueueTimingApi::compatible, &QueueTimingApi::capture, &QueueTimingApi::restore);
}

} // namespace fc_plugins::melee_timing
