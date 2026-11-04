#include "../patches/voice_chat/codec.hpp"
#include "../patches/voice_chat/transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace voice = fc_plugins::voice_chat;

TEST_CASE("Voice chat joins the six native codec calls") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare().has_value());

    // The deterministic waveform proves that six native blocks become one
    // 20 ms Opus frame and return through six separate output pointers.
    std::array<std::int16_t, voice::kFrameSamples> input{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto phase = static_cast<std::int32_t>(index % 240);
        input[index] = static_cast<std::int16_t>((phase < 120 ? phase : 240 - phase) * 160 - 9'600);
    }

    voice::NativeFrame encoded{};
    std::array<std::int16_t, voice::kFrameSamples> decoded{};
    auto encoder_state = std::uint32_t{1};
    auto decoder_state = std::uint32_t{2};
    auto progress = voice::CodecProgress::Pending;
    for (std::size_t block = 0; block < voice::kBlocksPerFrame; ++block) {
        progress =
            codecs.encode(&encoder_state, input.data() + block * voice::kNativeSamples, encoded.data() + block * voice::kNativeBytes);
    }
    REQUIRE(progress == voice::CodecProgress::Complete);
    REQUIRE(voice::parse_native_frame(encoded).has_value());
    for (std::size_t block = 0; block < voice::kBlocksPerFrame; ++block) {
        progress =
            codecs.decode(&decoder_state, encoded.data() + block * voice::kNativeBytes, decoded.data() + block * voice::kNativeSamples);
    }
    REQUIRE(progress == voice::CodecProgress::Complete);

    CHECK(std::ranges::any_of(decoded, [](std::int16_t sample) {
        return sample != 0;
    }));

    // Local recovery frames exercise the decoder's in-band FEC and PLC modes
    // without adding another transport representation.
    const auto recovery = voice::fec_frame(encoded);
    REQUIRE(recovery.has_value());
    const auto concealment = voice::plc_frame();
    for (const auto *frame : {&*recovery, &concealment}) {
        for (std::size_t block = 0; block < voice::kBlocksPerFrame; ++block) {
            progress =
                codecs.decode(&decoder_state, frame->data() + block * voice::kNativeBytes, decoded.data() + block * voice::kNativeSamples);
        }
        REQUIRE(progress == voice::CodecProgress::Complete);
    }
    CHECK(codecs.take_issues() == 0);
}

TEST_CASE("Voice codec reset discards incomplete blocks and malformed frames are silenced", "[voice]") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare());
    std::uint32_t state{7};
    std::array<std::int16_t, voice::kFrameSamples> pcm{};
    voice::NativeFrame malformed{};
    std::ranges::fill(pcm, std::int16_t{1234});
    CHECK(codecs.decode(&state, malformed.data(), pcm.data()) == voice::CodecProgress::Pending);
    codecs.reset(&state);
    for (std::size_t block = 0; block < voice::kBlocksPerFrame; ++block) {
        const auto progress =
            codecs.decode(&state, malformed.data() + block * voice::kNativeBytes, pcm.data() + block * voice::kNativeSamples);
        CHECK(progress == (block + 1 == voice::kBlocksPerFrame ? voice::CodecProgress::Failed : voice::CodecProgress::Pending));
    }
    CHECK(std::ranges::all_of(pcm, [](auto sample) {
        return sample == 0;
    }));
    CHECK(codecs.take_issues() == voice::CodecPool::DecodeFailed);
}

TEST_CASE("Voice codec refuses an unowned native state without exposing stale output", "[voice]") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare());
    std::array<std::int16_t, voice::kNativeSamples> pcm{};
    std::array<std::byte, voice::kNativeBytes> encoded{};
    std::ranges::fill(encoded, std::byte{0x7F});
    CHECK(codecs.encode(nullptr, pcm.data(), encoded.data()) == voice::CodecProgress::Failed);
    CHECK(std::ranges::all_of(encoded, [](auto byte) {
        return byte == std::byte{};
    }));
    CHECK(codecs.take_issues() == voice::CodecPool::NoEncoder);
}

