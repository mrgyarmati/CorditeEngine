#include "cordite/assets/Sound.h"

#include "cordite/core/Base.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace cordite {
namespace {

/// How many frames are asked of the decoder at a time.
constexpr size_t kChunkFrames = 4096;

/// What a WAV file of 16-bit samples has before them.
constexpr size_t kWavHeaderBytes = 44;

/// Ends a decoder when it goes out of scope.
class Decoder {
public:
    Decoder() = default;

    ~Decoder() { close(); }

    CORDITE_NON_MOVABLE(Decoder);

    /// Opens `encoded` for decoding to floats, at the rate of the file, in `channels`
    /// channels - or, for 0, in as many as the file has.
    [[nodiscard]] ma_result open(std::span<const std::byte> encoded, ma_uint32 channels) {
        close();
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, 0);
        const ma_result result = ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &m_decoder);
        m_open = result == MA_SUCCESS;
        return result;
    }

    void close() {
        if (m_open) {
            ma_decoder_uninit(&m_decoder);
            m_open = false;
        }
    }

    [[nodiscard]] ma_decoder& get() { return m_decoder; }

private:
    ma_decoder m_decoder{};
    bool m_open = false;
};

/// A file being written into memory. The sizes in the header of a WAV file are written
/// last, so the encoder goes back: `position` is where the next byte goes.
struct MemoryFile {
    std::vector<std::byte> bytes;
    size_t position = 0;
};

ma_result writeToMemory(ma_encoder* encoder, const void* buffer, size_t count, size_t* written) {
    auto* file = static_cast<MemoryFile*>(encoder->pUserData);
    if (file->position + count > file->bytes.size()) {
        file->bytes.resize(file->position + count);
    }
    if (count > 0) {
        std::memcpy(file->bytes.data() + file->position, buffer, count);
    }
    file->position += count;
    *written = count;
    return MA_SUCCESS;
}

ma_result seekInMemory(ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) {
    auto* file = static_cast<MemoryFile*>(encoder->pUserData);
    ma_int64 base = 0;
    if (origin == ma_seek_origin_current) {
        base = static_cast<ma_int64>(file->position);
    } else if (origin == ma_seek_origin_end) {
        base = static_cast<ma_int64>(file->bytes.size());
    }
    const ma_int64 target = base + offset;
    if (target < 0 || target > static_cast<ma_int64>(file->bytes.size())) {
        return MA_BAD_SEEK;
    }
    file->position = static_cast<size_t>(target);
    return MA_SUCCESS;
}

bool isSoundSampleRate(uint32_t sampleRate) {
    return sampleRate >= kMinSoundSampleRate && sampleRate <= kMaxSoundSampleRate;
}

} // namespace

Status validateSound(const SoundData& sound) {
    if (sound.channels != 1 && sound.channels != 2) {
        return makeError(ErrorCode::InvalidArgument, "a sound has one channel or two, not {}", sound.channels);
    }
    if (!isSoundSampleRate(sound.sampleRate)) {
        return makeError(ErrorCode::InvalidArgument, "a sound has a rate of {} to {} frames a second, not {}",
                         kMinSoundSampleRate, kMaxSoundSampleRate, sound.sampleRate);
    }
    if (sound.samples.empty() || sound.samples.size() % sound.channels != 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "a sound is whole frames, and at least one: {} samples in {} channels are not",
                         sound.samples.size(), sound.channels);
    }
    for (size_t index = 0; index < sound.samples.size(); ++index) {
        if (!std::isfinite(sound.samples[index])) {
            return makeError(ErrorCode::InvalidArgument, "sample {} of the sound is no number", index);
        }
    }
    return {};
}

