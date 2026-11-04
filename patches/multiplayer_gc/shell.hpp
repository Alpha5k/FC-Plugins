#pragma once

#include "protocol.hpp"
#include "layout.hpp"

#include <fc/native.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fc_plugins::multiplayer_gc {

// Copies accepted native-slot associations from the control thread to shell callbacks.
struct ShellSession {
    bool active{};
    bool host{};
    std::uint8_t local_slot{};
    std::uint64_t connected_slots{};
    std::uint64_t capture_slots{};
    std::vector<Player> players;
};

// Held actions from player zero's configured input bindings. Reading leaves them available to the game.
struct ShellInput {
    bool player_list{};
    bool consumed{};
    bool chat_open{};
};

using WriteMove = void(__cdecl *)(void *, void *, void *) noexcept;
using ReadMove = bool(__cdecl *)(void *, std::uint32_t, void *, void *) noexcept;
using WriteUpdate = void(__cdecl *)(void *, std::uint32_t, void *, void *) noexcept;
using ReadUpdate = bool(__cdecl *)(void *, void *, void *) noexcept;
using QueueChat = void(__cdecl *)(std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t, const wchar_t *, std::uint32_t) noexcept;
using ChatCodec = void(__cdecl *)(void *) noexcept;
using HostSendUpdates = fc::NativeCall<void(void *) noexcept, fc::abi::Ecx>;
using DisplayChat = void(__cdecl *)(const void *) noexcept;
using PlayerName = const char *(__cdecl *)(std::uint32_t) noexcept;
using LocalSlot = std::int32_t(__cdecl *)(std::uint32_t) noexcept;
using ResolvePacket = fc::NativeCall<void *(std::int32_t) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
using RemoveNode =
    fc::NativeCall<void(void *, void *) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using FreeRecord = void(__cdecl *)(void *) noexcept;
using ReadBytes = fc::NativeCall<
    void(void *, void *, std::uint32_t) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

using KeyboardEvent = void(__cdecl *)(std::uint32_t, void *, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
using CopyName = std::uint32_t(__cdecl *)(wchar_t *, const char *, std::uint32_t) noexcept;

// Maps stock KEYCHAR events to the original editor's repeat counters before its input update.
inline void prepare_chat_repeat(void *controller, std::span<const std::uint32_t> keys, std::span<const std::uint8_t> event_keys,
                                std::span<const bool> events) noexcept {
    // The stock editor produces a repeat edge when its held-key counter increments past ten.
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto event_key = event_keys[static_cast<std::uint8_t>(keys[index])];
        fc::write_field<std::uint8_t>(controller, 0x27EB + index, events[event_key] ? 10 : 0);
    }
}

using ChatInput = fc::NativeCall<void(void *) noexcept, fc::abi::Ecx>;
using NativeVoid = void(__cdecl *)() noexcept;
using StripDelete = fc::NativeCall<void *(void *, std::uint32_t) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using StripHide = fc::NativeCall<void(void *, bool) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using StripPosition = fc::NativeCall<void(void *, float, float, float) noexcept, fc::abi::Ecx, fc::abi::CalleeCleanup>;
using HudUpdate = fc::NativeCall<void(float) noexcept, fc::abi::Xmm0>;
using MessageEvent = fc::NativeCall<void(std::uint32_t, const wchar_t *, const std::uint32_t *) noexcept, fc::abi::Ecx, fc::abi::Edx>;

// Owns narrowly scoped native shell hooks, native chat servicing and copied native player names.
class Shell {
  public:
    // Qualifies exact retail instructions and reserves calls without owning the shared game-update hook.
    void plan(fc::Plan &plan);
    // Resolves recipient state before hooks become visible.
    fc::Result<> prepare();
    // Publishes immutable accepted state for the native application's thread.
    void configure(ShellSession session);

    // Sets the publication gate synchronously before BeginLobby's first advertisement.
    void host_intent(bool active) noexcept {
        host_intent_.store(active, std::memory_order_release);
    }

    // Uses the state-owned HUD and creates the original chat input and message displays.
    fc::Result<> open_chat();
    // Forgets previous state allocations when the provider initializes a new Lua VM.
    void reset_chat() noexcept;
    // Cancels the stock editor when leaving a screen without changing native message history.
    void close_editor() noexcept;
    // Destroys only native objects created by this shell session.
    void close_chat() noexcept;
    // Services the original HUD elements and queued roster notifications on the Lua owner thread.
    void update_chat(float dt);
    // Copies names once per campaign update on the Lua owner thread, retaining no game pointers.
    void capture_names() noexcept;
    // Retrieves copies for the control thread or Lua poll.
    std::vector<Player> names() const;
    // Adds a copied accepted-roster event without invoking Lua or a native function on the control thread.
    void event(std::string text);
    // Reads the local slot on a native/Lua owner thread.
    std::optional<std::uint8_t> local_slot() const noexcept;
    // Reads configured actions on the invoking native/Lua owner thread.
    std::optional<ShellInput> input() const noexcept;

