#include "cordite/assets/Sound.h"

#include "cordite/math/Random.h"

#include "AssetTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

/// A file put together a byte at a time, for the files nothing here can write.
class FileBuilder {
public:
    FileBuilder& text(std::string_view characters) {
        for (const char character : characters) {
            byte(static_cast<uint8_t>(character));
        }
        return *this;
    }

    FileBuilder& byte(uint8_t value) {
        m_bytes.push_back(static_cast<std::byte>(value));
        return *this;
    }

    FileBuilder& zeros(size_t count) {
        m_bytes.insert(m_bytes.end(), count, std::byte{0});
        return *this;
    }

    /// Whole numbers of `count` bytes, lowest first - as WAV files have them.
    FileBuilder& little(uint64_t value, uint32_t count) {
        for (uint32_t index = 0; index < count; ++index) {
            byte(static_cast<uint8_t>(value >> (8U * index)));
        }
        return *this;
    }

    /// Whole numbers of `count` bytes, highest first - as FLAC files have them.
    FileBuilder& big(uint64_t value, uint32_t count) {
        for (uint32_t index = count; index > 0; --index) {
            byte(static_cast<uint8_t>(value >> (8U * (index - 1))));
        }
        return *this;
    }

    FileBuilder& number(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return little(bits, 4);
    }

    FileBuilder& append(std::span<const std::byte> bytes) {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
        return *this;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

struct WavHeader {
    /// 1 for whole numbers, 3 for floats.
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t sampleRate = 48000;
    uint16_t bitsPerSample = 16;
};

/// A WAV file with `data` as its samples, saying of them that they are `declaredBytes`.
std::vector<std::byte> wavFile(const WavHeader& header, std::span<const std::byte> data, uint32_t declaredBytes) {
    const auto frameBytes = static_cast<uint16_t>(header.channels * (header.bitsPerSample / 8));
    FileBuilder file;
    file.text("RIFF").little(uint64_t{36} + declaredBytes, 4).text("WAVE");
    file.text("fmt ").little(16, 4).little(header.format, 2).little(header.channels, 2);
    file.little(header.sampleRate, 4).little(uint64_t{header.sampleRate} * frameBytes, 4);
    file.little(frameBytes, 2).little(header.bitsPerSample, 2);
    file.text("data").little(declaredBytes, 4).append(data);
    return file.bytes();
}

std::vector<std::byte> wavFile(const WavHeader& header, std::span<const std::byte> data) {
    return wavFile(header, data, static_cast<uint32_t>(data.size()));
}

/// Samples of 16 bits, as a WAV file of whole numbers holds them.
std::vector<std::byte> samples16(std::initializer_list<int16_t> values) {
    FileBuilder data;
    for (const int16_t value : values) {
        data.little(static_cast<uint16_t>(value), 2);
    }
    return data.bytes();
}

/// The checksum a FLAC frame has of its header: 8 bits, of the polynomial x8 + x2 + x + 1.
uint8_t flacCrc8(std::span<const std::byte> bytes) {
    uint8_t crc = 0;
    for (const std::byte value : bytes) {
        crc ^= static_cast<uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>((crc & 0x80U) != 0 ? (crc << 1U) ^ 0x07U : crc << 1U);
        }
    }
    return crc;
}

/// The checksum a FLAC frame ends with: 16 bits, of the polynomial x16 + x15 + x2 + 1.
uint16_t flacCrc16(std::span<const std::byte> bytes) {
    uint16_t crc = 0;
    for (const std::byte value : bytes) {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(value) << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint16_t>((crc & 0x8000U) != 0 ? (crc << 1U) ^ 0x8005U : crc << 1U);
        }
    }
    return crc;
}

