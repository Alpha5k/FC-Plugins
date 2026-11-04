#include "codec.hpp"

#include <opus.h>

#include <algorithm>
#include <cstring>
#include <expected>
#include <format>

namespace fc_plugins::voice_chat {
namespace {

constexpr std::int32_t kSampleRate = 48'000;
constexpr std::int32_t kBitrate = 48'000;
constexpr std::int32_t kExpectedLossPercent = 10;

// Reuses an active native address or associates it with an empty prepared slot until native close.
template <class Slot, std::size_t Size> Slot *find_or_claim(std::array<Slot, Size> &slots, void *native_state) noexcept {
    if (native_state == nullptr) {
        return nullptr;
    }

    // Established native streams must retain their Opus prediction state.
    for (auto &slot : slots) {
        if (slot.native_state.load(std::memory_order_acquire) == native_state) {
            return &slot;
        }
    }

    // Only address claims are atomic; callers must serialize each slot's codec and block buffers.
    for (auto &slot : slots) {
        void *empty{};
        if (slot.native_state.compare_exchange_strong(empty, native_state, std::memory_order_acq_rel)) {
            return &slot;
        }
    }

    return nullptr;
}

// Sets the mono speech encoder's bitrate, full-band maximum and in-band FEC for expected loss.
bool configure_encoder(OpusEncoder *encoder, int &error) noexcept {
    return (error = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kBitrate))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_VBR(1))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_LSB_DEPTH(16))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(kExpectedLossPercent))) == OPUS_OK &&
           (error = opus_encoder_ctl(encoder, OPUS_SET_DTX(0))) == OPUS_OK;
}

// Enables Opus deep packet-loss concealment and blind bandwidth extension.
bool configure_decoder(OpusDecoder *decoder, int &error) noexcept {
    // Complexity 10 selects the compiled Deep PLC and NoLACE paths.
    return (error = opus_decoder_ctl(decoder, OPUS_SET_COMPLEXITY(10))) == OPUS_OK &&
           (error = opus_decoder_ctl(decoder, OPUS_SET_OSCE_BWE(1))) == OPUS_OK;
}

} // namespace

CodecPool::~CodecPool() {
    for (auto &slot : encoders_) {
        opus_encoder_destroy(slot.opus);
    }
    for (auto &slot : decoders_) {
        opus_decoder_destroy(slot.opus);
    }
}

fc::Result<> CodecPool::prepare() {
    // Allocate and configure every codec before the sound callbacks need a slot.
    for (auto &slot : encoders_) {
        int error{};
        slot.opus = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
        if (slot.opus == nullptr || error != OPUS_OK || !configure_encoder(slot.opus, error)) {
            return std::unexpected(fc::Error{
                .message = std::format("Opus could not prepare the voice encoder: {} ({})", opus_strerror(error), error),
            });
        }
    }

    for (auto &slot : decoders_) {
        int error{};
        slot.opus = opus_decoder_create(kSampleRate, 1, &error);
        if (slot.opus == nullptr || error != OPUS_OK || !configure_decoder(slot.opus, error)) {
            return std::unexpected(fc::Error{
                .message = std::format("Opus could not prepare a voice decoder: {} ({})", opus_strerror(error), error),
            });
        }
    }
    return {};
}

void CodecPool::reset(void *native_state) noexcept {
    // Reset clears prediction history and borrowed outputs, but keeps each native address claim.
    for (auto &slot : encoders_) {
        if (slot.native_state.load(std::memory_order_acquire) == native_state) {
            opus_encoder_ctl(slot.opus, OPUS_RESET_STATE);
            slot.block_count = 0;
            slot.outputs.fill(nullptr);
        }
    }

    for (auto &slot : decoders_) {
        if (slot.native_state.load(std::memory_order_acquire) == native_state) {
            opus_decoder_ctl(slot.opus, OPUS_RESET_STATE);
            slot.block_count = 0;
            slot.outputs.fill(nullptr);
        }
    }
}

void CodecPool::release(void *native_state) noexcept {
    if (native_state == nullptr) {
        return;
    }

    // Native close has stopped this state's callbacks; discard borrowed buffers before publishing the slot as free.
    reset(native_state);
    for (auto &slot : encoders_) {
        if (slot.native_state.load(std::memory_order_acquire) == native_state) {
            slot.native_state.store(nullptr, std::memory_order_release);
        }
    }
    for (auto &slot : decoders_) {
        if (slot.native_state.load(std::memory_order_acquire) == native_state) {
            slot.native_state.store(nullptr, std::memory_order_release);
        }
    }
}