TEST_CASE("Voice codec reset retains decoder claims across distinct native addresses", "[voice]") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare());
    std::array<std::uint32_t, 33> states{};
    const auto concealment = voice::plc_frame();
    std::array<std::int16_t, voice::kNativeSamples> output{};
    for (std::size_t index = 0; index < 32; ++index) {
        CHECK(codecs.decode(&states[index], concealment.data(), output.data()) == voice::CodecProgress::Pending);
        codecs.reset(&states[index]);
    }

    // Reset ends an incomplete frame but currently keeps all 32 native address claims.
    CHECK(codecs.decode(&states.back(), concealment.data(), output.data()) == voice::CodecProgress::Failed);
    CHECK(codecs.take_issues() == voice::CodecPool::NoDecoder);
}

TEST_CASE("Closed native voice streams release prepared codecs and borrowed outputs", "[voice]") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare());
    std::array<std::uint32_t, 40> states{};
    const auto concealment = voice::plc_frame();
    std::array<std::int16_t, voice::kFrameSamples> output{};
    std::array<std::int16_t, voice::kNativeSamples> retired_output{};
    std::array<std::byte, voice::kNativeBytes> retired_encoded{};
    voice::NativeFrame encoded{};

    for (auto &state : states) {
        CHECK(codecs.decode(&state, concealment.data(), retired_output.data()) == voice::CodecProgress::Pending);
        CHECK(codecs.encode(&state, output.data(), retired_encoded.data()) == voice::CodecProgress::Pending);
        codecs.release(&state);
    }
    CHECK(codecs.take_issues() == 0);

    // A later frame must not write any retained output from a destroyed stream.
    retired_output.fill(1234);
    retired_encoded.fill(std::byte{0x71});
    for (std::size_t block = 0; block < voice::kBlocksPerFrame; ++block) {
        const auto expected = block + 1 == voice::kBlocksPerFrame ? voice::CodecProgress::Complete : voice::CodecProgress::Pending;
        CHECK(codecs.decode(&states[0], concealment.data() + block * voice::kNativeBytes, output.data() + block * voice::kNativeSamples) ==
              expected);
        CHECK(codecs.encode(&states[0], output.data() + block * voice::kNativeSamples, encoded.data() + block * voice::kNativeBytes) ==
              expected);
    }
    CHECK(std::ranges::all_of(retired_output, [](auto sample) {
        return sample == 1234;
    }));
    CHECK(std::ranges::all_of(retired_encoded, [](auto byte) {
        return byte == std::byte{0x71};
    }));
    CHECK(codecs.take_issues() == 0);
}

TEST_CASE("Closing another voice stream preserves an active decoder's incomplete frame", "[voice]") {
    voice::CodecPool codecs;
    REQUIRE(codecs.prepare());
    std::uint32_t active{};
    std::uint32_t closing{};
    const auto concealment = voice::plc_frame();
    std::array<std::int16_t, voice::kFrameSamples> output{};
    std::array<std::int16_t, voice::kNativeSamples> other_output{};
    REQUIRE(codecs.decode(&active, concealment.data(), output.data()) == voice::CodecProgress::Pending);
    REQUIRE(codecs.decode(&closing, concealment.data(), other_output.data()) == voice::CodecProgress::Pending);
    codecs.release(&closing);
    for (std::size_t block = 1; block < voice::kBlocksPerFrame; ++block) {
        const auto expected = block + 1 == voice::kBlocksPerFrame ? voice::CodecProgress::Complete : voice::CodecProgress::Pending;
        CHECK(codecs.decode(&active, concealment.data() + block * voice::kNativeBytes, output.data() + block * voice::kNativeSamples) ==
              expected);
    }
    CHECK(codecs.take_issues() == 0);
}
