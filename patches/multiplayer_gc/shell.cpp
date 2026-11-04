#include "shell.hpp"
#include "chat_record.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace fc_plugins::multiplayer_gc {

void Shell::plan(fc::Plan &plan) {
    const auto layout = select_shell_layout(plan.profile());
    if (!layout) {
        plan.fail(std::format("No native layout for image profile '{}'", plan.profile()));
        return;
    }
    // These cdecl wrappers preserve each original return and append the stock chat codec before EndWrite/EndRead.
    write_move_ = plan.hook<WriteMove>(
        layout->write_move,
        [this](void *group, void *update, void *move) noexcept {
            static_cast<void>(write_move_.call(group, update, move));
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active) {
                if (!session->host) {
                    static_cast<void>(send_server_.call(group));
                }
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x50}));
    read_move_ = plan.hook<ReadMove>(
        layout->read_move,
        [this](void *group, std::uint32_t slot, void *move, void *state) noexcept {
            const auto changed = read_move_.call(group, slot, move, state).value_or(false);
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active && session->host && slot < 64 &&
                (session->connected_slots & (std::uint64_t{1} << slot))) {
                receive(group, static_cast<std::uint8_t>(slot));
            }
            return changed;
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xE4, 0x00, 0x00, 0x00}));
    write_update_ = plan.hook<WriteUpdate>(
        layout->write_update,
        [this](void *group, std::uint32_t slot, void *update, void *state) noexcept {
            static_cast<void>(write_update_.call(group, slot, update, state));
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active && session->host && slot < 64 &&
                (session->connected_slots & (std::uint64_t{1} << slot))) {
                send_to_client(group, slot);
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00}));
    read_update_ = plan.hook<ReadUpdate>(
        layout->read_update,
        [this](void *group, void *update, void *state) noexcept {
            const auto changed = read_update_.call(group, update, state).value_or(false);
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active && !session->host) {
                receive(group, std::nullopt);
            }
            return changed;
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x44, 0x01, 0x00, 0x00}));

    // ColoredChats owns the queue prologue and display interior; call through its queue and hook only display entry.
    plan.check(layout->queue_chat + 0x73, 5, fc::exact_bytes({0x68, 0x28, 0x01, 0x00, 0x00}));
    queue_chat_ = plan.function<QueueChat>(layout->queue_chat);
    send_server_ = plan.function<ChatCodec>(layout->send_server, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20}));
    send_client_ = plan.function<ChatCodec>(layout->send_client, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30}));
    display_ = plan.hook<DisplayChat>(
        layout->display,
        [this](const void *record) noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active) {
                display(record);
            } else {
                static_cast<void>(display_.call(record));
            }
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00}));
    player_name_ = plan.function<PlayerName>(layout->player_name);
    local_slot_ = plan.function<LocalSlot>(layout->local_slot);
    read_bytes_ = plan.function<ReadBytes>(layout->read_bytes, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1}));
    resolve_packet_ = plan.function<ResolvePacket>(layout->resolve_packet);
    destination_memory_ = plan.access<std::uint32_t>(layout->destination);
    coverage_memory_ = plan.access<std::uint64_t>(layout->coverage);
    host_queue_memory_ = plan.access<std::byte>(layout->host_queue, 20);
    remove_node_ = plan.function<RemoveNode>(layout->remove_node, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC}));
    free_record_ = plan.function<FreeRecord>(layout->free_record, fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x85, 0xC0}));
    // Send queued chat on the next client update instead of waiting for the idle timer.
    plan.check(layout->send_server + 6, 1, fc::exact_bytes({0xA1}));
    plan.check(layout->send_server + 7, 4, fc::points_to(plan.at(layout->client_queue_count)));
    client_queue_count_ = plan.access<const std::int32_t>(layout->client_queue_count);
    plan.check(layout->client_due - 4, 4, fc::exact_bytes({0x0F, 0x2F, 0x42, 0x04}));
    plan.hook_instruction(
        layout->client_due,
        [this](fc::CpuContext &cpu) noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active && !session->host && client_queue_count_.read().value_or(0) > 0) {
                // Clear the carry and zero flags so JBE falls through to the existing send path.
                cpu.eflags &= ~0x41u;
            }
        },
        fc::exact_bytes({0x0F, 0x86, 0x1E, 0x02, 0x00, 0x00}));
    // Request updates for players still waiting for this message; SendUpdates keeps its four-player limit.
    plan.check(layout->host_send_updates + 0x2A, 12,
               fc::exact_bytes({0x8B, 0x8A, 0x28, 0x15, 0x00, 0x00, 0x0B, 0x88, 0x2C, 0x15, 0x00, 0x00}));
    host_send_updates_ = plan.hook<HostSendUpdates>(
        layout->host_send_updates,
        [this](void *host) noexcept {
            const auto session = session_.load(std::memory_order_acquire);
            if (in_shell() && session && session->active && session->host) {
                const auto pending = pending_chat(*session);
                fc::write_field<std::uint64_t>(host, 0x1528, fc::read_field<std::uint64_t>(host, 0x1528) | pending);
            }
            static_cast<void>(host_send_updates_.call(host));
        },
        fc::exact_bytes({0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00}));

    // Each formatter sees the calculated count already at [ESP]; preserve the native roster and EDI.
    plan.hook_instruction(
        layout->publication_count,
        [this](fc::CpuContext &cpu) noexcept {
            if (host_intent_.load(std::memory_order_acquire)) {
                fc::write_field<std::uint32_t>(fc::stack_pointer(cpu), 0, 0);
            }
        },
        layout->publication_evidence);
    plan.check(layout->send_client + 0x33, 4, fc::points_to(plan.at(layout->destination)));
    plan.check(layout->send_client + 0x117, 4, fc::points_to(plan.at(layout->coverage)));

    // The native player-list callback reads this manager, then the processed input button word at +0x10.
    plan.check(layout->input_manager_reference - 1, 1, fc::exact_bytes({0xB9}));
    plan.check(layout->input_manager_reference, 4, fc::points_to(plan.at(layout->input_manager)));
    plan.check(layout->input_getter, 32,
               fc::exact_bytes({0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x85, 0xC0, 0x75, 0x10, 0x8D, 0x04, 0x40, 0x8D, 0x80, 0x99,
                                0x05, 0x00, 0x00, 0x8D, 0x04, 0xC1, 0x5D, 0xC2, 0x04, 0x00, 0x33, 0xC0, 0x5D, 0xC2, 0x04, 0x00}));
    plan.check(layout->input_flags_read, 3, fc::exact_bytes({0x8B, 0x58, 0x10}));
    input_flags_ = plan.access<std::uint32_t>(layout->input_manager + 0x2CD8);
    plan_chat(plan, *layout);
}

