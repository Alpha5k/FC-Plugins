#pragma once

#include <fc/patch_api.hpp>

#include <cstdint>
#include <string_view>

namespace fc_plugins::hero_animation_fix {

// The compatible packet owner calls this after native owner decoding, selection and melee phase restoration.
struct ReplayApi {
    static constexpr std::string_view id = "HeroReplayApi.v1";

    // Captures the selected ready melee weapon at the received turn in the existing replay history.
    fc::Function<void(void *soldier, std::int32_t turn) noexcept> received;
};

// Describes the synchronous completed-owner-read notification.
inline fc::ApiContract<ReplayApi> api_contract() {
    return fc::api<ReplayApi>(ReplayApi::id, &ReplayApi::received);
}

} // namespace fc_plugins::hero_animation_fix
