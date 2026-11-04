#include "../patches/soldier_state_sync/format.hpp"
#include "../patches/soldier_state_sync/phase.hpp"
#include "../patches/direct_transport/protocol.hpp"
#include "../patches/direct_transport/security.hpp"
#include "../patches/direct_transport/socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace state_sync = fc_plugins::soldier_state_sync;
namespace direct = fc_plugins::direct_transport;

namespace {

// Reproduces the qualified native fixed-point interior arithmetic for this state clock.
float restore_clock(float value, state_sync::Format format) {
    if (state_sync::state_clock_bits(format) == 32) {
        return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value));
    }
    const auto code = static_cast<std::uint32_t>((value / 8.0F) * 8190.0F + 0.5F);
    return static_cast<float>(code) / 8190.0F * 8.0F;
}

// Advances the native clock with the same single-precision 50 ms steps.
float advance_clock(float value) {
    value += 0.05F;
    value += 0.05F;
    return value;
}

} // namespace

TEST_CASE("Soldier state clock preserves the strict fall threshold after authority restoration", "[soldier_state_sync]") {
    const float writer = 0.9000000953674316F;
    const float native_end = advance_clock(writer);
    REQUIRE(native_end > 1.0F);
    const auto old_format = state_sync::packet_format(0x0C, 0x80);
    const auto state_format = state_sync::packet_format(0x1F, 0x80);
    CHECK(advance_clock(restore_clock(writer, old_format)) < 1.0F);
    CHECK(advance_clock(restore_clock(writer, state_format)) == native_end);

    // Exact and adjacent float values keep the native comparison's result without a tolerance.
    for (const float initial : {std::nextafter(0.9F, 0.0F), 0.9F, std::nextafter(0.9F, 1.0F), 8.25F}) {
        const auto restored = restore_clock(initial, state_format);
        CHECK(std::bit_cast<std::uint32_t>(restored) == std::bit_cast<std::uint32_t>(initial));
        CHECK((advance_clock(restored) > 1.0F) == (advance_clock(initial) > 1.0F));
    }
}

TEST_CASE("Soldier velocity tag preserves native fragment bounds and compression", "[soldier_state_sync]") {
    CHECK(state_sync::valid_fragment(0x80, 1));
    CHECK(state_sync::valid_fragment(0xFE, 127));
    CHECK_FALSE(state_sync::valid_fragment(0xFF, 127));
    CHECK_FALSE(state_sync::valid_fragment(0x80, 0));
    for (unsigned count = 1; count <= 127; ++count) {
        for (unsigned index = 0; index < count; ++index) {
            const auto tagged = static_cast<std::uint8_t>(index | state_sync::kRawMagnitude);
            const auto compressed_count = static_cast<std::uint8_t>(count | 0x80);
            CHECK(state_sync::fragment_index(tagged) == index);
            CHECK(state_sync::valid_fragment(tagged, compressed_count & 0x7F));
        }
    }
}

TEST_CASE("Soldier precise triggers restore native state and duration while preserving unrelated flags", "[soldier_state_sync]") {
    constexpr std::uint32_t unrelated = 0xE00000E0;
    for (std::uint32_t state = 0; state < 32; ++state) {
        for (const std::uint32_t duration : {0U, 1U, 0x7FFFU, 0x8000U, 393216U, 524288U, 589824U, 0xFFFFFU, 0x1FFFFFU}) {
            const auto restored = state_sync::restore_trigger(unrelated | 0x00123409, state, duration);
            const auto code = state_sync::trigger_code((duration << 8) | state);
            CHECK(state_sync::restore_trigger(unrelated, code & 0x1F, code >> 5) == restored);
            CHECK((restored & unrelated) == unrelated);
            CHECK((restored & 0x1F) == state);
            CHECK(((restored >> 8) & 0x1FFFFF) == duration);
        }
    }
    // Crouch hold checkpoints at 20 and 60 TPS retain their strict 0.30 s threshold.
    CHECK(state_sync::trigger_code(0x06000001) == 0x00C00001);
    CHECK(state_sync::trigger_code(0x08000001) == 0x01000001);
    CHECK(state_sync::restore_trigger(unrelated, 1, 589824) == 0xE90000E1);

    // Native 0x0C and 0x1C remain distinct even with no duration; no sentinel is necessary.
    CHECK(state_sync::restore_trigger(0, 0x0C, 0) == 0x0C);
    CHECK(state_sync::restore_trigger(0, 0x1C, 0) == 0x1C);
}