fc::Result<> Shell::prepare() {
    const auto destination = destination_memory_.view();
    const auto coverage = coverage_memory_.view();
    const auto queue = host_queue_memory_.view();
    if (!destination) {
        return std::unexpected(destination.error());
    }
    if (!coverage) {
        return std::unexpected(coverage.error());
    }
    if (!queue) {
        return std::unexpected(queue.error());
    }
    destination_ = destination->data();
    coverage_ = coverage->data();
    host_queue_ = queue->data();
    return {};
}

void Shell::configure(ShellSession session) {
    if (!session.active) {
        std::lock_guard lock(copies_mutex_);
        chat_.clear();
    }
    session_.store(std::make_shared<const ShellSession>(std::move(session)), std::memory_order_release);
}

std::optional<std::uint8_t> Shell::local_slot() const noexcept {
    const auto slot = local_slot_.call(0).value_or(-1);
    return slot >= 0 && slot < 64 ? std::optional{static_cast<std::uint8_t>(slot)} : std::nullopt;
}

void Shell::capture_names() noexcept {
    try {
        const auto session = session_.load(std::memory_order_acquire);
        if (!in_shell() || !session) {
            return;
        }
        std::vector<Player> names;
        for (unsigned slot = 0; slot < 64; ++slot) {
            if (!(session->capture_slots & (std::uint64_t{1} << slot))) {
                continue;
            }
            const auto *name = player_name_.call(slot).value_or(nullptr);
            if (name) {
                names.push_back({static_cast<std::uint8_t>(slot), 0, false, std::string{name, ::strnlen_s(name, 31)}});
            }
        }
        std::lock_guard lock(copies_mutex_);
        names_ = std::move(names);
    } catch (...) {
        // A failed name copy leaves the previous copied roster available to the control thread.
    }
}

std::vector<Player> Shell::names() const {
    std::lock_guard lock(copies_mutex_);
    return names_;
}

