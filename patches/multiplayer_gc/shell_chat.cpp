#include "shell.hpp"
#include "chat_record.hpp"

#include <algorithm>
#include <cwchar>
#include <utility>

namespace fc_plugins::multiplayer_gc {

void Shell::plan_chat(fc::Plan &plan, const ShellLayout &shell) {
    const auto &layout = shell.chat;
    // The original update chooses ChatInputProcess from this same shell-state branch.
    plan.check(layout.input_gate - 0x16, 4, fc::points_to(plan.at(layout.shell_state)));
    plan.check(layout.input_gate + 2, 4, fc::exact_bytes({0x8B, 0xCE, 0x74, 0x07}));
    plan.check(layout.input_gate + 6, 5, fc::direct_call_to(plan.at(layout.input_process)));
    // Admit the original configured chat actions only while GC owns the native displays.
    plan.hook_instruction(
        layout.input_gate,
        [this](fc::CpuContext &cpu) noexcept {
            if (chat_active()) {
                cpu.ecx = (cpu.ecx & ~0xFFu) | 1u;
            }
        },
        fc::exact_bytes({0x84, 0xC9}));
    plan.check(layout.shell_gate - 7, 2, fc::exact_bytes({0x80, 0x3D}));
    plan.check(layout.shell_gate - 1, 1, fc::exact_bytes({0x00}));
    plan.hook_instruction(
        layout.shell_gate,
        [this](fc::CpuContext &cpu) noexcept {
            if (chat_active()) {
                cpu.eflags |= 0x40;
            }
        },
        fc::exact_bytes({0x75, 0x40}));
    // RedKeyboard posts KEYCHAR for presses and its timed repeats before the controller reads held keys.
    plan.check(layout.input_process + 0x67, 4, fc::points_to(plan.at(layout.key_map)));
    plan.check(layout.keyboard_update + 0x137, 4, fc::points_to(plan.at(layout.event_key_map)));
    plan.check(layout.keyboard_update + 0x13D, 2, fc::exact_bytes({0x6A, 0x02}));
    plan.check(layout.keyboard_update + 0x13F, 5, fc::direct_call_to(plan.at(layout.keyboard_event)));
    key_map_ = plan.access<const std::uint32_t>(layout.key_map, 144);
    event_key_map_ = plan.access<const std::uint8_t>(layout.event_key_map, 256);
    keyboard_event_ = plan.hook<KeyboardEvent>(
        layout.keyboard_event,
        [this](std::uint32_t type, void *keyboard, std::uint32_t key, std::uint32_t character, std::uint32_t flags) noexcept {
            if (type == 2 && key < key_events_.size() && chat_active() && controller_ &&
                fc::read_field<void *>(controller_, 0x2608) == keyboard) {
                key_events_[key] = true;
            }
            static_cast<void>(keyboard_event_.call(type, keyboard, key, character, flags));
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x35}));
    // Preserve the native editor while replacing its frame-count repeats with stock keyboard events.
    chat_input_ = plan.hook<ChatInput>(
        layout.input_process,
        [this](void *controller) noexcept {
            const bool active = chat_active();
            if (active) {
                controller_ = controller;
                consumed_ = fc::read_field<std::uint16_t>(controller, 0x2768) != 0;
                const auto keys = key_map_.view();
                const auto event_keys = event_key_map_.view();
                if (keys && event_keys) {
                    prepare_chat_repeat(controller, *keys, *event_keys, key_events_);
                }
                key_events_.fill(false);
            }
            static_cast<void>(chat_input_.call(controller));
            if (active) {
                consumed_ |= fc::read_field<std::uint16_t>(controller, 0x2768) != 0;
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x14, 0x01, 0x00, 0x00}));
    // Authenticate only this editor's queue call; retain the native queue and ColoredChats formatting.
    submit_ = plan.hook_call<QueueChat>(
        layout.submit,
        [this](std::uint32_t destination, std::uint32_t sender, std::uint64_t exclusions, std::uint32_t code, const wchar_t *text,
               std::uint32_t channel) noexcept {
            if (chat_active()) {
                submit_chat(text, channel);
            } else {
                static_cast<void>(submit_.call(destination, sender, exclusions, code, text, channel));
            }
        },
        fc::direct_call_to(plan.at(shell.queue_chat)));

