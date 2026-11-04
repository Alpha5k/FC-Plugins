#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fc_plugins::multiplayer_gc {
namespace {

// Appends width bytes of a little-endian integer.
void number(std::vector<std::byte> &bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::byte>(value >> (8 * i)));
    }
}

// Appends a two-byte length followed by the string's bytes.
void string(std::vector<std::byte> &bytes, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error{"Campaign message string exceeds its two-byte length"};
    }
    number(bytes, value.size(), 2);
    for (const unsigned char character : value) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

// Reads complete fields from one FC feature message and copies strings into owned storage.
struct Reader {
    std::span<const std::byte> bytes;
    bool valid{true};

    // Reads a little-endian integer of width bytes, marking incomplete fields invalid.
    std::uint64_t number(unsigned width) {
        if (bytes.size() < width) {
            valid = false;
            return 0;
        }
        std::uint64_t value{};
        for (unsigned i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
        }
        bytes = bytes.subspan(width);
        return value;
    }

    // Copies a length-prefixed string, marking an incomplete length or payload invalid.
    std::string string() {
        const auto size = static_cast<std::size_t>(number(2));
        if (!valid || bytes.size() < size) {
            valid = false;
            return {};
        }
        std::string value{reinterpret_cast<const char *>(bytes.data()), size};
        bytes = bytes.subspan(size);
        return value;
    }
};

// Recognizes one base64 character in the existing FC1 metadata envelope.
int digit(char value) noexcept {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto position = alphabet.find(value);
    return position == std::string_view::npos ? -1 : static_cast<int>(position);
}
} // namespace

bool Decision::same_context(const Decision &other) const noexcept {
    return faction == other.faction && turn == other.turn && screen == other.screen && pending_card == other.pending_card &&
           presentation_phase == other.presentation_phase && presentation_source == other.presentation_source;
}

bool valid_action_name(std::string_view name) {
    constexpr std::array<std::string_view, 14> actions{
        "recruit", "buy_card", "place_card",     "cancel_card", "build_fleet",  "move_fleet",      "undo_move",
        "pass",    "attack",   "decline_attack", "choose_mode", "choose_bonus", "continue_result", "continue_summary"};
    return std::ranges::contains(actions, name);
}

bool valid_presentation_phase(std::string_view phase) {
    return phase.empty() || phase == "build" || phase == "move";
}

bool action_owner(std::string_view action, bool host, bool commander) {
    return action == "continue_result" ? host : commander;
}

std::optional<std::uint8_t> action_faction(const Decision &decision, std::string_view action, std::uint8_t member_faction, bool host) {
    if (member_faction < 1 || member_faction > 2) {
        return std::nullopt;
    }
    if (action == "continue_result" && decision.screen == "ifs_freeform_result" && host) {
        return decision.faction;
    }
    if (action == "choose_bonus") {
        return (decision.bonus_pending_mask & (1u << (member_faction - 1))) ? std::optional{member_faction} : std::nullopt;
    }
    return member_faction == decision.faction ? std::optional{member_faction} : std::nullopt;
}