bool Shell::available(void *group, std::size_t count) const noexcept {
    auto *packet = fc::read_field<void *>(group, 0x10);
    const auto large = reinterpret_cast<std::uintptr_t>(resolve_packet_.call(0).value_or(nullptr));
    const auto small = reinterpret_cast<std::uintptr_t>(resolve_packet_.call(256).value_or(nullptr));
    bool first = true;
    // Shell indices cover exactly 256 large and 256 small pool records.
    for (unsigned visited = 0; packet && visited < 512; ++visited) {
        const auto address = reinterpret_cast<std::uintptr_t>(packet);
        const auto in_pool = [address](std::uintptr_t base, std::uintptr_t stride) {
            return base && address >= base && address - base < 256 * stride && (address - base) % stride == 0;
        };
        const auto capacity = in_pool(large, 0x400) ? 0x3F4 : in_pool(small, 0x80) ? 0x74 : 0;
        if (!capacity) {
            return false;
        }
        const auto position = first ? fc::read_field<std::int16_t>(packet, 4) : fc::read_field<std::int8_t>(packet, 9);
        const auto limit = fc::read_field<std::int16_t>(packet, 6);
        if (position < 0 || limit < position || limit > capacity) {
            return false;
        }
        const auto remaining = static_cast<std::size_t>(limit - position);
        if (remaining >= count) {
            return true;
        }
        count -= remaining;
        packet = resolve_packet_.call(fc::read_field<std::int16_t>(packet, 0)).value_or(nullptr);
        first = false;
    }
    return false;
}

void Shell::receive(void *group, std::optional<std::uint8_t> source) noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session || !available(group, 4)) {
        return;
    }
    std::uint32_t header{};
    static_cast<void>(read_bytes_.call(group, &header, 4));
    const auto size = header & 0xFFFF;
    if ((header >> 16) != 0xFFFF || !size) {
        return;
    }
    if (size < 0x16 || size > 0x114 || (size & 1) || !available(group, size)) {
        return;
    }
    ChatRecord record{};
    static_cast<void>(read_bytes_.call(group, &record, size));
    const auto characters = (size - 0x14) / 2;
    if (record.code != UINT32_MAX || record.channel > 1 || record.text[characters - 1] != L'\0') {
        return;
    }
    if (source) {
        relay(&record, *source);
    } else {
        display(&record);
    }
}

void Shell::relay(const void *data, std::uint8_t source) noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session) {
        return;
    }
    ChatRecord record{};
    std::memcpy(&record, data, 0x114);
    record.sender = source;
    record.exclusions = chat_exclusions(session->players, source, record.channel == 1);
    if (record.exclusions == UINT64_MAX) {
        return;
    }
    display(&record);
    record.exclusions |= std::uint64_t{1} << session->local_slot;
    static_cast<void>(queue_chat_.call(1, record.sender, record.exclusions, UINT32_MAX, record.text.data(), record.channel));
}

std::uint64_t Shell::pending_chat(const ShellSession &session) noexcept {
    // Retire a fully excluded head even when no remaining destination would make the stock encoder pop it.
    while (fc::read_field<std::int32_t>(host_queue_, 0x10) > 0) {
        auto *node = fc::read_field<void *>(host_queue_, 4);
        auto *record = fc::read_field<ChatRecord *>(node, 0xC);
        if (record->code == UINT32_MAX && record->sender < 64) {
            record->exclusions |= chat_exclusions(session.players, static_cast<std::uint8_t>(record->sender), record->channel == 1);
        }
        if ((record->exclusions & session.connected_slots) != session.connected_slots) {
            return session.connected_slots & ~record->exclusions & ~(std::uint64_t{1} << session.local_slot);
        }
        static_cast<void>(remove_node_.call(host_queue_, node));
        static_cast<void>(free_record_.call(record));
    }
    return 0;
}

void Shell::send_to_client(void *group, std::uint32_t slot) noexcept {
    const auto session = session_.load(std::memory_order_acquire);
    if (!session) {
        return;
    }
    pending_chat(*session);
    const auto destination = *destination_;
    const auto coverage = *coverage_;
    *destination_ = slot;
    *coverage_ = session->connected_slots;
    static_cast<void>(send_client_.call(group));
    *destination_ = destination;
    *coverage_ = coverage;
}

void Shell::display(const void *data) noexcept {
    ChatRecord record{};
    std::memcpy(&record, data, 0x114);
    const auto session = session_.load(std::memory_order_acquire);
    if (!chat_active() || record.sender >= 64 || record.channel > 1 || record.code != UINT32_MAX) {
        return;
    }
    const auto mask = chat_exclusions(session->players, static_cast<std::uint8_t>(record.sender), record.channel == 1);
    if ((mask | record.exclusions) & (std::uint64_t{1} << session->local_slot)) {
        return;
    }
    if (std::ranges::find(session->players, record.sender, &Player::slot) == session->players.end() ||
        std::ranges::find(record.text, L'\0') == record.text.end()) {
        return;
    }
    static_cast<void>(display_.call(&record));
}

} // namespace fc_plugins::multiplayer_gc