Result<SoundData> decodeSound(std::span<const std::byte> encoded, const SoundDecodeOptions& options) {
    if (encoded.empty()) {
        return makeError(ErrorCode::ParseError, "the file is empty");
    }
    Decoder decoder;
    ma_result opened = decoder.open(encoded, 0);
    // A file of more channels than two is opened again, to be mixed down to two.
    if (opened == MA_SUCCESS && decoder.get().outputChannels > 2) {
        opened = decoder.open(encoded, 2);
    }
    if (opened != MA_SUCCESS) {
        return makeError(ErrorCode::ParseError, "it is no WAV, FLAC or MP3 file that can be read: {}",
                         ma_result_description(opened));
    }

    SoundData sound;
    sound.sampleRate = decoder.get().outputSampleRate;
    sound.channels = decoder.get().outputChannels;
    if (sound.channels != 1 && sound.channels != 2) {
        return makeError(ErrorCode::ParseError, "it says it has {} channels", sound.channels);
    }
    if (!isSoundSampleRate(sound.sampleRate)) {
        return makeError(ErrorCode::OutOfRange, "it has {} frames a second; a sound has {} to {}", sound.sampleRate,
                         kMinSoundSampleRate, kMaxSoundSampleRate);
    }

    // Read until the decoder has no more, a chunk at a time: what a file says of its
    // length is not what decides how much memory is taken.
    std::vector<float> chunk(kChunkFrames * sound.channels);
    uint64_t frames = 0;
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder.get(), chunk.data(), kChunkFrames, &read);
        if (read > kChunkFrames) {
            return makeError(ErrorCode::ParseError, "the decoder returned more than it was asked for");
        }
        if (read > 0) {
            frames += read;
            if (frames > options.maxFrames) {
                return makeError(ErrorCode::OutOfRange, "it has more than the {} frames a sound may have",
                                 options.maxFrames);
            }
            const auto values = static_cast<ptrdiff_t>(read * sound.channels);
            sound.samples.insert(sound.samples.end(), chunk.begin(), chunk.begin() + values);
        }
        if (result == MA_AT_END || (result == MA_SUCCESS && read == 0)) {
            break;
        }
        if (result != MA_SUCCESS) {
            return makeError(ErrorCode::ParseError, "it cannot be decoded past frame {}: {}", frames,
                             ma_result_description(result));
        }
    }
    if (frames == 0) {
        return makeError(ErrorCode::OutOfRange, "it holds no sound: not one frame");
    }
    // A file of floats can hold anything; nothing that is no number goes to a mixer.
    for (float& sample : sound.samples) {
        if (!std::isfinite(sample)) {
            sample = 0.0f;
        }
    }
    return sound;
}

Result<std::vector<std::byte>> encodeWav(const SoundData& sound) {
    CORDITE_TRY(validateSound(sound));
    // The sizes in the header of a WAV file are 32 bits.
    if (sound.samples.size() > (std::numeric_limits<uint32_t>::max() - kWavHeaderBytes) / sizeof(int16_t)) {
        return makeError(ErrorCode::OutOfRange, "{} samples are more than a WAV file holds", sound.samples.size());
    }

    std::vector<int16_t> samples;
    samples.reserve(sound.samples.size());
    for (const float sample : sound.samples) {
        samples.push_back(static_cast<int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)));
    }

    MemoryFile file;
    file.bytes.reserve(kWavHeaderBytes + samples.size() * sizeof(int16_t));
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, sound.channels, sound.sampleRate);
    ma_encoder encoder;
    if (const ma_result opened = ma_encoder_init(writeToMemory, seekInMemory, &file, &config, &encoder);
        opened != MA_SUCCESS) {
        return makeError(ErrorCode::IoError, "the WAV file could not be begun: {}", ma_result_description(opened));
    }
    ma_uint64 written = 0;
    const ma_result result = ma_encoder_write_pcm_frames(&encoder, samples.data(), sound.frames(), &written);
    // Ending it is what writes the sizes into the header.
    ma_encoder_uninit(&encoder);
    if (result != MA_SUCCESS || written != sound.frames()) {
        return makeError(ErrorCode::IoError, "the WAV file could not be written: {}", ma_result_description(result));
    }
    return std::move(file.bytes);
}

} // namespace cordite