std::vector<std::byte> encode_message(const Message &message) {
    std::vector<std::byte> bytes;
    number(bytes, static_cast<std::uint8_t>(message.kind), 1);
    switch (message.kind) {
    case MessageKind::Ready:
        break;
    case MessageKind::CurrentState: {
        number(bytes, message.snapshot.cooperative, 1);
        number(bytes, message.snapshot.host_faction, 1);
        number(bytes, message.decision.id, 4);
        number(bytes, message.decision.turn, 4);
        number(bytes, message.decision.faction, 1);
        string(bytes, message.decision.screen);
        number(bytes, message.decision.pending_card, 4);
        string(bytes, message.decision.presentation_phase);
        string(bytes, message.decision.presentation_source);
        number(bytes, message.decision.presentation_remaining_ms, 4);
        number(bytes, message.decision.bonus_pending_mask, 1);
        number(bytes, message.decision.timer_enabled, 1);
        number(bytes, message.decision.timer_remaining_ms, 4);
        number(bytes, message.decision.bonus_remaining_ms, 4);
        string(bytes, message.snapshot.state.campaign);
        string(bytes, message.snapshot.state.mission);
        break;
    }
    case MessageKind::Action:
        number(bytes, message.action.decision, 4);
        string(bytes, message.action.name);
        string(bytes, message.action.first);
        string(bytes, message.action.second);
        number(bytes, message.action.index, 4);
        number(bytes, message.action.confirm, 1);
        break;
    case MessageKind::ActionResult:
        number(bytes, message.accepted, 1);
        string(bytes, message.error);
        break;
    case MessageKind::BattleSetup:
        number(bytes, message.battle.id, 4);
        string(bytes, message.battle.state.campaign);
        string(bytes, message.battle.state.mission);
        break;
    case MessageKind::BattleReady:
        number(bytes, message.token, 4);
        break;
    case MessageKind::Choose:
        number(bytes, message.faction, 1);
        break;
    case MessageKind::Roster:
        number(bytes, message.roster.players.size(), 1);
        for (const auto &player : message.roster.players) {
            number(bytes, player.slot, 1);
            number(bytes, player.faction, 1);
            number(bytes, player.host, 1);
            string(bytes, player.name);
        }
        for (const auto slot : message.roster.commanders) {
            number(bytes, static_cast<std::uint8_t>(slot), 1);
        }
        break;
    case MessageKind::Error:
        string(bytes, message.error);
        break;
    }
    return bytes;
}