TEST_CASE("Soldier sprint and crouch wire bits preserve following fields at an unaligned offset", "[soldier_state_sync]") {
    // A fixed native word gives an independently specified code and a field crossing the 32-bit boundary.
    const auto code = state_sync::trigger_code(0x1122330C);
    REQUIRE(code == 0x0224466C);
    const std::uint64_t stream = 5U | (std::uint64_t{code} << 3) | (std::uint64_t{0x15} << 29);
    CHECK(stream == 0x2B1223365ULL);
    CHECK(((stream >> 3) & 0x3FFFFFF) == code);
    CHECK(((stream >> 29) & 0x1F) == 0x15);
    CHECK(state_sync::restore_trigger(0xE00000E0, static_cast<std::uint32_t>(stream >> 3) & 0x1F,
                                      static_cast<std::uint32_t>(stream >> 8) & 0x1FFFFF) == 0xF12233EC);
}

TEST_CASE("Soldier fire trigger loop preserves both exact words and following fields at an unaligned offset", "[soldier_state_sync]") {
    constexpr std::array triggers{0x1122330CU, 0x07000001U};
    std::uint64_t stream = 5;
    for (std::size_t index = 0; index < triggers.size(); ++index) {
        stream |= std::uint64_t{state_sync::trigger_code(triggers[index])} << (3 + 26 * index);
    }
    stream |= std::uint64_t{0xA5} << 55;
    CHECK((stream & 7) == 5);
    for (std::size_t index = 0; index < triggers.size(); ++index) {
        const auto code = static_cast<std::uint32_t>(stream >> (3 + 26 * index)) & 0x3FFFFFF;
        CHECK(state_sync::restore_trigger(0xE00000E0, code & 31, code >> 5) == (triggers[index] | 0xE00000E0));
    }
    CHECK((stream >> 55) == 0xA5);
}