    // The shell has NetPlayer names but no mission PlayerStats for the original name or team lookup.
    copy_name_ = plan.function<CopyName>(layout.copy_name, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08}));
    plan.hook_instruction(
        layout.display_name,
        [this](fc::CpuContext &cpu) noexcept {
            if (chat_active() && (cpu.eflags & 0x40)) {
                const auto *record = fc::read_field<const ChatRecord *>(fc::frame_pointer(cpu), 8);
                const auto *name = player_name_.call(record->sender).value_or(nullptr);
                if (name) {
                    static_cast<void>(copy_name_.call(reinterpret_cast<wchar_t *>(cpu.ebp - 0x15C), name, 128));
                }
            }
        },
        fc::exact_bytes({0x74, 0x1D}));
    // Accepted faction routing already filters recipients; retain the channel for native chat colors.
    plan.hook_instruction(
        layout.display_team,
        [this](fc::CpuContext &cpu) noexcept {
            if (chat_active()) {
                cpu.eflags &= ~0x40u;
            }
        },
        fc::exact_bytes({0x75, 0x35}));

    // Constructors use ordinary cdecl calls; editor methods carry this in ECX.
    const auto create_evidence = fc::exact_bytes({0x55, 0x8B, 0xEC, 0x6A, 0xFF});
    strip_create_ = plan.function<NativeVoid>(layout.strip_create, create_evidence);
    messages_create_ = plan.function<NativeVoid>(layout.messages_create, fc::exact_bytes({0x56, 0x57, 0xBE}));
    strip_delete_ = plan.function<StripDelete>(layout.strip_delete, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}));
    // The HUD update receives dt in XMM0; message events use ECX/EDX and a caller-owned color on the stack.
    strip_init_ = plan.function<ChatInput>(layout.strip_init, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}));
    strip_activate_ =
        plan.function<ChatInput>(layout.strip_activate, fc::exact_bytes({0x8B, 0x01, 0x6A, 0x00, 0x6A, 0x00, 0xFF, 0x50, 0x08, 0xC3}));
    strip_hide_ = plan.function<StripHide>(layout.strip_hide, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08}));
    strip_position_ =
        plan.function<StripPosition>(layout.strip_position, fc::exact_bytes({0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x45, 0x08}));
    hud_update_ = plan.function<HudUpdate>(layout.hud_update, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08}));
    plan.check(layout.hud_update + 0xD, 5, fc::exact_bytes({0xF3, 0x0F, 0x11, 0x45, 0xFC}));
    message_event_ =
        plan.function<MessageEvent>(layout.message_event, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xEC, 0x14}));
    messages_destroy_ = plan.function<NativeVoid>(layout.messages_destroy, fc::exact_bytes({0x56, 0x57, 0xBE}));

    // Every singleton access is tied to an original instruction operand, including relocated images.
    plan.check(layout.strip_create + 0x3C, 4, fc::points_to(plan.at(layout.strip_global)));
    plan.check(layout.hud_group_reference, 4, fc::points_to(plan.at(layout.hud_group)));
    plan.check(layout.messages_create + 3, 4, fc::points_to(plan.at(layout.messages_global)));
    plan.check(layout.messages_destroy + 3, 4, fc::points_to(plan.at(layout.messages_global)));
    plan.check(layout.shell_gate - 5, 4, fc::points_to(plan.at(layout.shell_state)));
    shell_state_ = plan.access<std::uint8_t>(layout.shell_state);
    strip_global_ = plan.access<void *>(layout.strip_global);
    hud_group_ = plan.access<void *>(layout.hud_group);
    messages_global_ = plan.access<void *>(layout.messages_global, 2);
}

bool Shell::in_shell() const noexcept {
    return shell_state_.read().value_or(0) != 0;
}

bool Shell::chat_active() const noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    return in_shell() && owned_strip_ && session && session->active;
}