std::optional<Message> decode_message(std::span<const std::byte> bytes) {
    Reader reader{bytes};
    Message message{.kind = static_cast<MessageKind>(reader.number(1))};
    switch (message.kind) {
    case MessageKind::Ready:
        break;
    case MessageKind::CurrentState: {
        const auto cooperative = reader.number(1);
        message.snapshot.cooperative = cooperative != 0;
        message.snapshot.host_faction = static_cast<std::uint8_t>(reader.number(1));
        message.decision.id = static_cast<std::uint32_t>(reader.number(4));
        message.decision.turn = static_cast<std::uint32_t>(reader.number(4));
        message.decision.faction = static_cast<std::uint8_t>(reader.number(1));
        message.decision.screen = reader.string();
        message.decision.pending_card = static_cast<std::uint32_t>(reader.number(4));
        message.decision.presentation_phase = reader.string();
        message.decision.presentation_source = reader.string();
        message.decision.presentation_remaining_ms = static_cast<std::uint32_t>(reader.number(4));
        message.decision.bonus_pending_mask = static_cast<std::uint8_t>(reader.number(1));
        const auto timer_enabled = reader.number(1);
        message.decision.timer_enabled = timer_enabled != 0;
        message.decision.timer_remaining_ms = static_cast<std::uint32_t>(reader.number(4));
        message.decision.bonus_remaining_ms = static_cast<std::uint32_t>(reader.number(4));
        message.snapshot.state.campaign = reader.string();
        message.snapshot.state.mission = reader.string();
        if (cooperative > 1 || message.snapshot.host_faction < 1 || message.snapshot.host_faction > 2 || message.decision.faction > 2 ||
            (!message.decision.faction && message.decision.id) || message.decision.bonus_pending_mask > 3 || timer_enabled > 1 ||
            message.decision.timer_remaining_ms > 600000 || message.decision.bonus_remaining_ms > 60000 ||
            !valid_presentation_phase(message.decision.presentation_phase) || message.snapshot.state.campaign.empty() ||
            message.snapshot.state.campaign.size() > kCampaignCapacity || message.snapshot.state.mission.size() > kMissionCapacity) {
            return std::nullopt;
        }
        break;
    }
    case MessageKind::Action: {
        message.action.decision = static_cast<std::uint32_t>(reader.number(4));
        message.action.name = reader.string();
        message.action.first = reader.string();
        message.action.second = reader.string();
        message.action.index = static_cast<std::uint32_t>(reader.number(4));
        const auto confirm = reader.number(1);
        message.action.confirm = confirm != 0;
        if (confirm > 1 || !valid_action_name(message.action.name)) {
            return std::nullopt;
        }
        break;
    }
    case MessageKind::ActionResult: {
        const auto accepted = reader.number(1);
        if (accepted > 1) {
            return std::nullopt;
        }
        message.accepted = accepted != 0;
        message.error = reader.string();
        break;
    }
    case MessageKind::BattleSetup:
        message.battle.id = static_cast<std::uint32_t>(reader.number(4));
        message.battle.state.campaign = reader.string();
        message.battle.state.mission = reader.string();
        if (!message.battle.id || message.battle.state.campaign.empty() || message.battle.state.campaign.size() > kCampaignCapacity ||
            message.battle.state.mission.size() > kMissionCapacity) {
            return std::nullopt;
        }
        break;
    case MessageKind::BattleReady:
        message.token = static_cast<std::uint32_t>(reader.number(4));
        break;
    case MessageKind::Choose:
        message.faction = static_cast<std::uint8_t>(reader.number(1));
        if (message.faction != 1 && message.faction != 2) {
            return std::nullopt;
        }
        break;
    case MessageKind::Roster: {
        const auto count = reader.number(1);
        if (count > 64) {
            return std::nullopt;
        }
        std::uint64_t slots{};
        unsigned hosts{};
        std::array<unsigned, 2> factions{};
        for (std::uint64_t index = 0; index < count; ++index) {
            Player player;
            player.slot = static_cast<std::uint8_t>(reader.number(1));
            player.faction = static_cast<std::uint8_t>(reader.number(1));
            const auto host = reader.number(1);
            player.host = host != 0;
            player.name = reader.string();
            if (player.slot >= 64 || player.faction > 2 || host > 1 || player.name.size() > 31 ||
                (slots & (std::uint64_t{1} << player.slot)) || (player.host && ++hosts > 1) ||
                (player.faction && ++factions[player.faction - 1] > 8)) {
                return std::nullopt;
            }
            slots |= std::uint64_t{1} << player.slot;
            message.roster.players.push_back(std::move(player));
        }
        for (std::size_t index = 0; index < message.roster.commanders.size(); ++index) {
            const auto encoded = reader.number(1);
            if (encoded == 255) {
                continue;
            }
            const auto player = std::ranges::find(message.roster.players, encoded, &Player::slot);
            if (player == message.roster.players.end() || player->faction != index + 1) {
                return std::nullopt;
            }
            message.roster.commanders[index] = static_cast<std::int32_t>(encoded);
        }
        break;
    }
    case MessageKind::Error:
        message.error = reader.string();
        break;
    default:
        return std::nullopt;
    }
    return reader.valid && reader.bytes.empty() ? std::optional{std::move(message)} : std::nullopt;
}

void PendingMessages::replace(const Message &message) {
    Value *value{};
    switch (message.kind) {
    case MessageKind::Ready:
        value = &ready;
        break;
    case MessageKind::Choose:
        value = &choose;
        break;
    case MessageKind::CurrentState:
        value = &state;
        break;
    case MessageKind::Roster:
        value = &roster;
        break;
    case MessageKind::Error:
        value = &error;
        break;
    case MessageKind::Action:
        value = &action;
        break;
    case MessageKind::ActionResult:
        value = &result;
        break;
    case MessageKind::BattleSetup:
        value = &battle;
        break;
    case MessageKind::BattleReady:
        value = &acknowledgement;
        break;
    }
    if (value) {
        *value = encode_message(message);
    }
}