TEST_CASE("Soldier retained formats preserve old packets and separate mixed fragments", "[soldier_state_sync]") {
    int outer{}, inner{};
    state_sync::SendFormat first{.format = state_sync::Format::State32};
    first.bind(&outer);
    first.bind(&inner);
    CHECK(first.applies(&outer));
    CHECK_FALSE(first.applies(&inner));
    const state_sync::SendFormat nested{.group = &inner, .format = state_sync::Format::Native};
    CHECK_FALSE(nested.applies(&inner));
    CHECK(first.applies(&outer));

    // The complete stored header selects the format even after agreement or destination changes.
    CHECK(state_sync::packet_format(0x0C, 0x80) == state_sync::Format::Velocity32);
    CHECK(state_sync::packet_format(0x1F, 0x80) == state_sync::Format::State32);
    CHECK(state_sync::packet_format(0x20, 0x80) == state_sync::Format::Precision);
    CHECK(state_sync::packet_format(0x20, 0) == state_sync::Format::Native);
    CHECK_FALSE(state_sync::valid_update_fragment(0x20, 0, 1));
    CHECK(state_sync::valid_update_fragment(0x20, 0x80, 1));
    CHECK(state_sync::valid_update_fragment(0x20, 0xFE, 127));
    CHECK_FALSE(state_sync::valid_update_fragment(0x20, 0xFF, 127));
    CHECK(state_sync::packet_format(0x0C, 0) == state_sync::Format::Native);
    CHECK(state_sync::packet_format(0x1F, 0) == state_sync::Format::Native);
    CHECK_FALSE(state_sync::valid_update_fragment(0x1F, 0, 1));
    CHECK_FALSE(state_sync::valid_update_fragment(0x1F, 0x80, 0));
    CHECK_FALSE(state_sync::valid_update_fragment(0x1F, 0xFF, 127));
    CHECK(state_sync::valid_update_fragment(0x0C, 0x80, 1));
    CHECK(state_sync::valid_update_fragment(0x1F, 0x80, 1));
    CHECK_FALSE(state_sync::raw_magnitude(0x0B, 0x80));
    CHECK(state_sync::state_clock_bits(state_sync::packet_format(0x0C, 0x80)) == 13);
    CHECK(state_sync::state_clock_bits(state_sync::packet_format(0x1F, 0x80)) == 32);
    CHECK(state_sync::state_clock_bits(state_sync::packet_format(0x20, 0x80)) == 32);

    CHECK(state_sync::select_format(false, false) == state_sync::Format::Native);
    CHECK(state_sync::select_format(true, false) == state_sync::Format::Velocity32);
    CHECK(state_sync::select_format(false, true) == state_sync::Format::State32);
    CHECK(state_sync::select_format(true, true) == state_sync::Format::State32);
    CHECK(state_sync::select_format(true, true, true) == state_sync::Format::Precision);
    CHECK(state_sync::select_format(false, false, true) == state_sync::Format::Precision);

    CHECK(state_sync::update_packet(0x21));
    CHECK(state_sync::packet_format(0x21, 0x80) == state_sync::Format::WeaponState);
    CHECK(state_sync::packet_format(0x21, 0) == state_sync::Format::Native);
    CHECK_FALSE(state_sync::valid_update_fragment(0x21, 0, 1));
    CHECK(state_sync::valid_update_fragment(0x21, 0x80, 1));
    CHECK(state_sync::select_format(true, true, true, true) == state_sync::Format::WeaponState);
    CHECK(state_sync::select_format(false, false, false, true) == state_sync::Format::WeaponState);
    CHECK(state_sync::state_clock_bits(state_sync::Format::WeaponState) == 32);
    CHECK(state_sync::precise_gameplay(state_sync::Format::WeaponState));
    constexpr std::array types{std::uint8_t{0x0C}, std::uint8_t{0x1F}, std::uint8_t{0x20}, std::uint8_t{0x21}};
    for (const auto incoming : types) {
        for (const auto pending : types) {
            CHECK(state_sync::same_group_format(incoming, 0x80, 127, pending, 0xFE, 127) == (incoming == pending));
        }
    }

    CHECK_FALSE(state_sync::same_group_format(0x0C, 0x80, 2, 0x1F, 0x81, 2));
    CHECK_FALSE(state_sync::same_group_format(0x0C, 0, 2, 0x0C, 0x81, 2));
    CHECK_FALSE(state_sync::same_group_format(0x1F, 0x80, 2, 0x1F, 0x81, 3));
    CHECK(state_sync::same_group_format(0x0C, 0x80, 2, 0x0C, 0x81, 2));
    CHECK(state_sync::same_group_format(0x1F, 0x80, 127, 0x1F, 0xFE, 127));
}

TEST_CASE("Direct Transport preserves and authenticates Soldier format separately from compression", "[soldier_state_sync]") {
    const direct::SessionKey key{};
    for (const std::uint8_t type : {std::uint8_t{0x0C}, std::uint8_t{0x1F}, std::uint8_t{0x20}, std::uint8_t{0x21}}) {
        const std::array<std::uint8_t, 7> payload{type, 1, 0, 0xFE, 0xFF, 0x12, 0x34};
        std::array<std::uint8_t, direct::kDirectHeaderBytes + payload.size()> bytes{};
        REQUIRE(direct::write_data_datagram(bytes, direct::Direction::ServerToClient, key, 12, 30, payload) == bytes.size());
        direct::ParsedDirect parsed;
        REQUIRE(direct::parse_direct(bytes, parsed) == direct::ParseStatus::Ok);
        CHECK(std::ranges::equal(parsed.payload, payload));
        CHECK(direct::verify_authentication_tag(direct::Direction::ServerToClient, key, std::span(bytes).first(direct::kDirectHeaderBytes),
                                                parsed.payload));
        CHECK(state_sync::fragment_index(parsed.payload[3]) == 126);
        CHECK((parsed.payload[4] & 0x7F) == 127);
        bytes[direct::kDirectHeaderBytes] ^= 1;
        CHECK_FALSE(direct::verify_authentication_tag(direct::Direction::ServerToClient, key,
                                                      std::span(bytes).first(direct::kDirectHeaderBytes), parsed.payload));
        bytes[direct::kDirectHeaderBytes] ^= 1;
        bytes[direct::kDirectHeaderBytes + 3] ^= 0x80;
        CHECK_FALSE(direct::verify_authentication_tag(direct::Direction::ServerToClient, key,
                                                      std::span(bytes).first(direct::kDirectHeaderBytes), parsed.payload));
    }
}