/// A FLAC file of one frame whose samples are stored as they are, without any of what
/// makes FLAC small: `frames[frame][channel]`, 16 bits each. A frame of FLAC has 16 to
/// 256 of them here.
std::vector<std::byte> flacFile(uint32_t sampleRate, const std::vector<std::vector<int16_t>>& frames) {
    const size_t channels = frames.front().size();
    FileBuilder file;
    file.text("fLaC");
    // The one block of metadata, which is the last: what the stream is.
    file.byte(0x80).big(34, 3);
    file.big(frames.size(), 2).big(frames.size(), 2); // every block has this many frames
    file.big(0, 3).big(0, 3);                         // how many bytes a frame takes is not said
    file.big((uint64_t{sampleRate} << 44U) | (uint64_t{channels - 1} << 41U) | (uint64_t{15} << 36U) | frames.size(),
             8);
    file.zeros(16); // no checksum of the samples

    FileBuilder frame;
    frame.byte(0xFF).byte(0xF8);                            // a frame begins; all blocks are of one size
    frame.byte(0x60);                                       // its size follows in 8 bits; the rate is the stream's
    frame.byte(static_cast<uint8_t>((channels - 1) << 4U)); // channels side by side; bits as the stream says
    frame.byte(0x00);                                       // the first frame
    frame.byte(static_cast<uint8_t>(frames.size() - 1));
    frame.byte(flacCrc8(frame.bytes()));
    for (size_t channel = 0; channel < channels; ++channel) {
        frame.byte(0x02); // samples as they are
        for (const std::vector<int16_t>& values : frames) {
            frame.big(static_cast<uint16_t>(values[channel]), 2);
        }
    }
    frame.big(flacCrc16(frame.bytes()), 2);
    return file.append(frame.bytes()).bytes();
}

/// How many frames of sound one frame of an MP3 file of this kind holds, and how many
/// bytes it takes: 128 kbit a second of 44.1 kHz.
constexpr size_t kMp3FrameSamples = 1152;
constexpr size_t kMp3FrameBytes = 417;

/// An MP3 file of `frameCount` frames of silence in one channel: a frame that says it
/// holds nothing decodes to nothing.
std::vector<std::byte> silentMp3File(size_t frameCount) {
    FileBuilder file;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        // MPEG-1 layer 3 without checksum; 128 kbit at 44.1 kHz; one channel.
        file.byte(0xFF).byte(0xFB).byte(0x90).byte(0xC4).zeros(kMp3FrameBytes - 4);
    }
    return file.bytes();
}

/// A sound that is not the same in any two places, and uses all of the range.
SoundData sweep(uint32_t sampleRate, uint32_t channels, size_t frames) {
    SoundData sound{.sampleRate = sampleRate, .channels = channels};
    for (size_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float phase = static_cast<float>(frame * (channel + 1)) * 0.37f;
            sound.samples.push_back(std::sin(phase) * (channel == 0 ? 1.0f : 0.5f));
        }
    }
    return sound;
}

/// What a sample is after it has been written in 16 bits and read again.
float through16Bits(float sample) {
    return static_cast<float>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)) / 32768.0f;
}

/// The error of a result that has to be one.
template <typename T>
Error errorIn(const Result<T>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

constexpr float kNoNumber = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

} // namespace