std::vector<std::byte> encode_metadata(const SessionInfo &info) {
    std::vector<std::byte> bytes{std::byte{'M'}, std::byte{'G'}, std::byte{'C'}};
    number(bytes, info.count, 1);
    number(bytes, info.cooperative, 1);
    number(bytes, info.host_faction, 1);
    number(bytes, info.turn, 4);
    string(bytes, info.campaign);
    string(bytes, info.era);
    string(bytes, info.factions);
    string(bytes, info.victory_condition);
    string(bytes, info.difficulty);
    number(bytes, info.team_damage, 1);
    string(bytes, info.map);
    number(bytes, info.joinable, 1);
    number(bytes, info.maximum, 1);
    return bytes;
}

std::optional<SessionInfo> decode_metadata(std::string_view text) {
    if (!text.starts_with("FC1:3:")) {
        return std::nullopt;
    }
    const auto payload = text.substr(6);
    if (payload.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    bytes.reserve(payload.size() / 4 * 3);
    for (std::size_t i = 0; i < payload.size(); i += 4) {
        // FC1 permits padding only in the last quartet, with zero unused bits.
        unsigned digits[4]{};
        unsigned padding{};
        for (unsigned j = 0; j < 4; ++j) {
            if (payload[i + j] == '=') {
                if (j < 2 || i + 4 != payload.size()) {
                    return std::nullopt;
                }
                ++padding;
            } else {
                const auto value = digit(payload[i + j]);
                if (padding || value < 0) {
                    return std::nullopt;
                }
                digits[j] = static_cast<unsigned>(value);
            }
        }
        if ((padding == 2 && (digits[1] & 15)) || (padding == 1 && (digits[2] & 3))) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::byte>((digits[0] << 2) | (digits[1] >> 4)));
        if (padding < 2) {
            bytes.push_back(static_cast<std::byte>((digits[1] << 4) | (digits[2] >> 2)));
        }
        if (padding == 0) {
            bytes.push_back(static_cast<std::byte>((digits[2] << 6) | digits[3]));
        }
    }

    Reader reader{bytes};
    if (reader.number(3) != 0x43474D) {
        return std::nullopt;
    }
    const auto count = reader.number(1);
    const auto cooperative = reader.number(1);
    const auto host_faction = reader.number(1);
    const auto turn = static_cast<std::uint32_t>(reader.number(4));
    auto campaign = reader.string();
    auto era = reader.string();
    auto factions = reader.string();
    auto victory = reader.string();
    auto difficulty = reader.string();
    const auto team_damage = reader.number(1);
    auto map = reader.string();
    const auto joinable = reader.number(1);
    const auto maximum = reader.number(1);
    if (!reader.valid || !reader.bytes.empty() || maximum < 2 || maximum > (cooperative ? 8u : 16u) || count < 1 || count > maximum ||
        cooperative > 1 || host_faction < 1 || host_faction > 2 || team_damage > 1 || joinable > 1 || map.empty()) {
        return std::nullopt;
    }
    return SessionInfo{.count = static_cast<std::uint8_t>(count),
                       .cooperative = cooperative != 0,
                       .era = std::move(era),
                       .campaign = std::move(campaign),
                       .turn = turn,
                       .host_faction = static_cast<std::uint8_t>(host_faction),
                       .factions = std::move(factions),
                       .victory_condition = std::move(victory),
                       .difficulty = std::move(difficulty),
                       .team_damage = team_damage != 0,
                       .map = std::move(map),
                       .joinable = joinable != 0,
                       .maximum = static_cast<std::uint8_t>(maximum)};
}

std::uint64_t chat_exclusions(std::span<const Player> players, std::uint8_t sender, bool team_only) noexcept {
    const auto source = std::ranges::find(players, sender, &Player::slot);
    if (source == players.end() || (team_only && source->faction == 0)) {
        return UINT64_MAX;
    }
    std::uint64_t exclusions = UINT64_MAX;
    for (const auto &player : players) {
        if (player.slot < 64 && (!team_only || player.faction == source->faction)) {
            exclusions &= ~(std::uint64_t{1} << player.slot);
        }
    }
    return exclusions;
}
} // namespace fc_plugins::multiplayer_gc
