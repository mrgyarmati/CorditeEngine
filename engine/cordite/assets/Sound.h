#pragma once

/// @file
/// A sound as an asset: samples, as a mixer plays them.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::vector<std::byte> file, readBinaryFile(path));
/// CORDITE_TRY_ASSIGN(const SoundData click, decodeSound(file));          // WAV, FLAC or MP3
/// CORDITE_LOG_INFO("Assets", "{:.2f} s at {} Hz", click.seconds(), click.sampleRate);
///
/// SoundData beep{.sampleRate = 48000, .channels = 1};                    // a second of 440 Hz
/// for (uint32_t frame = 0; frame < beep.sampleRate; ++frame) {
///     beep.samples.push_back(0.5f * std::sin(kTwoPi * 440.0f * static_cast<float>(frame) / 48000.0f));
/// }
/// CORDITE_TRY_ASSIGN(const std::vector<std::byte> wav, encodeWav(beep));
/// CORDITE_TRY(writeBinaryFile(directory / "beep.wav", wav));
/// @endcode
///
/// A sound is decoded once, all of it, when it is loaded: what plays it afterwards reads
/// memory and never a file. That is right for effects, which are short and played often,
/// and costs memory for music - a minute of stereo at 48 kHz is 23 MB. Samples are floats
/// from -1 to 1, the channels of a frame side by side: left, then right.
///
/// Files are untrusted. What a file says of itself is not believed where it decides how
/// much memory is taken: decoding counts what it is given and refuses a sound of more than
/// SoundDecodeOptions::maxFrames - refuses, not cuts. A file that breaks off is as long as
/// what is there of it, as it is to every player. Decoding is miniaudio's; the files it
/// reads here are WAV, FLAC and MP3.

#include "cordite/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// The most a sound file may hold, in bytes: hours of music, far more than any effect.
inline constexpr size_t kMaxSoundFileBytes = size_t{256} * 1024 * 1024;

/// The most frames a sound is decoded to unless asked otherwise: ten minutes at 48 kHz,
/// which in stereo is 230 MB of samples.
inline constexpr uint64_t kMaxSoundFrames = uint64_t{48000} * 60 * 10;

/// The rates a sound may have, in frames per second: from what a telephone carries to
/// more than anything records. The same limits as the mixer's.
inline constexpr uint32_t kMinSoundSampleRate = 8000;
inline constexpr uint32_t kMaxSoundSampleRate = 384000;

struct SoundData {
    /// Frames per second, from kMinSoundSampleRate to kMaxSoundSampleRate.
    uint32_t sampleRate = 48000;
    /// 1 for mono, 2 for stereo. A file of more channels is mixed down to two.
    uint32_t channels = 1;
    /// `channels` values for every frame, each from -1 to 1.
    std::vector<float> samples;

    /// How many frames there are; what is left over of a frame is not counted.
    [[nodiscard]] size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }

    /// How long the sound lasts.
    [[nodiscard]] double seconds() const {
        return sampleRate == 0 ? 0.0 : static_cast<double>(frames()) / static_cast<double>(sampleRate);
    }

    bool operator==(const SoundData&) const = default;
};

/// Whether a sound is one that can be played and written: mono or stereo, at a rate within
/// the limits, at least one frame and no part of another, every sample a number. Fails
/// with InvalidArgument saying which of these it is not. Samples beyond -1 and 1 are
/// allowed: they are what a mixer clips. What decodeSound() returns always passes.
[[nodiscard]] Status validateSound(const SoundData& sound);

struct SoundDecodeOptions {
    /// A sound of more frames than this is refused.
    uint64_t maxFrames = kMaxSoundFrames;
};

/// Decodes what a sound file holds - WAV, FLAC or MP3, told apart by what is in it and not
/// by a name - at the rate the file has, in one or two channels. Fails with ParseError
/// for bytes that are none of the three or that the decoder gives up on, and with
/// OutOfRange for a sound of no frames, of more than `options.maxFrames`, or at a rate
/// outside the limits. Samples that are no numbers, which a file of floats can hold,
/// become silence.
[[nodiscard]] Result<SoundData> decodeSound(std::span<const std::byte> encoded, const SoundDecodeOptions& options = {});

/// Writes a sound as a WAV file of 16 bits a sample, which everything reads. Values
/// beyond -1 and 1 are clipped. The same sound gives the same bytes every time. Fails as
/// validateSound() does, and with OutOfRange for a sound that a WAV file cannot hold,
/// which is 4 GB.
[[nodiscard]] Result<std::vector<std::byte>> encodeWav(const SoundData& sound);

} // namespace cordite