TEST_SUITE("Sound") {
    TEST_CASE("a sound knows how long it is") {
        const SoundData stereo{.sampleRate = 8000, .channels = 2, .samples = std::vector<float>(4000, 0.0f)};
        CHECK(stereo.frames() == 2000);
        CHECK(stereo.seconds() == 0.25);
        // What is left over of a frame is not one.
        CHECK(SoundData{.channels = 2, .samples = {0.0f, 0.0f, 0.0f}}.frames() == 1);
        // Something that is no sound has no length, and asking is safe.
        CHECK(SoundData{.sampleRate = 0, .channels = 0, .samples = {0.0f}}.frames() == 0);
        CHECK(SoundData{.sampleRate = 0, .channels = 1, .samples = {0.0f}}.seconds() == 0.0);
        CHECK(SoundData().frames() == 0);
        CHECK(SoundData().seconds() == 0.0);
    }

    TEST_CASE("a sound that can be played is mono or stereo, at a rate, whole, and made of numbers") {
        CHECK(validateSound(sweep(48000, 1, 1)).has_value());
        CHECK(validateSound(sweep(kMinSoundSampleRate, 2, 10)).has_value());
        CHECK(validateSound(sweep(kMaxSoundSampleRate, 2, 10)).has_value());
        // Louder than a file can say is a sound all the same: it is the mixer that clips.
        CHECK(validateSound({.samples = {4.0f, -4.0f}}).has_value());

        const auto refused = [](const SoundData& sound) {
            const Error error = errorIn(validateSound(sound));
            CHECK(error.code == ErrorCode::InvalidArgument);
            return error.message;
        };
        CHECK(refused({.channels = 0, .samples = {0.0f}}) == "a sound has one channel or two, not 0");
        CHECK(refused({.channels = 3, .samples = {0.0f, 0.0f, 0.0f}}) == "a sound has one channel or two, not 3");
        CHECK(refused({.sampleRate = 0, .samples = {0.0f}}) ==
              "a sound has a rate of 8000 to 384000 frames a second, not 0");
        CHECK(refused({.sampleRate = 7999, .samples = {0.0f}}) ==
              "a sound has a rate of 8000 to 384000 frames a second, not 7999");
        CHECK(refused({.sampleRate = 384001, .samples = {0.0f}}) ==
              "a sound has a rate of 8000 to 384000 frames a second, not 384001");
        CHECK(refused({}) == "a sound is whole frames, and at least one: 0 samples in 1 channels are not");
        CHECK(refused({.channels = 2, .samples = {0.0f, 0.0f, 0.0f}}) ==
              "a sound is whole frames, and at least one: 3 samples in 2 channels are not");
        CHECK(refused({.samples = {0.0f, kNoNumber}}) == "sample 1 of the sound is no number");
        CHECK(refused({.samples = {kInfinity}}) == "sample 0 of the sound is no number");
        CHECK(refused({.channels = 2, .samples = {0.0f, 0.0f, 0.0f, -kInfinity}}) ==
              "sample 3 of the sound is no number");
    }

    TEST_CASE("a sound written as a WAV file is read back as sixteen bits hold it") {
        for (const uint32_t channels : std::initializer_list<uint32_t>{1, 2}) {
            for (const uint32_t sampleRate : std::initializer_list<uint32_t>{8000, 22050, 44100, 48000, 384000}) {
                // Fewer frames than are decoded at a time, exactly as many, and more.
                for (const size_t frames : std::initializer_list<size_t>{1, 4095, 4096, 4097, 8192, 10000}) {
                    CAPTURE(channels);
                    CAPTURE(sampleRate);
                    CAPTURE(frames);
                    const SoundData written = sweep(sampleRate, channels, frames);
                    const SoundData read = unwrap(decodeSound(unwrap(encodeWav(written))));
                    CHECK(read.sampleRate == sampleRate);
                    CHECK(read.channels == channels);
                    CHECK(read.frames() == frames);
                    REQUIRE(read.samples.size() == written.samples.size());
                    size_t different = 0;
                    for (size_t index = 0; index < read.samples.size(); ++index) {
                        different += read.samples[index] == through16Bits(written.samples[index]) ? 0 : 1;
                    }
                    CHECK(different == 0);
                    CHECK(validateSound(read).has_value());
                }
            }
        }
        // The loudest a sound can be, either way, and silence - which stays exactly that.
        const SoundData ends = unwrap(decodeSound(unwrap(encodeWav({.samples = {1.0f, -1.0f, 0.0f}}))));
        CHECK(ends.samples == std::vector<float>{32767.0f / 32768.0f, -32767.0f / 32768.0f, 0.0f});
    }

    TEST_CASE("a WAV file is written the way everything reads it") {
        const std::vector<std::byte> file =
            unwrap(encodeWav({.sampleRate = 48000, .channels = 2, .samples = {0.0f, 1.0f, -1.0f, 0.5f}}));
        const std::array<uint8_t, 52> expected = {
            0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, // "RIFF", and how much follows
            0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20, // "WAVE", "fmt "
            0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, // 16 bytes of format: whole numbers, 2 channels
            0x80, 0xBB, 0x00, 0x00, 0x00, 0xEE, 0x02, 0x00, // 48000 frames, 192000 bytes a second
            0x04, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, // 4 bytes a frame, 16 bits a sample, "data"
            0x08, 0x00, 0x00, 0x00,                         // 8 bytes of it
            0x00, 0x00, 0xFF, 0x7F, 0x01, 0x80, 0x00, 0x40, // 0, 32767, -32767, 16384
        };
        REQUIRE(file.size() == expected.size());
        for (size_t index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(static_cast<uint8_t>(file[index]) == expected[index]);
        }
        // Which is what the writer of these tests makes of it, too.
        CHECK(file == wavFile({.channels = 2}, samples16({0, 32767, -32767, 16384})));
    }

    TEST_CASE("what is louder than a file can say is clipped, and the same sound is the same file") {
        const SoundData loud{.samples = {2.0f, -3.0f, 1.0f, -1.0f, 1.0e30f, -1.0e30f}};
        const std::vector<std::byte> file = unwrap(encodeWav(loud));
        CHECK(file == wavFile({}, samples16({32767, -32767, 32767, -32767, 32767, -32767})));
        CHECK(unwrap(encodeWav(loud)) == file);
    }

    // A sound of more than a WAV file holds is refused as well. No test makes one: it
    // would be eight thousand million bytes of samples.
    TEST_CASE("a sound that cannot be played is not written either") {
        const std::vector<SoundData> unplayable = {
            SoundData{.channels = 3, .samples = {0.0f, 0.0f, 0.0f}},
            SoundData{.sampleRate = 0, .samples = {0.0f}},
            SoundData{},
            SoundData{.channels = 2, .samples = {0.0f}},
            SoundData{.samples = {kNoNumber}},
        };
        for (const SoundData& sound : unplayable) {
            const Error error = errorIn(encodeWav(sound));
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message == errorIn(validateSound(sound)).message);
        }
    }

    TEST_CASE("what is no sound file is refused, and said to be none") {
        const std::string_view none = "it is no WAV, FLAC or MP3 file that can be read: ";

        const Error empty = errorIn(decodeSound({}));
        CHECK(empty.code == ErrorCode::ParseError);
        CHECK(empty.message == "the file is empty");

        const std::string words = "this is no sound at all, it is a sentence";
        const Error text = errorIn(decodeSound(std::as_bytes(std::span(words))));
        CHECK(text.code == ErrorCode::ParseError);
        CHECK(text.message.starts_with(none));
        // What the decoder had to say comes after it.
        CHECK(text.message.size() > none.size());

        const Error picture = errorIn(decodeSound(test::encodeSolidPng(4, 4, Vec4(1.0f))));
        CHECK(picture.code == ErrorCode::ParseError);
        CHECK(picture.message.starts_with(none));

        // How each of the three begins, and nothing after it.
        for (const std::string_view beginning : {"RIFF", "RIFF\x24\0\0\0WAVE", "fLaC", "\xFF\xFB\x90\xC4", "ID3"}) {
            CAPTURE(beginning);
            const Error error = errorIn(decodeSound(std::as_bytes(std::span(beginning))));
            CHECK(error.code == ErrorCode::ParseError);
        }
    }

    TEST_CASE("a file that breaks off is as long as what is there of it") {
        const std::vector<std::byte> whole = unwrap(encodeWav(sweep(48000, 1, 100)));
        REQUIRE(whole.size() == 244);
        for (size_t length = 1; length <= whole.size(); ++length) {
            CAPTURE(length);
            const Result<SoundData> cut = decodeSound(std::span(whole).first(length));
            if (length < 44) {
                // Not even all of what says what the file is.
                CHECK(errorIn(cut).code == ErrorCode::ParseError);
            } else if (length < 46) {
                // All of that, and not one whole sample.
                CHECK(errorIn(cut).code == ErrorCode::OutOfRange);
                CHECK(errorIn(cut).message == "it holds no sound: not one frame");
            } else {
                const SoundData sound = unwrap(cut);
                CHECK(sound.frames() == (length - 44) / 2);
                CHECK(validateSound(sound).has_value());
            }
        }
    }

    TEST_CASE("a file does not decide how much memory its sound takes") {
        // Ten frames, in a file that says it holds four thousand million bytes of them.
        const SoundData honest =
            unwrap(decodeSound(wavFile({}, samples16({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 0xFFFFFFF0U)));
        CHECK(honest.frames() == 10);
        CHECK(honest.samples.back() == 10.0f / 32768.0f);

        // As long as a sound may be, and one frame longer - in the first frames that are
        // decoded, and in those that come later.
        for (const uint64_t limit : std::initializer_list<uint64_t>{1, 100, 4096, 5000}) {
            CAPTURE(limit);
            const std::vector<std::byte> fits = unwrap(encodeWav(sweep(48000, 2, static_cast<size_t>(limit))));
            const std::vector<std::byte> tooLong = unwrap(encodeWav(sweep(48000, 2, static_cast<size_t>(limit) + 1)));
            CHECK(unwrap(decodeSound(fits, {.maxFrames = limit})).frames() == limit);
            const Error error = errorIn(decodeSound(tooLong, {.maxFrames = limit}));
            CHECK(error.code == ErrorCode::OutOfRange);
            CHECK(error.message == fmt::format("it has more than the {} frames a sound may have", limit));
        }
        CHECK(errorIn(decodeSound(unwrap(encodeWav(sweep(48000, 1, 1))), {.maxFrames = 0})).code ==
              ErrorCode::OutOfRange);
        // Unless told otherwise, ten minutes of the rate most sound has.
        CHECK(SoundDecodeOptions().maxFrames == 28800000);
        CHECK(kMaxSoundFileBytes == 268435456);
    }

    TEST_CASE("a file of more than two channels is mixed down to two") {
        // Four channels, each of them heard alone in a frame of its own.
        const SoundData mixed = unwrap(decodeSound(
            wavFile({.channels = 4}, samples16({16384, 0, 0, 0, 0, 16384, 0, 0, 0, 0, 16384, 0, 0, 0, 0, 16384}))));
        CHECK(mixed.channels == 2);
        CHECK(mixed.sampleRate == 48000);
        REQUIRE(mixed.frames() == 4);
        // The first two are left and right. Where the others go is the mixer's to say.
        CHECK(mixed.samples[0] == 0.5f);
        CHECK(mixed.samples[1] == 0.0f);
        CHECK(mixed.samples[2] == 0.0f);
        CHECK(mixed.samples[3] == 0.5f);
        CHECK(validateSound(mixed).has_value());

        // Six, as a film has them: still two, and still as long.
        const SoundData film = unwrap(decodeSound(wavFile({.channels = 6}, std::vector<std::byte>(6 * 2 * 50))));
        CHECK(film.channels == 2);
        CHECK(film.frames() == 50);
    }

    TEST_CASE("a file of floats is read as it is, but for what is no number") {
        FileBuilder floats;
        floats.number(0.25f).number(kNoNumber).number(kInfinity).number(-kInfinity).number(-0.5f).number(3.0f);
        const SoundData sound = unwrap(decodeSound(wavFile({.format = 3, .bitsPerSample = 32}, floats.bytes())));
        CHECK(sound.channels == 1);
        // Louder than one stays as loud; what is no number is silence.
        CHECK(sound.samples == std::vector<float>{0.25f, 0.0f, 0.0f, 0.0f, -0.5f, 3.0f});
        CHECK(validateSound(sound).has_value());
    }

    TEST_CASE("a file at a rate no sound has is refused") {
        const std::vector<std::byte> samples = samples16({1, 2, 3, 4});
        CHECK(unwrap(decodeSound(wavFile({.sampleRate = 8000}, samples))).sampleRate == 8000);
        CHECK(unwrap(decodeSound(wavFile({.sampleRate = 384000}, samples))).sampleRate == 384000);
        for (const uint32_t sampleRate : std::initializer_list<uint32_t>{1, 4000, 7999}) {
            CAPTURE(sampleRate);
            const Error error = errorIn(decodeSound(wavFile({.sampleRate = sampleRate}, samples)));
            CHECK(error.code == ErrorCode::OutOfRange);
            CHECK(error.message == fmt::format("it has {} frames a second; a sound has 8000 to 384000", sampleRate));
        }
        // What reads WAV files refuses these itself.
        CHECK(errorIn(decodeSound(wavFile({.sampleRate = 0}, samples))).code == ErrorCode::ParseError);
        CHECK(errorIn(decodeSound(wavFile({.sampleRate = 384001}, samples))).code == ErrorCode::ParseError);

        // What reads FLAC files does not: a rate above every sound's gets as far as here.
        const std::vector<std::vector<int16_t>> frames(16, std::vector<int16_t>{0});
        CHECK(unwrap(decodeSound(flacFile(384000, frames))).sampleRate == 384000);
        const Error fast = errorIn(decodeSound(flacFile(384001, frames)));
        CHECK(fast.code == ErrorCode::OutOfRange);
        CHECK(fast.message == "it has 384001 frames a second; a sound has 8000 to 384000");
    }

    TEST_CASE("a FLAC file is read, to the last bit") {
        std::vector<std::vector<int16_t>> frames;
        for (int frame = 0; frame < 32; ++frame) {
            frames.push_back({static_cast<int16_t>(frame * 1000), static_cast<int16_t>(-frame * 500 - 1)});
        }
        // The ends of what sixteen bits hold.
        frames[30] = {32767, -32768};

        const SoundData stereo = unwrap(decodeSound(flacFile(44100, frames)));
        CHECK(stereo.sampleRate == 44100);
        CHECK(stereo.channels == 2);
        REQUIRE(stereo.frames() == 32);
        for (size_t frame = 0; frame < frames.size(); ++frame) {
            CAPTURE(frame);
            CHECK(stereo.samples[frame * 2] == static_cast<float>(frames[frame][0]) / 32768.0f);
            CHECK(stereo.samples[frame * 2 + 1] == static_cast<float>(frames[frame][1]) / 32768.0f);
        }
        CHECK(validateSound(stereo).has_value());

        const SoundData mono = unwrap(decodeSound(flacFile(22050, std::vector<std::vector<int16_t>>(16, {-16384}))));
        CHECK(mono.sampleRate == 22050);
        CHECK(mono.channels == 1);
        CHECK(mono.samples == std::vector<float>(16, -0.5f));
    }

    TEST_CASE("a FLAC file whose samples are not what its checksum says has no sound in it") {
        std::vector<std::byte> file = flacFile(44100, std::vector<std::vector<int16_t>>(32, {1000, -1000}));
        REQUIRE(unwrap(decodeSound(file)).frames() == 32);
        // One bit of one sample, in the middle of the only frame there is.
        file[file.size() - 40] ^= std::byte{0x10};
        const Error error = errorIn(decodeSound(file));
        CHECK((error.code == ErrorCode::ParseError || error.code == ErrorCode::OutOfRange));
    }

    TEST_CASE("an MP3 file is read") {
        const SoundData sound = unwrap(decodeSound(silentMp3File(12)));
        CHECK(sound.sampleRate == 44100);
        CHECK(sound.channels == 1);
        CHECK(sound.frames() == 12 * kMp3FrameSamples);
        CHECK(std::ranges::all_of(sound.samples, [](float sample) { return sample == 0.0f; }));

        // It has no length written in it; it is as long as it turns out to be, and that
        // is counted like any other.
        CHECK(unwrap(decodeSound(silentMp3File(12), {.maxFrames = 12 * kMp3FrameSamples})).frames() ==
              12 * kMp3FrameSamples);
        CHECK(errorIn(decodeSound(silentMp3File(12), {.maxFrames = 12 * kMp3FrameSamples - 1})).code ==
              ErrorCode::OutOfRange);
    }

    TEST_CASE("files with bytes changed at random are sounds or errors, and nothing else") {
        FileBuilder floats;
        for (int sample = 0; sample < 64; ++sample) {
            floats.number(static_cast<float>(sample) / 64.0f);
        }
        const std::array<std::vector<std::byte>, 4> files = {
            unwrap(encodeWav(sweep(44100, 2, 64))),
            wavFile({.format = 3, .channels = 2, .bitsPerSample = 32}, floats.bytes()),
            flacFile(48000, std::vector<std::vector<int16_t>>(64, {12345, -12345})),
            silentMp3File(12),
        };
        constexpr uint64_t kMostFrames = 100000;

        Random random(20261013);
        size_t sounds = 0;
        size_t errors = 0;
        for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
            for (int round = 0; round < 500; ++round) {
                std::vector<std::byte> changed = files[fileIndex];
                const int32_t changes = random.rangeInt(1, 4);
                for (int32_t change = 0; change < changes; ++change) {
                    // Mostly near the beginning, where a file says what it is.
                    const auto last = static_cast<int32_t>(change == 0 ? std::min<size_t>(changed.size(), 64) - 1
                                                                       : changed.size() - 1);
                    changed[static_cast<size_t>(random.rangeInt(0, last))] =
                        static_cast<std::byte>(random.rangeInt(0, 255));
                }
                if (random.rangeInt(0, 3) == 0) {
                    changed.resize(static_cast<size_t>(random.rangeInt(0, static_cast<int32_t>(changed.size()))));
                }

                CAPTURE(fileIndex);
                CAPTURE(round);
                const Result<SoundData> result = decodeSound(changed, {.maxFrames = kMostFrames});
                if (result.has_value()) {
                    const SoundData sound = unwrap(result);
                    CHECK(validateSound(sound).has_value());
                    CHECK(sound.frames() <= kMostFrames);
                    ++sounds;
                } else {
                    const ErrorCode code = errorIn(result).code;
                    CHECK((code == ErrorCode::ParseError || code == ErrorCode::OutOfRange));
                    ++errors;
                }
            }
        }
        // Both happen: a change in the samples leaves a sound, one in what says what
        // the file is leaves none.
        CHECK(sounds > 100);
        CHECK(errors > 100);
    }
}
