#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fc_plugins::multiplayer_gc {

// Retail records have a 20-byte header and 128 UTF-16 code units; the intrusive node is not transmitted.
struct ChatRecord {
    std::uint32_t sender{};
    std::uint32_t channel{};
    std::uint64_t exclusions{};
    std::uint32_t code{UINT32_MAX};
    std::array<wchar_t, 128> text{};
};

static_assert(offsetof(ChatRecord, text) == 0x14);

} // namespace fc_plugins::multiplayer_gc
