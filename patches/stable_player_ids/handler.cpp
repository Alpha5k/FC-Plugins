#include "handler.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include <Windows.h>

namespace fc_plugins::stable_player_ids {
namespace {

// Releases a successfully acquired native mutex, including when copying its protected strings throws.
class MutexGuard final {
  public:
    MutexGuard(void *mutex, const fc::NativeFunction<MutexOperation> &unlock) noexcept : mutex_(mutex), unlock_(unlock) {}

    MutexGuard(const MutexGuard &) = delete;
    MutexGuard &operator=(const MutexGuard &) = delete;

    // Releases the same embedded mutex passed to the native lock operation.
    ~MutexGuard() {
        static_cast<void>(unlock_.call(mutex_));
    }

  private:
    void *mutex_;
    const fc::NativeFunction<MutexOperation> &unlock_;
};

// Compares the full native UTF-16 player name with the ASCII UUID supplied to the admin hash call.
bool matches_name(const void *player, std::string_view uuid) noexcept {
    const auto *name = static_cast<const std::byte *>(player) + kPlayerNameOffset;
    const auto length = fc::read_field<std::uint32_t>(name, kStringLengthOffset);
    const auto *data = fc::read_field<const wchar_t *>(name, kStringDataOffset);
    if (data == nullptr || length == 0 || length != uuid.size()) {
        return false;
    }

    for (std::size_t index = 0; index < uuid.size(); ++index) {
        if (data[index] != static_cast<unsigned char>(uuid[index])) {
            return false;
        }
    }
    return true;
}

// Converts all code units of the protected UserID JString, preserving case and embedded nulls.
std::string copy_user_id(const void *player) {
    const auto *user_id = static_cast<const std::byte *>(player) + kPlayerUserIdOffset;
    const auto length = fc::read_field<std::uint32_t>(user_id, kStringLengthOffset);
    const auto *data = fc::read_field<const wchar_t *>(user_id, kStringDataOffset);
    if (data == nullptr || length == 0 || length > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    // Windows conversion accepts an int code-unit count; strict conversion leaves malformed UTF-16 on the fallback path.
    const auto wide_length = static_cast<int>(length);
    const auto byte_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, wide_length, nullptr, 0, nullptr, nullptr);
    if (byte_count <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(byte_count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, wide_length, result.data(), byte_count, nullptr, nullptr) != byte_count) {
        return {};
    }
    return result;
}

} // namespace

void Handler::plan(fc::Plan &plan) {
    auto game = plan.for_image("game");
    const auto game_layout = select_game_layout(game.profile());
    if (!game_layout) {
        game.fail(std::format("No native layout for image profile '{}'", game.profile()));
        return;
    }

    auto bootstrap = plan.for_image("bootstrap");
    const auto bootstrap_layout = select_bootstrap_layout(bootstrap.profile());
    if (!bootstrap_layout) {
        bootstrap.fail(std::format("No native layout for image profile '{}'", bootstrap.profile()));
        return;
    }

    md5_ = game.function(game_layout->md5);
    rednet_ = bootstrap.access(bootstrap_layout->rednet);
    lock_ = bootstrap.function(bootstrap_layout->lock);
    unlock_ = bootstrap.function(bootstrap_layout->unlock);
    bootstrap.write(bootstrap_layout->publish_user_id, fc::byte_array<0xB2, 0x01>());

    const auto hash = [this](const char *uuid, std::uint32_t length, char *output) noexcept {
        hash_player(uuid, length, output);
    };
    for (const auto &site : game_layout->admin_hashes) {
        game.hook_call(site, hash);
    }
}

std::string Handler::find_user_id(std::string_view uuid) const {
    const auto rednet = rednet_.read();
    if (!rednet || *rednet == nullptr) {
        return {};
    }
    const auto *client = fc::read_field<const std::byte *>(*rednet, kRedNetClientOffset);
    if (client == nullptr) {
        return {};
    }

    // Admin calls run on the client's game owner thread; the mutex excludes background Photon service mutation.
    // The mutex is embedded storage, and client lifetime extends through this game-thread call.
    auto *mutex = const_cast<std::byte *>(client) + kClientMutexOffset;
    const auto locked = lock_.call(mutex);
    if (!locked || *locked != 0) {
        return {};
    }
    const MutexGuard guard{mutex, unlock_};

    const auto *photon = fc::read_field<const void *>(client, kPhotonClientOffset);
    if (photon == nullptr) {
        return {};
    }
    const auto *room = fc::read_field<const void *>(photon, kCurrentRoomOffset);
    if (room == nullptr) {
        return {};
    }
    const auto count = fc::read_field<std::uint32_t>(room, kPlayerCountOffset);
    const auto *players = fc::read_field<const void *const *>(room, kPlayersOffset);
    if (players == nullptr) {
        return {};
    }

    // Photon names contain the same formatted UUID used by the admin command; UserIDs remain opaque account text.
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto *player = fc::read_field<const void *>(players, static_cast<std::ptrdiff_t>(index) * sizeof(void *));
        if (player != nullptr && matches_name(player, uuid)) {
            return copy_user_id(player);
        }
    }
    return {};
}

void Handler::hash_player(const char *uuid, std::uint32_t length, char *output) const noexcept {
    try {
        const auto user_id = find_user_id(std::string_view{uuid, length});
        if (!user_id.empty()) {
            // The copy outlives the mutex guard; native MD5 sees the complete UTF-8 byte count without a prefix.
            static_cast<void>(md5_.call(user_id.data(), static_cast<std::uint32_t>(user_id.size()), output));
            return;
        }
    } catch (...) {
        // String allocation failures preserve the native hash and cannot escape this native callback.
    }
    static_cast<void>(md5_.call(uuid, length, output));
}

} // namespace fc_plugins::stable_player_ids