TEST_CASE("Soldier melee phase carries live chronological inputs and exact ages without moving following fields", "[soldier_state_sync]") {
    // The fixture starts three bits into a native stream and exercises both optional field groups.
    for (const bool alignment : {false, true}) {
        for (const bool deflect : {false, true}) {
            for (const std::uint8_t count : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{8}}) {
                for (const bool different : {false, true}) {
                    for (const bool precise : {false, true}) {
                        const state_sync::PhaseProfile profile{8, 12, alignment, deflect};
                        state_sync::MeleePhase phase{
                            .age = std::bit_cast<std::uint32_t>(0.3000000119F),
                            .previous = different ? 7U : 8U,
                            .previous_duration = std::bit_cast<std::uint32_t>(0.3125F),
                            .clock = std::bit_cast<std::uint32_t>(0.4000000358F),
                            .blend = std::bit_cast<std::uint32_t>(0.375F),
                            .count = count,
                            .held = 0xFE,
                            .gravity = true,
                            .flee = true,
                            .alignment_z = std::bit_cast<std::uint32_t>(0.75F),
                            .alignment_x = std::bit_cast<std::uint32_t>(-0.5F),
                            .deflect_age = std::bit_cast<std::uint32_t>(0.125F),
                            .deflect_variant = 7,
                            .precise_ages = precise,
                        };
                        for (std::size_t i = 0; i < count; ++i) {
                            phase.events[i] = {std::bit_cast<std::uint32_t>(static_cast<float>(i) * 0.0625F),
                                               static_cast<std::uint8_t>(i % 7 + 1), static_cast<std::uint8_t>(i % 8)};
                            phase.ages.values[i] = 0.4 - static_cast<double>(i) / 60.0;
                        }
                        std::vector<bool> stream{true, false, true};
                        const auto put = [&stream](std::uint32_t value, std::int32_t bits) noexcept {
                            for (int i = 0; i < bits; ++i) {
                                stream.push_back(((value >> i) & 1) != 0);
                            }
                        };
                        state_sync::write_phase(profile, phase, put);
                        const auto expected =
                            113 + 38 * count + 37 * different + 64 * alignment + 40 * deflect + (precise ? 64 * count : 0);
                        CHECK(stream.size() == static_cast<std::size_t>(3 + expected));
                        put(0xFA7C9013, 32);
                        std::size_t cursor = 3;
                        const auto get = [&stream, &cursor](std::int32_t bits) noexcept {
                            std::uint32_t value{};
                            for (int i = 0; i < bits; ++i) {
                                value |= static_cast<std::uint32_t>(stream[cursor++]) << i;
                            }
                            return value;
                        };
                        const auto received = state_sync::read_phase(get);
                        REQUIRE(received);
                        CHECK(get(32) == 0xFA7C9013);
                        CHECK(cursor == stream.size());
                        CHECK(received->age == phase.age);
                        CHECK(received->previous_is_current == !different);
                        CHECK(received->previous == (different ? phase.previous : 0));
                        CHECK(received->previous_duration == (different ? phase.previous_duration : 0));
                        CHECK(received->clock == phase.clock);
                        CHECK(received->blend == phase.blend);
                        CHECK(received->held == phase.held);
                        CHECK(received->count == count);
                        CHECK(received->gravity);
                        CHECK(received->flee);
                        CHECK(received->precise_ages == precise);
                        CHECK(received->alignment == alignment);
                        CHECK(received->deflect == deflect);
                        for (std::size_t i = 0; i < count; ++i) {
                            CHECK(received->events[i].time == phase.events[i].time);
                            CHECK(received->events[i].button == phase.events[i].button);
                            CHECK(received->events[i].action == phase.events[i].action);
                            if (precise) {
                                CHECK(std::bit_cast<std::uint64_t>(received->ages.values[i]) ==
                                      std::bit_cast<std::uint64_t>(phase.ages.values[i]));
                            }
                        }
                        if (alignment) {
                            CHECK(received->alignment_z == phase.alignment_z);
                            CHECK(received->alignment_x == phase.alignment_x);
                        }
                        if (deflect) {
                            CHECK(received->deflect_age == phase.deflect_age);
                            CHECK(received->deflect_variant == phase.deflect_variant);
                        }

                        // An omitted previous combo resolves only when applying to the decoded native combo.
                        std::array<std::byte, 0x1A0> weapon{};
                        REQUIRE(state_sync::restore_phase(weapon.data(), profile, *received));
                        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x184) == phase.previous);
                        CHECK(fc::read_field<std::uint32_t>(weapon.data(), 0x188) == phase.age);

                        const auto restored_weapon = weapon;
                        const auto captured = state_sync::capture_phase(weapon.data(), profile);
                        REQUIRE(captured);
                        REQUIRE(state_sync::restore_phase(weapon.data(), profile, *captured));
                        CHECK(weapon == restored_weapon);
                    }
                }
            }
        }
    }
}

