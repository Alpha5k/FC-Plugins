#pragma once

#include "transport.hpp"

#include <fc/native.hpp>
#include <fc/plugin.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

struct OpusDecoder;
struct OpusEncoder;

namespace fc_plugins::voice_chat {

// Reports whether one native codec call is still assembling a frame, finished
// a complete Opus frame, or failed before producing usable output.
enum class CodecProgress {
    Pending,
    Complete,
    Failed,
};

// Assembles native 160-sample or 33-byte blocks into prepared 20 ms Opus frames.
// Calls for one native state must not overlap, including reset and release.
class CodecPool final {
  public:
    enum Issue : std::uint32_t {
        NoEncoder = 1U << 0,
        NoDecoder = 1U << 1,
        EncodeFailed = 1U << 2,
        DecodeFailed = 1U << 3,
    };

    CodecPool() = default;

    // Releases every prepared Opus state; callers must stop native callbacks before destruction.
    ~CodecPool();

    CodecPool(const CodecPool &) = delete;
    CodecPool &operator=(const CodecPool &) = delete;

    // Allocates every Opus state; call once before native audio callbacks begin.
    fc::Result<> prepare();

    // Resets matching prediction history and incomplete blocks, retaining the native address claim.
    void reset(void *native_state) noexcept;

    // Clears prediction and borrowed outputs, then frees the prepared association after native callbacks stop.
    void release(void *native_state) noexcept;

    // Copies each 160-sample input and retains its 33-byte output buffer until the sixth call fills all six blocks.
    CodecProgress encode(void *native_state, const std::int16_t *pcm, std::byte *encoded) noexcept;

    // Copies each 33-byte input and retains its 160-sample output buffer until the sixth call fills all six blocks.
    CodecProgress decode(void *native_state, const std::byte *encoded, std::int16_t *pcm) noexcept;

    // Returns and clears issue bits without logging on the sound thread.
    std::uint32_t take_issues() noexcept;

    // Retains the first failed codec return value for the once-per-lifetime control-thread diagnostic.
    int encode_result() const noexcept {
        return encode_result_.load(std::memory_order_relaxed);
    }

    int decode_result() const noexcept {
        return decode_result_.load(std::memory_order_relaxed);
    }

  private:
    struct EncoderSlot {
        // Native stream close releases this association without destroying the prepared codec.
        std::atomic<void *> native_state{};
        OpusEncoder *opus{};
        std::array<std::int16_t, kFrameSamples> pcm{};
        std::array<std::byte, kMaximumPayloadBytes> payload{};
        // Borrowed native packet blocks; reset or frame completion clears them.
        std::array<std::byte *, kBlocksPerFrame> outputs{};
        // Number of 160-sample inputs collected for the current 960-sample frame.
        std::size_t block_count{};
    };

    struct DecoderSlot {
        // Native nested decoder close releases this association before another stream can claim it.
        std::atomic<void *> native_state{};
        OpusDecoder *opus{};
        NativeFrame frame{};
        std::array<std::int16_t, kFrameSamples> pcm{};
        // Borrowed native PCM blocks; the surrounding stream must keep them alive through completion.
        std::array<std::int16_t *, kBlocksPerFrame> outputs{};
        // Number of 33-byte inputs collected for the current 198-byte frame.
        std::size_t block_count{};
    };

    static constexpr std::size_t kEncoderSlots = 2;
    static constexpr std::size_t kDecoderSlots = 32;

    // Publishes an issue bit for the serialized control-thread logger.
    void report(Issue issue, int result = 0) noexcept;

    std::array<EncoderSlot, kEncoderSlots> encoders_{};
    std::array<DecoderSlot, kDecoderSlots> decoders_{};
    // Sound-thread failures are accumulated for logging on the serialized control thread.
    std::atomic<std::uint32_t> issues_{};
    std::atomic<int> encode_result_{std::numeric_limits<int>::max()};
    std::atomic<int> decode_result_{std::numeric_limits<int>::max()};
};

} // namespace fc_plugins::voice_chat