fc::Result<> Shell::open_chat() {
    if (chat_active()) {
        return {};
    }
    const auto session = session_.load(std::memory_order_acquire);
    if (!session || !session->active) {
        return std::unexpected(fc::Error{"Join the campaign before opening chat."});
    }
    const auto shell = shell_state_.read();
    const auto group = hud_group_.read();
    const auto messages = messages_global_.view();
    if (!shell || !*shell || !group || !*group || !messages) {
        return std::unexpected(fc::Error{"The native chat renderer is unavailable."});
    }
    if (((*messages)[0] == nullptr) != ((*messages)[1] == nullptr)) {
        return std::unexpected(fc::Error{"The native message display is incomplete."});
    }
    // PreStateInit opens the HUD before shell Lua; PostStateCleanup owns its loaded elements.
    if (!(*messages)[0]) {
        static_cast<void>(messages_create_.call());
        owns_messages_ = true;
    }
    // Mission cleanup resets its thread groups and heaps before shell entry; Create replaces the stale global without dereferencing it.
    static_cast<void>(strip_create_.call());
    owned_strip_ = strip_global_.read().value_or(nullptr);
    if (!owned_strip_ || !messages_global_.read().value_or(nullptr) || !messages_global_.read(1).value_or(nullptr)) {
        close_chat();
        return std::unexpected(fc::Error{"The game could not create its native chat display."});
    }
    static_cast<void>(strip_init_.call(owned_strip_));
    static_cast<void>(strip_activate_.call(owned_strip_));
    // Keep the original renderer anchor and children; move the stock strip below the GC heading.
    static_cast<void>(strip_position_.call(static_cast<std::byte *>(owned_strip_) + 0x20, 0.0f, 78.0f, 0.0f));
    static_cast<void>(strip_hide_.call(owned_strip_, false));
    return {};
}

void Shell::reset_chat() noexcept {
    owned_strip_ = controller_ = nullptr;
    owns_messages_ = consumed_ = false;
    key_events_.fill(false);
}

void Shell::close_editor() noexcept {
    key_events_.fill(false);
    if (controller_) {
        fc::write_field<std::uint16_t>(controller_, 0x2768, 0);
        fc::write_field<std::uint8_t>(controller_, 0x27EA, 0);
        fc::write_field<wchar_t>(controller_, 0x276A, L'\0');
    }
    if (owned_strip_) {
        static_cast<void>(strip_hide_.call(owned_strip_, false));
    }
}

void Shell::close_chat() noexcept {
    close_editor();
    controller_ = nullptr;
    consumed_ = false;
    if (owned_strip_) {
        if (strip_global_.read().value_or(nullptr) == owned_strip_) {
            static_cast<void>(strip_delete_.call(owned_strip_, 1));
            static_cast<void>(strip_global_.write(nullptr));
        }
        owned_strip_ = nullptr;
    }
    if (owns_messages_) {
        owns_messages_ = false;
        static_cast<void>(messages_destroy_.call());
    }
}

void Shell::update_chat(float dt) {
    if (!chat_active()) {
        return;
    }
    std::vector<std::string> events;
    {
        std::lock_guard lock(copies_mutex_);
        events = std::exchange(chat_, {});
    }
    for (const auto &event : events) {
        std::wstring text;
        for (const unsigned char character : event) {
            text.push_back(static_cast<wchar_t>(character));
        }
        const std::uint32_t color = 0xFFFFFFFF;
        static_cast<void>(message_event_.call(0, text.c_str(), &color));
    }
    static_cast<void>(hud_update_.call(dt));
}

std::optional<ShellInput> Shell::input() const noexcept {
    const auto flags = input_flags_.read();
    if (!flags) {
        return std::nullopt;
    }
    return ShellInput{
        .player_list = (*flags & (1u << 17)) != 0, .consumed = chat_active() && consumed_, .chat_open = owned_strip_ != nullptr};
}

void Shell::event(std::string text) {
    std::lock_guard lock(copies_mutex_);
    chat_.push_back(std::move(text));
}

void Shell::submit_chat(const wchar_t *text, std::uint32_t channel) noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session || !text || channel > 1 || chat_exclusions(session->players, session->local_slot, channel == 1) == UINT64_MAX) {
        return;
    }
    ChatRecord record{.channel = channel};
    const auto length = ::wcsnlen_s(text, record.text.size());
    if (!length || length == record.text.size()) {
        return;
    }
    std::copy_n(text, length + 1, record.text.begin());
    if (session->host) {
        relay(&record, session->local_slot);
    } else {
        static_cast<void>(queue_chat_.call(0, session->local_slot, 0, UINT32_MAX, record.text.data(), channel));
    }
}

} // namespace fc_plugins::multiplayer_gc