TEST_CASE("Soldier melee phase preserves following fields and discards state without a compatible receiver profile",
          "[soldier_state_sync]") {
    for (const bool alignment : {false, true}) {
        for (const bool deflect : {false, true}) {
            const state_sync::PhaseProfile sender{8, 12, alignment, deflect};
            const state_sync::MeleePhase phase{
                .age = std::bit_cast<std::uint32_t>(0.3F),
                .previous = 7,
                .previous_duration = std::bit_cast<std::uint32_t>(0.3125F),
                .clock = std::bit_cast<std::uint32_t>(0.4F),
                .blend = std::bit_cast<std::uint32_t>(0.375F),
                .events = {{{0x10203040, 3, 1}}},
                .count = 1,
                .held = 0xFE,
                .gravity = true,
                .flee = true,
                .alignment_z = std::bit_cast<std::uint32_t>(0.75F),
                .alignment_x = std::bit_cast<std::uint32_t>(-0.5F),
                .deflect_age = std::bit_cast<std::uint32_t>(0.125F),
                .deflect_variant = 7,
                .precise_ages = true,
                .ages = {{0.4}},
            };
            const std::array<std::optional<state_sync::PhaseProfile>, 4> receivers{
                std::nullopt, state_sync::PhaseProfile{8, 12, !alignment, deflect}, state_sync::PhaseProfile{8, 12, alignment, !deflect},
                state_sync::PhaseProfile{2, 4, alignment, deflect}};
            for (const auto &receiver : receivers) {
                std::vector<bool> stream{true, false, true};
                const auto put = [&stream](std::uint32_t value, std::int32_t bits) noexcept {
                    for (int i = 0; i < bits; ++i) {
                        stream.push_back(((value >> i) & 1) != 0);
                    }
                };
                state_sync::write_phase(sender, phase, put);
                const auto phase_end = stream.size();
                put(0xFA7C9013, 32);
                std::size_t cursor = 3;
                const auto get = [&stream, &cursor](std::int32_t bits) noexcept {
                    std::uint32_t value{};
                    for (int i = 0; i < bits; ++i) {
                        value |= static_cast<std::uint32_t>(stream[cursor++]) << i;
                    }
                    return value;
                };

                const auto received = state_sync::read_phase(get);
                REQUIRE(received);
                CHECK(cursor == phase_end);
                CHECK(get(32) == 0xFA7C9013);
                CHECK(cursor == stream.size());

                // Missing metadata, optional-group differences and invalid previous states leave native storage untouched.
                std::array<std::byte, 0x1A0> weapon;
                weapon.fill(std::byte{0xA5});
                const auto before = weapon;
                const bool restored = receiver && state_sync::restore_phase(weapon.data(), *receiver, *received);
                CHECK_FALSE(restored);
                CHECK(weapon == before);
            }
        }
    }
}