CodecProgress CodecPool::encode(void *native_state, const std::int16_t *pcm, std::byte *encoded) noexcept {
    if (pcm == nullptr || encoded == nullptr) {
        report(EncodeFailed, OPUS_BAD_ARG);
        return CodecProgress::Failed;
    }

    auto *slot = find_or_claim(encoders_, native_state);
    if (slot == nullptr) {
        std::memset(encoded, 0, kNativeBytes);
        report(NoEncoder);
        return CodecProgress::Failed;
    }

    // A native GSM call supplies 160 samples. Six calls collect one 20 ms frame at 48 kHz.
    const auto block = slot->block_count;
    std::memcpy(slot->pcm.data() + block * kNativeSamples, pcm, kNativeSamples * sizeof(std::int16_t));
    slot->outputs[block] = encoded;
    std::memset(encoded, 0, kNativeBytes);
    ++slot->block_count;
    if (slot->block_count != kBlocksPerFrame) {
        return CodecProgress::Pending;
    }

    // Normal aligned stream updates retain all six output buffers until the final callback returns.
    const auto length = opus_encode(slot->opus, slot->pcm.data(), static_cast<int>(kFrameSamples),
                                    reinterpret_cast<unsigned char *>(slot->payload.data()), static_cast<opus_int32>(slot->payload.size()));
    NativeFrame frame{};
    const auto valid = length > 0 && build_frame(frame, std::span{slot->payload}.first(length));
    if (!valid) {
        report(EncodeFailed, length);
    }

    for (std::size_t index = 0; index < kBlocksPerFrame; ++index) {
        std::memcpy(slot->outputs[index], frame.data() + index * kNativeBytes, kNativeBytes);
    }
    slot->block_count = 0;

    // Completion releases the borrowed output pointers before the native packet is published.
    slot->outputs.fill(nullptr);
    return valid ? CodecProgress::Complete : CodecProgress::Failed;
}

CodecProgress CodecPool::decode(void *native_state, const std::byte *encoded, std::int16_t *pcm) noexcept {
    if (encoded == nullptr || pcm == nullptr) {
        report(DecodeFailed, OPUS_BAD_ARG);
        return CodecProgress::Failed;
    }

    auto *slot = find_or_claim(decoders_, native_state);
    if (slot == nullptr) {
        std::memset(pcm, 0, kNativeSamples * sizeof(std::int16_t));
        report(NoDecoder);
        return CodecProgress::Failed;
    }

    // Native GSM input arrives in 33-byte blocks; decode only when all six form the internal frame.
    const auto block = slot->block_count;
    std::memcpy(slot->frame.data() + block * kNativeBytes, encoded, kNativeBytes);
    slot->outputs[block] = pcm;
    std::memset(pcm, 0, kNativeSamples * sizeof(std::int16_t));
    ++slot->block_count;
    if (slot->block_count != kBlocksPerFrame) {
        return CodecProgress::Pending;
    }

    const auto frame = parse_native_frame(slot->frame);
    int samples = OPUS_INVALID_PACKET;
    if (frame) {
        // PLC passes a null payload to synthesize loss; FEC uses the next packet to recover its predecessor.
        const auto *payload = frame->mode == FrameMode::Plc ? nullptr : reinterpret_cast<const unsigned char *>(frame->payload.data());
        const auto length = frame->mode == FrameMode::Plc ? 0 : static_cast<opus_int32>(frame->payload.size());
        samples = opus_decode(slot->opus, payload, length, slot->pcm.data(), static_cast<int>(kFrameSamples),
                              frame->mode == FrameMode::Fec ? 1 : 0);
    }

    if (samples != static_cast<int>(kFrameSamples)) {
        slot->pcm.fill(0);
        report(DecodeFailed, samples);
    }

    // Normal aligned stream updates publish after all six calls, allowing these borrowed PCM blocks to be filled here.
    for (std::size_t index = 0; index < kBlocksPerFrame; ++index) {
        std::memcpy(slot->outputs[index], slot->pcm.data() + index * kNativeSamples, kNativeSamples * sizeof(std::int16_t));
    }
    slot->block_count = 0;
    slot->outputs.fill(nullptr);
    return samples == static_cast<int>(kFrameSamples) ? CodecProgress::Complete : CodecProgress::Failed;
}

std::uint32_t CodecPool::take_issues() noexcept {
    return issues_.exchange(0, std::memory_order_acq_rel);
}

void CodecPool::report(Issue issue, int result) noexcept {
    // Publish the original codec result before its issue bit reaches the control-thread logger.
    if (issue == EncodeFailed || issue == DecodeFailed) {
        auto &stored = issue == EncodeFailed ? encode_result_ : decode_result_;
        auto empty = std::numeric_limits<int>::max();
        stored.compare_exchange_strong(empty, result, std::memory_order_relaxed);
    }
    issues_.fetch_or(issue, std::memory_order_release);
}

} // namespace fc_plugins::voice_chat
