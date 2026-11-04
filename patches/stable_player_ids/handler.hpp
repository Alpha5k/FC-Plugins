#pragma once

#include "layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fc_plugins::stable_player_ids {

// Resolves admin UUIDs through the native Photon roster without retaining player or string pointers.
class Handler final {
  public:
    // Enables room UserID publication and replaces only the game's player admin hash calls.
    void plan(fc::Plan &plan);

  private:
    // Copies a matching nonempty UserID as UTF-8 under the native client mutex, or returns an empty string.
    std::string find_user_id(std::string_view uuid) const;

    // Uses raw UTF-8 UserID bytes when available, otherwise preserves the native UUID hash.
    void hash_player(const char *uuid, std::uint32_t length, char *output) const noexcept;

    fc::Memory<void *> rednet_;
    fc::NativeFunction<Md5> md5_;
    fc::NativeFunction<MutexOperation> lock_;
    fc::NativeFunction<MutexOperation> unlock_;
};

} // namespace fc_plugins::stable_player_ids