  private:
    // Uses the native pool chain to reject truncated records before the void ReadBytes call.
    bool available(void *group, std::size_t count) const noexcept;
    // Reads the stock record format, authenticating source and routing before native queue insertion.
    void receive(void *group, std::optional<std::uint8_t> source) noexcept;
    // Applies the same authenticated relay and local-delivery path to host-local and received chat.
    void relay(const void *record, std::uint8_t source) noexcept;
    // Removes messages with no recipients left and returns the players still waiting for the next message.
    std::uint64_t pending_chat(const ShellSession &session) noexcept;
    // Encodes one native queued record with destination and host coverage scoped to this shell update.
    void send_to_client(void *group, std::uint32_t slot) noexcept;
    // Enforces privacy before the original native formatter and HUD message events.
    void display(const void *record) noexcept;

    // Identifies the current native shell; mission chat and team handling use the original game paths.
    bool in_shell() const noexcept;

    // Limits native input and display to the accepted session owning these shell objects.
    bool chat_active() const noexcept;
    // Routes original editor output through authenticated shell transport.
    void submit_chat(const wchar_t *text, std::uint32_t channel) noexcept;
    // Binds original chat/HUD functions with each image's native calling convention.
    void plan_chat(fc::Plan &plan, const ShellLayout &shell);

    std::atomic_bool host_intent_{};
    std::atomic<std::shared_ptr<const ShellSession>> session_;
    fc::NativeFunction<ChatInput> chat_input_, strip_init_, strip_activate_;
    fc::NativeFunction<KeyboardEvent> keyboard_event_;
    fc::NativeFunction<CopyName> copy_name_;
    fc::Memory<const std::uint32_t> key_map_;
    fc::Memory<const std::uint8_t> event_key_map_;
    std::array<bool, 256> key_events_{};
    fc::NativeFunction<NativeVoid> strip_create_, messages_create_, messages_destroy_;
    fc::NativeFunction<StripDelete> strip_delete_;
    fc::NativeFunction<StripHide> strip_hide_;
    fc::NativeFunction<StripPosition> strip_position_;
    fc::NativeFunction<HudUpdate> hud_update_;
    fc::NativeFunction<MessageEvent> message_event_;
    fc::NativeFunction<QueueChat> submit_;
    fc::Memory<void *> strip_global_, hud_group_;
    fc::Memory<void *> messages_global_;
    fc::Memory<std::uint8_t> shell_state_;
    void *owned_strip_{};
    void *controller_{};
    bool owns_messages_{}, consumed_{};
    fc::NativeFunction<WriteMove> write_move_;
    fc::NativeFunction<ReadMove> read_move_;
    fc::NativeFunction<WriteUpdate> write_update_;
    fc::NativeFunction<ReadUpdate> read_update_;
    fc::NativeFunction<QueueChat> queue_chat_;
    fc::NativeFunction<ChatCodec> send_server_, send_client_;
    fc::NativeFunction<HostSendUpdates> host_send_updates_;
    fc::Memory<const std::int32_t> client_queue_count_;
    fc::NativeFunction<DisplayChat> display_;
    fc::NativeFunction<PlayerName> player_name_;
    fc::NativeFunction<LocalSlot> local_slot_;
    fc::NativeFunction<ReadBytes> read_bytes_;
    fc::NativeFunction<ResolvePacket> resolve_packet_;
    fc::NativeFunction<RemoveNode> remove_node_;
    fc::NativeFunction<FreeRecord> free_record_;
    fc::Memory<std::byte> host_queue_memory_;
    void *host_queue_{};
    fc::Memory<std::uint32_t> destination_memory_;
    fc::Memory<std::uint64_t> coverage_memory_;
    fc::Memory<std::uint32_t> input_flags_;
    std::uint32_t *destination_{};
    std::uint64_t *coverage_{};
    mutable std::mutex copies_mutex_;
    std::vector<Player> names_;
    std::vector<std::string> chat_;
};
} // namespace fc_plugins::multiplayer_gc
