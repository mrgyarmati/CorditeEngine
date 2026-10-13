#include "cordite/audio/AudioSystem.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Sound.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/core/FileSystem.h"
#include "cordite/math/Transform.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

/// How long the sounds of the stage are, in frames of the mix: a tenth of a second.
constexpr size_t kSoundFrames = 4800;
/// A hundredth of a second, and the frames mixed in it.
constexpr double kTick = 0.01;
constexpr size_t kTickFrames = 480;

/// What was heard: left and right of every frame.
struct Heard {
    std::vector<float> samples;

    [[nodiscard]] size_t frames() const { return samples.size() / 2; }

    [[nodiscard]] float left(size_t frame) const { return samples[frame * 2]; }

    [[nodiscard]] float right(size_t frame) const { return samples[frame * 2 + 1]; }

    /// What both ears heard at the end.
    [[nodiscard]] Vec2 last() const {
        REQUIRE(frames() > 0);
        return {left(frames() - 1), right(frames() - 1)};
    }

    /// Which frame of the counting sound the left ear heard, when it is played as it is.
    [[nodiscard]] long count(size_t frame) const { return std::lround(left(frame) * 32768.0f); }

    [[nodiscard]] bool silent() const {
        return std::ranges::all_of(samples, [](float sample) { return sample == 0.0f; });
    }

    void append(const Heard& more) { samples.insert(samples.end(), more.samples.begin(), more.samples.end()); }
};

/// Whether two levels are the same, as closely as the mixer works out distances: it is
/// in no hurry to be exact about a square root.
bool about(const Vec2& heard, const Vec2& expected) {
    return test::nearlyEqual(heard, expected, 1.0e-3f * std::max(1.0f, std::max(expected.x, expected.y)));
}

/// A project with a few sounds whose samples say what they are, a world, and an audio
/// system that mixes into memory.
struct Stage {
    explicit Stage(const AudioSystemDesc& desc = {}) {
        // Half as loud as can be, all the way through.
        writeSound("steady.wav", 48000, 1, kSoundFrames, [](size_t, uint32_t) { return 0.5f; });
        // Every sample says which frame it is: frame 7 is read back as 7 of 32768.
        writeSound("count.wav", 48000, 1, kSoundFrames,
                   [](size_t frame, uint32_t) { return static_cast<float>(frame) / 32767.0f; });
        // Left and right differ, in size and in sign.
        writeSound("sides.wav", 48000, 2, kSoundFrames,
                   [](size_t, uint32_t channel) { return channel == 0 ? 0.25f : -0.5f; });
        // At half the rate of the mix: as long as the others, in half as many frames.
        writeSound("slow.wav", 24000, 1, kSoundFrames / 2, [](size_t, uint32_t) { return 0.5f; });
        // Quiet, so that several of them can be told apart in a sum.
        writeSound("quiet.wav", 48000, 1, kSoundFrames, [](size_t, uint32_t) { return 0.125f; });
        writeText("words.wav", "these are words, whatever the name says");
        writeText("notes.lua", "return {}");
        assets = unwrap(AssetManager::create({.rootDirectory = directory.path() / "assets"}));
        audio = unwrap(AudioSystem::create(desc));
    }

    template <typename Sample>
    void writeSound(std::string_view name, uint32_t sampleRate, uint32_t channels, size_t frames, Sample sample) const {
        SoundData sound{.sampleRate = sampleRate, .channels = channels};
        for (size_t frame = 0; frame < frames; ++frame) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                sound.samples.push_back(sample(frame, channel));
            }
        }
        REQUIRE(
            writeBinaryFile(directory.path() / "assets" / pathFromUtf8(name), unwrap(encodeWav(sound))).has_value());
    }

    void writeText(std::string_view name, std::string_view text) const {
        REQUIRE(writeTextFile(directory.path() / "assets" / pathFromUtf8(name), text).has_value());
    }

    [[nodiscard]] Uuid idOf(std::string_view reference) const {
        const AssetInfo* asset = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
        return asset->id;
    }

    /// An entity that plays `sound` the same in both ears.
    Entity source(std::string_view sound) {
        const Entity entity = world.createEntity("Source");
        world.add<AudioSourceComponent>(entity) = {.sound = idOf(sound), .spatial = false};
        return entity;
    }

    /// An entity at `position` that plays `sound` from there.
    Entity sourceAt(std::string_view sound, const Vec3& position) {
        const Entity entity = world.createEntity("Source");
        world.tryGet<Transform>(entity)->position = position;
        world.add<AudioSourceComponent>(entity) = {.sound = idOf(sound)};
        return entity;
    }

    [[nodiscard]] AudioSourceComponent& sourceOf(Entity entity) {
        AudioSourceComponent* component = world.tryGet<AudioSourceComponent>(entity);
        REQUIRE(component != nullptr);
        return *component;
    }

    /// Lets time pass, and returns what was heard in it.
    Heard hear(double seconds = kTick) {
        world.updateWorldTransforms();
        statistics = audio->update(world, *assets, seconds);
        const std::span<const float> mixed = audio->mixed();
        return {.samples = {mixed.begin(), mixed.end()}};
    }

    /// Lets a hundredth of a second pass, and returns what both ears heard at its end.
    Vec2 level() { return hear().last(); }

    /// The same after an eighth of a second, for what is heard from a place. The mixer
    /// does not jump to where a source or the listener has moved: it slides there, over
    /// some hundredths of a second, and gets ever closer after that. Compare what this
    /// returns with about().
    Vec2 settled() {
        for (int update = 0; update < 12; ++update) {
            hear();
        }
        return hear().last();
    }

    TempDirectory directory;
    std::unique_ptr<AssetManager> assets;
    World world;
    std::unique_ptr<AudioSystem> audio;
    AudioStatistics statistics;
};

/// The error of a result that has to be one.
template <typename T>
Error errorIn(const Result<T>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_SUITE("AudioSystem") {
    TEST_CASE("an audio system mixes into memory unless it is asked for more") {
        const std::unique_ptr<AudioSystem> audio = unwrap(AudioSystem::create());
        CHECK(audio->output() == AudioOutput::Offline);
        CHECK(audio->sampleRate() == 48000);
        CHECK(audio->mixed().empty());
        CHECK(audio->masterVolume() == 1.0f);
        for (const AudioBus bus : kAudioBuses) {
            CHECK(audio->volume(bus) == 1.0f);
        }
        CHECK(AudioSystemDesc().maxVoices == 64);

        CHECK(nameOf(AudioOutput::Offline) == "offline");
        CHECK(nameOf(AudioOutput::Device) == "device");
        CHECK(nameOf(AudioOutput::Silent) == "silent");

        // At the rate it is told to.
        CHECK(unwrap(AudioSystem::create({.sampleRate = 22050}))->sampleRate() == 22050);
    }

    TEST_CASE("an audio system outside its limits is not made") {
        const auto refused = [](const AudioSystemDesc& desc) {
            const Error error = errorIn(AudioSystem::create(desc));
            CHECK(error.code == ErrorCode::InvalidArgument);
            return error.message;
        };
        CHECK(refused({.sampleRate = 7999}) == "sound is mixed at 8000 to 384000 frames a second, not 7999");
        CHECK(refused({.sampleRate = 384001}) == "sound is mixed at 8000 to 384000 frames a second, not 384001");
        CHECK(refused({.maxVoices = 0}) == "an audio system has 1 to 1024 voices, not 0");
        CHECK(refused({.maxVoices = 1025}) == "an audio system has 1 to 1024 voices, not 1025");
        CHECK(refused({.output = static_cast<AudioOutput>(7)}) == "7 is no audio output");
        // At the limits it is.
        CHECK(AudioSystem::create({.sampleRate = 8000, .maxVoices = 1}).has_value());
        CHECK(AudioSystem::create({.sampleRate = 384000, .maxVoices = 1024}).has_value());
    }

    TEST_CASE("a source plays its sound from the beginning to the end, and then says that it is over") {
        Stage stage;
        const Entity entity = stage.source("count.wav");

        Heard all;
        int updates = 0;
        while (stage.sourceOf(entity).playing) {
            REQUIRE(updates < 100);
            all.append(stage.hear());
            ++updates;
            // It is counted while it plays, and not after.
            CHECK(stage.statistics.sources == (stage.sourceOf(entity).playing ? 1 : 0));
        }
        // A tenth of a second of sound: over in the eleventh hundredth, and not before.
        CHECK(updates == 11);
        CHECK(stage.statistics.voices == 0);

        // What was heard is the sound, one frame after the other, the same in both ears.
        size_t first = 0;
        while (first < all.frames() && all.left(first) == 0.0f) {
            ++first;
        }
        // (The mixer is a frame behind, and the first frame of the sound is silence.)
        REQUIRE(first <= 2);
        const long begin = all.count(first);
        CHECK(begin <= 1);
        size_t wrong = 0;
        for (size_t frame = first; frame < first + kSoundFrames - 4; ++frame) {
            wrong += all.count(frame) == begin + static_cast<long>(frame - first) ? 0 : 1;
            wrong += all.left(frame) == all.right(frame) ? 0 : 1;
        }
        CHECK(wrong == 0);
        // After its last frame there is nothing.
        CHECK(all.last() == Vec2(0.0f));

        // It stays over.
        CHECK(stage.hear().silent());
        CHECK_FALSE(stage.sourceOf(entity).playing);

        // Told to play again, it begins again: what is heard is what was heard first.
        stage.sourceOf(entity).playing = true;
        const Heard again = stage.hear();
        REQUIRE(again.frames() == kTickFrames);
        CHECK(std::equal(again.samples.begin(), again.samples.end(), all.samples.begin()));
        CHECK(stage.statistics.sources == 1);
    }

    TEST_CASE("a source that loops starts again at its end, until it is told to stop") {
        Stage stage;
        const Entity entity = stage.source("count.wav");
        stage.sourceOf(entity).loop = true;

        // Two and a half times its length.
        Heard all;
        for (int update = 0; update < 25; ++update) {
            all.append(stage.hear());
        }
        CHECK(stage.sourceOf(entity).playing);
        size_t restarts = 0;
        for (size_t frame = 1; frame < all.frames(); ++frame) {
            if (all.count(frame) < all.count(frame - 1)) {
                ++restarts;
                // From its last frames straight to its first.
                CHECK(all.count(frame - 1) >= static_cast<long>(kSoundFrames) - 2);
                CHECK(all.count(frame) <= 1);
            }
        }
        CHECK(restarts == 2);

        // No longer looping, it plays to its end and is over.
        stage.sourceOf(entity).loop = false;
        for (int update = 0; update < 11; ++update) {
            stage.hear();
        }
        CHECK_FALSE(stage.sourceOf(entity).playing);
        CHECK(stage.hear().silent());
    }

    TEST_CASE("a source is heard while it is playing, has a sound, and is there") {
        Stage stage;
        const Entity entity = stage.source("steady.wav");
        stage.sourceOf(entity).loop = true;
        CHECK(stage.level() == Vec2(0.5f));

        // Not playing: silence, from the next update on, and no voice.
        stage.sourceOf(entity).playing = false;
        CHECK(stage.hear().silent());
        CHECK(stage.statistics.sources == 0);
        // Playing again: from the beginning, which a steady sound does not show - but
        // one that counts does.
        stage.sourceOf(entity).sound = stage.idOf("count.wav");
        stage.sourceOf(entity).playing = true;
        CHECK(stage.hear().count(kTickFrames - 1) <= static_cast<long>(kTickFrames));
        stage.sourceOf(entity).playing = false;
        stage.hear();
        stage.sourceOf(entity).playing = true;
        CHECK(stage.hear().count(kTickFrames - 1) <= static_cast<long>(kTickFrames));

        // Without a sound there is nothing to play; it waits, and says nothing is over.
        // That is no mistake to report: a source that is being set up looks like this.
        {
            const LogCapture log(LogLevel::Warn);
            stage.sourceOf(entity).sound = Uuid();
            CHECK(stage.hear().silent());
            CHECK(stage.statistics.sources == 0);
            CHECK(stage.sourceOf(entity).playing);
            CHECK(log.messages().empty());
        }
        stage.sourceOf(entity).sound = stage.idOf("steady.wav");
        CHECK(stage.level() == Vec2(0.5f));

        // Switched off, alone or with what it hangs under.
        const Entity room = stage.world.createEntity("Room");
        REQUIRE(stage.world.setParent(entity, room).has_value());
        stage.world.setEnabled(room, false);
        CHECK(stage.hear().silent());
        stage.world.setEnabled(room, true);
        CHECK(stage.level() == Vec2(0.5f));
        stage.world.setEnabled(entity, false);
        CHECK(stage.hear().silent());
        stage.world.setEnabled(entity, true);
        CHECK(stage.level() == Vec2(0.5f));

        // Without the component, and without the entity.
        stage.world.remove<AudioSourceComponent>(entity);
        CHECK(stage.hear().silent());
        stage.world.add<AudioSourceComponent>(entity) = {.sound = stage.idOf("steady.wav"), .spatial = false};
        CHECK(stage.level() == Vec2(0.5f));
        stage.world.destroyEntity(entity);
        CHECK(stage.hear().silent());
        CHECK(stage.statistics == AudioStatistics());
    }

    TEST_CASE("a voice follows its source: how loud, how fast, and what it plays") {
        Stage stage;
        const Entity entity = stage.source("steady.wav");
        stage.sourceOf(entity).loop = true;
        CHECK(stage.level() == Vec2(0.5f));

        stage.sourceOf(entity).volume = 0.5f;
        CHECK(stage.level() == Vec2(0.25f));
        stage.sourceOf(entity).volume = 0.0f;
        CHECK(stage.hear().silent());
        // Not playing a note, it is still a voice.
        CHECK(stage.statistics.sources == 1);
        stage.sourceOf(entity).volume = 1.5f;
        CHECK(stage.level() == Vec2(0.75f));

        // Another sound begins at once, at its beginning.
        stage.sourceOf(entity).volume = 1.0f;
        stage.hear();
        stage.sourceOf(entity).sound = stage.idOf("count.wav");
        const Heard counted = stage.hear();
        CHECK(counted.count(kTickFrames - 1) <= static_cast<long>(kTickFrames));
        CHECK(counted.count(kTickFrames - 1) >= static_cast<long>(kTickFrames) - 2);
        // And so does the same sound on another bus.
        stage.sourceOf(entity).bus = AudioBus::Voice;
        CHECK(stage.hear().count(kTickFrames - 1) <= static_cast<long>(kTickFrames));
        // What changes neither goes on.
        stage.sourceOf(entity).loop = false;
        stage.sourceOf(entity).maxDistance = 20.0f;
        CHECK(stage.hear().count(0) >= static_cast<long>(kTickFrames) - 2);
    }

    TEST_CASE("a sound played twice as fast is over in half the time, and half as fast in twice") {
        Stage stage;
        const auto updatesToTheEnd = [&](float pitch) {
            const Entity entity = stage.source("count.wav");
            stage.sourceOf(entity).pitch = pitch;
            int updates = 0;
            while (stage.sourceOf(entity).playing) {
                REQUIRE(updates < 200);
                stage.hear();
                ++updates;
            }
            stage.world.destroyEntity(entity);
            return updates;
        };
        CHECK(updatesToTheEnd(1.0f) == 11);
        CHECK(updatesToTheEnd(2.0f) == 6);
        CHECK(updatesToTheEnd(0.5f) == 21);
        // No faster than four times and no slower than a quarter, whatever is asked for.
        CHECK(updatesToTheEnd(4.0f) == 3);
        CHECK(updatesToTheEnd(1000.0f) == 3);
        CHECK(updatesToTheEnd(0.25f) == 41);
        CHECK(updatesToTheEnd(0.0f) == 41);
        CHECK(updatesToTheEnd(-3.0f) == 41);
        // And at its own speed when what is asked for is no number.
        CHECK(updatesToTheEnd(kNotANumber) == 11);

        // Twice as fast is every other frame.
        const Entity entity = stage.source("count.wav");
        stage.sourceOf(entity).pitch = 2.0f;
        const Heard heard = stage.hear();
        CHECK(heard.count(100) - heard.count(99) == 2);
    }

    TEST_CASE("a sound of another rate than the mix is as long and as loud as it was made") {
        Stage stage;
        const Entity entity = stage.source("slow.wav");
        int updates = 0;
        while (stage.sourceOf(entity).playing) {
            REQUIRE(updates < 100);
            const Heard heard = stage.hear();
            ++updates;
            if (updates >= 2 && updates <= 9) {
                CHECK(heard.left(0) == doctest::Approx(0.5f).epsilon(0.001));
                CHECK(heard.last().x == doctest::Approx(0.5f).epsilon(0.001));
            }
        }
        // 2400 frames at 24 kHz: a tenth of a second, like the others.
        CHECK(updates == 11);

        // And the other way: a mix at 24 kHz plays a sound of 48 kHz in half the frames.
        Stage coarse({.sampleRate = 24000});
        const Entity fine = coarse.source("steady.wav");
        updates = 0;
        while (coarse.sourceOf(fine).playing) {
            REQUIRE(updates < 100);
            const Heard heard = coarse.hear();
            CHECK(heard.frames() == kTickFrames / 2);
            ++updates;
        }
        CHECK(updates == 11);
    }

    TEST_CASE("values no property allows are heard as the nearest that is allowed") {
        Stage stage;
        // The bus is turned down so that four times as loud is not more than can be said.
        REQUIRE(stage.audio->setVolume(AudioBus::Effects, 0.25f).has_value());
        const Entity entity = stage.source("steady.wav");
        stage.sourceOf(entity).loop = true;
        CHECK(stage.level() == Vec2(0.125f));
        stage.sourceOf(entity).volume = 100.0f;
        CHECK(stage.level() == Vec2(0.5f));
        stage.sourceOf(entity).volume = kInfinity;
        CHECK(stage.level() == Vec2(0.125f));
        stage.sourceOf(entity).volume = -2.0f;
        CHECK(stage.hear().silent());
        stage.sourceOf(entity).volume = kNotANumber;
        CHECK(stage.level() == Vec2(0.125f));

        // Distances: a source with none is one with the nearest there are.
        stage.sourceOf(entity).volume = 1.0f;
        stage.sourceOf(entity).spatial = true;
        stage.sourceOf(entity).minDistance = -5.0f;
        stage.sourceOf(entity).maxDistance = kNotANumber;
        stage.world.tryGet<Transform>(entity)->position = Vec3(0.0f, 0.0f, -100.0f);
        // Straight ahead, beyond the far distance, which is 50 m when it is not said:
        // 0.01 m over 50 m of a quarter of 0.5, and half of that in each ear.
        CHECK(about(stage.settled() * 100000.0f, Vec2(1.25f)));
    }

    TEST_CASE("a source in space is heard from its side") {
        Stage stage;
        const Entity entity = stage.sourceAt("steady.wav", Vec3(0.0f));
        stage.sourceOf(entity).loop = true;
        const auto levelAt = [&](const Vec3& position) {
            stage.world.tryGet<Transform>(entity)->position = position;
            return stage.settled();
        };
        // Where the listener is: all of it, in both ears.
        CHECK(about(levelAt(Vec3(0.0f)), Vec2(0.5f)));
        // At a side: all of it in that ear, a fifth in the other.
        CHECK(about(levelAt(Vec3(1.0f, 0.0f, 0.0f)), Vec2(0.1f, 0.5f)));
        CHECK(about(levelAt(Vec3(-1.0f, 0.0f, 0.0f)), Vec2(0.5f, 0.1f)));
        // Ahead, behind, above and below: half of it in both.
        CHECK(about(levelAt(Vec3(0.0f, 0.0f, -1.0f)), Vec2(0.25f)));
        CHECK(about(levelAt(Vec3(0.0f, 0.0f, 1.0f)), Vec2(0.25f)));
        CHECK(about(levelAt(Vec3(0.0f, 1.0f, 0.0f)), Vec2(0.25f)));
        CHECK(about(levelAt(Vec3(0.0f, -1.0f, 0.0f)), Vec2(0.25f)));
        // Half-way to the right: nearer to all of it in that ear, and the other still at
        // the fifth it never goes below.
        CHECK(about(levelAt(glm::normalize(Vec3(1.0f, 0.0f, -1.0f))), Vec2(0.1f, 0.5f * 0.8535534f)));
        // A little to the left: both ears hear more than a fifth, the left one more.
        const Vec2 slightly = levelAt(glm::normalize(Vec3(-1.0f, 0.0f, -3.0f)));
        CHECK(slightly.x > slightly.y + 0.1f);
        CHECK(slightly.y > 0.15f);
        CHECK(slightly.x < 0.45f);

        // A source without a place is the same in both ears wherever its entity is.
        stage.sourceOf(entity).spatial = false;
        CHECK(about(levelAt(Vec3(30.0f, 0.0f, 0.0f)), Vec2(0.5f)));
        // And so is one whose entity is nowhere.
        stage.sourceOf(entity).spatial = true;
        CHECK(about(levelAt(Vec3(kNotANumber, 0.0f, 0.0f)), Vec2(0.5f)));
        CHECK(about(levelAt(Vec3(0.0f, kInfinity, 0.0f)), Vec2(0.5f)));
    }

    TEST_CASE("a source in space gets quieter with distance, the way its rolloff says") {
        Stage stage;
        const Entity entity = stage.sourceAt("steady.wav", Vec3(0.0f));
        stage.sourceOf(entity).loop = true;
        stage.sourceOf(entity).minDistance = 2.0f;
        stage.sourceOf(entity).maxDistance = 10.0f;
        // Straight ahead, where both ears hear half of what there is: 0.25 at its loudest.
        const auto levelAt = [&](float distance) {
            stage.world.tryGet<Transform>(entity)->position = Vec3(0.0f, 0.0f, -distance);
            return stage.settled();
        };

        // As in the open: all of it up to the near distance, half at twice that, a third
        // at three times, and no quieter beyond the far one.
        CHECK(about(levelAt(1.0f), Vec2(0.25f)));
        CHECK(about(levelAt(2.0f), Vec2(0.25f)));
        CHECK(about(levelAt(4.0f), Vec2(0.125f)));
        CHECK(about(levelAt(6.0f), Vec2(0.25f / 3.0f)));
        CHECK(about(levelAt(10.0f), Vec2(0.05f)));
        CHECK(about(levelAt(1000.0f), Vec2(0.05f)));

        // Evenly: all of it at the near distance, half of it half-way, nothing at the far
        // one and beyond.
        stage.sourceOf(entity).rolloff = AudioRolloff::Linear;
        CHECK(about(levelAt(1.0f), Vec2(0.25f)));
        CHECK(about(levelAt(2.0f), Vec2(0.25f)));
        CHECK(about(levelAt(6.0f), Vec2(0.125f)));
        CHECK(about(levelAt(8.0f), Vec2(0.0625f)));
        CHECK(about(levelAt(10.0f), Vec2(0.0f)));
        CHECK(about(levelAt(1000.0f), Vec2(0.0f)));

        // A far distance that is not beyond the near one is taken to be just beyond it:
        // as loud as it gets up to there, and as quiet as it gets after.
        stage.sourceOf(entity).maxDistance = 1.0f;
        CHECK(about(levelAt(1.9f), Vec2(0.25f)));
        CHECK(about(levelAt(2.1f), Vec2(0.0f)));
        stage.sourceOf(entity).rolloff = AudioRolloff::Inverse;
        CHECK(about(levelAt(1.9f), Vec2(0.25f)));
        CHECK(about(levelAt(500.0f), Vec2(0.25f / 1.001f)));
    }

    TEST_CASE("a sound of two channels keeps them, in space as well") {
        Stage stage;
        const Entity entity = stage.source("sides.wav");
        stage.sourceOf(entity).loop = true;
        CHECK(stage.level() == Vec2(0.25f, -0.5f));
        // At the right: its left channel is the ear on the far side.
        stage.sourceOf(entity).spatial = true;
        stage.world.tryGet<Transform>(entity)->position = Vec3(1.0f, 0.0f, 0.0f);
        CHECK(about(stage.settled(), Vec2(0.05f, -0.5f)));
    }

    TEST_CASE("everything is heard from the listener: where it is, and the way it faces") {
        Stage stage;
        const Entity entity = stage.sourceAt("steady.wav", Vec3(10.0f, 0.0f, 0.0f));
        stage.sourceOf(entity).loop = true;
        // Without a listener, from the origin, looking along -Z: far off to the right.
        CHECK(about(stage.settled(), Vec2(0.01f, 0.05f)));
        CHECK(stage.statistics.listeners == 0);

        // A listener next to it, on its far side: it is to the left, and near.
        const Entity ears = stage.world.createEntity("Ears");
        stage.world.add<AudioListenerComponent>(ears);
        stage.world.tryGet<Transform>(ears)->position = Vec3(11.0f, 0.0f, 0.0f);
        CHECK(about(stage.settled(), Vec2(0.5f, 0.1f)));
        CHECK(stage.statistics.listeners == 1);

        // Turned to face it: ahead. A quarter turn to the left is about +Y.
        stage.world.tryGet<Transform>(ears)->rotation = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f));
        CHECK(about(stage.settled(), Vec2(0.25f)));
        // Turned the other way: behind, which sounds the same.
        stage.world.tryGet<Transform>(ears)->rotation = glm::angleAxis(glm::radians(-90.0f), Vec3(0.0f, 1.0f, 0.0f));
        CHECK(about(stage.settled(), Vec2(0.25f)));
        // Lying on its right side, looking along -Z: what was at its left is below it.
        stage.world.tryGet<Transform>(ears)->rotation = glm::angleAxis(glm::radians(-90.0f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(about(stage.settled(), Vec2(0.25f)));
        // Standing on its head: left and right are the other way round.
        stage.world.tryGet<Transform>(ears)->rotation = glm::angleAxis(glm::radians(180.0f), Vec3(0.0f, 0.0f, 1.0f));
        CHECK(about(stage.settled(), Vec2(0.1f, 0.5f)));

        // However large it is; and one that has no size at all hears from the origin.
        stage.world.tryGet<Transform>(ears)->rotation = kIdentityQuat;
        stage.world.tryGet<Transform>(ears)->scale = Vec3(40.0f, 0.5f, 3.0f);
        CHECK(about(stage.settled(), Vec2(0.5f, 0.1f)));
        stage.world.tryGet<Transform>(ears)->scale = Vec3(0.0f);
        CHECK(about(stage.settled(), Vec2(0.01f, 0.05f)));
        stage.world.tryGet<Transform>(ears)->scale = Vec3(1.0f);

        // It hears as loud as it says.
        stage.world.tryGet<AudioListenerComponent>(ears)->volume = 0.5f;
        CHECK(about(stage.settled(), Vec2(0.25f, 0.05f)));
        stage.world.tryGet<AudioListenerComponent>(ears)->volume = kNotANumber;
        CHECK(about(stage.settled(), Vec2(0.5f, 0.1f)));
        stage.world.tryGet<AudioListenerComponent>(ears)->volume = 0.0f;
        CHECK(stage.hear().silent());
        stage.world.tryGet<AudioListenerComponent>(ears)->volume = 1.0f;

        // A second one is counted, and hears nothing: the first in the hierarchy does.
        const Entity others = stage.world.createEntity("Others");
        stage.world.add<AudioListenerComponent>(others);
        CHECK(about(stage.settled(), Vec2(0.5f, 0.1f)));
        CHECK(stage.statistics.listeners == 2);
        // Until the first is switched off, or is none any more.
        stage.world.setEnabled(ears, false);
        CHECK(about(stage.settled(), Vec2(0.01f, 0.05f)));
        CHECK(stage.statistics.listeners == 1);
        stage.world.setEnabled(ears, true);
        stage.world.remove<AudioListenerComponent>(ears);
        CHECK(about(stage.settled(), Vec2(0.01f, 0.05f)));
        stage.world.destroyEntity(others);
        CHECK(about(stage.settled(), Vec2(0.01f, 0.05f)));
        CHECK(stage.statistics.listeners == 0);
    }

    TEST_CASE("every bus has a volume of its own, and everything has one on top") {
        Stage stage;
        std::vector<Entity> entities;
        for (const AudioBus bus : kAudioBuses) {
            const Entity entity = stage.source("quiet.wav");
            stage.sourceOf(entity).bus = bus;
            stage.sourceOf(entity).loop = true;
            entities.push_back(entity);
        }
        // Five of them, an eighth each.
        CHECK(stage.level() == Vec2(0.625f));

        // One bus down to nothing: one of them less. And each in its turn.
        for (const AudioBus bus : kAudioBuses) {
            CAPTURE(nameOf(bus));
            REQUIRE(stage.audio->setVolume(bus, 0.0f).has_value());
            CHECK(stage.audio->volume(bus) == 0.0f);
            CHECK(stage.level() == Vec2(0.5f));
            REQUIRE(stage.audio->setVolume(bus, 2.0f).has_value());
            CHECK(stage.level() == Vec2(0.75f));
            REQUIRE(stage.audio->setVolume(bus, 1.0f).has_value());
        }

        // Everything at once.
        REQUIRE(stage.audio->setMasterVolume(0.5f).has_value());
        CHECK(stage.audio->masterVolume() == 0.5f);
        CHECK(stage.level() == Vec2(0.3125f));
        // Which is on top of what the listener says, not instead of it.
        const Entity ears = stage.world.createEntity("Ears");
        stage.world.add<AudioListenerComponent>(ears).volume = 0.5f;
        CHECK(stage.level() == Vec2(0.15625f));
        REQUIRE(stage.audio->setMasterVolume(1.0f).has_value());
        CHECK(stage.level() == Vec2(0.3125f));
    }

    TEST_CASE("a volume that is none is refused, and changes nothing") {
        Stage stage;
        REQUIRE(stage.audio->setVolume(AudioBus::Music, 0.75f).has_value());
        REQUIRE(stage.audio->setMasterVolume(0.25f).has_value());
        for (const float volume : {-0.01f, 4.01f, kNotANumber, kInfinity, -kInfinity}) {
            CAPTURE(volume);
            const Error bus = errorIn(stage.audio->setVolume(AudioBus::Music, volume));
            CHECK(bus.code == ErrorCode::InvalidArgument);
            CHECK(bus.message.starts_with("the volume of a bus is from 0 to 4, not "));
            const Error master = errorIn(stage.audio->setMasterVolume(volume));
            CHECK(master.code == ErrorCode::InvalidArgument);
            CHECK(master.message.starts_with("the volume of everything is from 0 to 4, not "));
        }
        CHECK(stage.audio->volume(AudioBus::Music) == 0.75f);
        CHECK(stage.audio->masterVolume() == 0.25f);
        // The ends of what is allowed are allowed.
        CHECK(stage.audio->setVolume(AudioBus::Music, 4.0f).has_value());
        CHECK(stage.audio->setMasterVolume(0.0f).has_value());
        // A bus there is none of.
        const Error bus = errorIn(stage.audio->setVolume(static_cast<AudioBus>(9), 1.0f));
        CHECK(bus.code == ErrorCode::InvalidArgument);
        CHECK(bus.message == "9 is no audio bus");
    }

    TEST_CASE("what is louder than can be said is clipped, not wrapped") {
        Stage stage;
        const Entity loud = stage.source("sides.wav");
        stage.sourceOf(loud).volume = 4.0f;
        stage.sourceOf(loud).loop = true;
        CHECK(stage.level() == Vec2(1.0f, -1.0f));
        // And so is a sum.
        const Entity more = stage.source("steady.wav");
        stage.sourceOf(more).loop = true;
        stage.sourceOf(loud).volume = 3.0f;
        CHECK(stage.level() == Vec2(1.0f, -1.0f));
        stage.sourceOf(loud).volume = 1.0f;
        CHECK(stage.level() == Vec2(0.75f, 0.0f));
    }

    TEST_CASE("a sound without an entity is played once, where it is told to") {
        Stage stage;
        CHECK(stage.hear().silent());
        const AudioVoiceId shot = unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")}));
        CHECK(shot != 0);
        CHECK(stage.audio->isPlaying(shot));
        // The same in both ears, and counted as a voice that is no source.
        CHECK(stage.hear().last() == Vec2(0.5f));
        CHECK(stage.statistics.voices == 1);
        CHECK(stage.statistics.sources == 0);
        for (int update = 0; update < 9; ++update) {
            stage.hear();
            CHECK(stage.audio->isPlaying(shot));
        }
        stage.hear();
        CHECK_FALSE(stage.audio->isPlaying(shot));
        CHECK(stage.statistics.voices == 0);
        CHECK(stage.hear().silent());

        // From a place, as loud and as fast as it is told, on the bus it is told.
        REQUIRE(stage.audio->setVolume(AudioBus::Interface, 0.5f).has_value());
        const AudioVoiceId placed = unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav"),
                                                                             .bus = AudioBus::Interface,
                                                                             .volume = 2.0f,
                                                                             .pitch = 2.0f,
                                                                             .position = Vec3(-3.0f, 0.0f, 0.0f),
                                                                             .rolloff = AudioRolloff::Linear,
                                                                             .minDistance = 1.0f,
                                                                             .maxDistance = 5.0f}));
        CHECK(placed != shot);
        // Half-way between near and far, at the left: half of 0.5, twice, halved.
        CHECK(about(stage.hear().last(), Vec2(0.25f, 0.05f)));
        for (int update = 0; update < 5; ++update) {
            stage.hear();
        }
        CHECK_FALSE(stage.audio->isPlaying(placed));

        // Round and round, until it is stopped; stopping what is over does nothing.
        const AudioVoiceId round =
            unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav"), .loop = true}));
        for (int update = 0; update < 30; ++update) {
            CHECK(stage.level() == Vec2(0.5f));
        }
        CHECK(stage.audio->isPlaying(round));
        stage.audio->stop(round);
        CHECK_FALSE(stage.audio->isPlaying(round));
        CHECK(stage.hear().silent());
        stage.audio->stop(round);
        stage.audio->stop(shot);
        stage.audio->stop(0);
        stage.audio->stop(123456);
        CHECK_FALSE(stage.audio->isPlaying(0));
        CHECK_FALSE(stage.audio->isPlaying(123456));
    }

    TEST_CASE("a sound that cannot be played as described is not played, and the reason is given") {
        Stage stage;
        const Uuid steady = stage.idOf("steady.wav");
        const auto refused = [&](const AudioPlayDesc& desc) {
            const Error error = errorIn(stage.audio->play(*stage.assets, desc));
            CHECK(error.code == ErrorCode::InvalidArgument);
            return error.message;
        };
        CHECK(refused({}) == "there is no sound to play: its id is nil");
        CHECK(refused({.sound = steady, .bus = static_cast<AudioBus>(5)}) == "5 is no audio bus");
        CHECK(refused({.sound = steady, .volume = -1.0f}) == "the volume of a sound is from 0 to 4, not -1");
        CHECK(refused({.sound = steady, .volume = 4.5f}) == "the volume of a sound is from 0 to 4, not 4.5");
        CHECK(refused({.sound = steady, .volume = kNotANumber}) == "the volume of a sound is from 0 to 4, not nan");
        CHECK(refused({.sound = steady, .pitch = 0.2f}) == "the pitch of a sound is from 0.25 to 4, not 0.2");
        CHECK(refused({.sound = steady, .pitch = 5.0f}) == "the pitch of a sound is from 0.25 to 4, not 5");
        CHECK(refused({.sound = steady, .pitch = kInfinity}) == "the pitch of a sound is from 0.25 to 4, not inf");
        CHECK(refused({.sound = steady, .position = Vec3(0.0f, kNotANumber, 0.0f)}) ==
              "the position of a sound is three numbers");
        CHECK(refused({.sound = steady, .minDistance = 0.0f}) ==
              "the distances of a sound are from 0.01 to 10000 m, not 0");
        CHECK(refused({.sound = steady, .maxDistance = 20000.0f}) ==
              "the distances of a sound are from 0.01 to 10000 m, not 20000");
        CHECK(refused({.sound = steady, .maxDistance = kNotANumber}) ==
              "the distances of a sound are from 0.01 to 10000 m, not nan");
        // The ends of what is allowed are allowed.
        CHECK(
            stage.audio
                ->play(*stage.assets,
                       {.sound = steady, .volume = 4.0f, .pitch = 0.25f, .minDistance = 0.01f, .maxDistance = 10000.0f})
                .has_value());
        CHECK(stage.audio->play(*stage.assets, {.sound = steady, .volume = 0.0f, .pitch = 4.0f}).has_value());

        // What is no sound, in three ways: no such asset, an asset of another kind, a
        // file that holds none.
        const Error missing = errorIn(stage.audio->play(*stage.assets, {.sound = Uuid::fromName(Uuid(), "none")}));
        CHECK(missing.code == ErrorCode::NotFound);
        const Error script = errorIn(stage.audio->play(*stage.assets, {.sound = stage.idOf("notes.lua")}));
        CHECK(script.code == ErrorCode::InvalidArgument);
        CHECK(script.message == "notes.lua is a script, not a sound");
        const Error words = errorIn(stage.audio->play(*stage.assets, {.sound = stage.idOf("words.wav")}));
        CHECK(words.code == ErrorCode::ParseError);
        CHECK(words.message.starts_with("words.wav: it is no WAV, FLAC or MP3 file that can be read"));

        // None of them left anything playing but the two that were right.
        stage.hear();
        CHECK(stage.statistics.voices == 2);

        // A file that has been put right is a sound once the assets have been read again -
        // at once, without an update in between.
        stage.writeSound("words.wav", 48000, 1, kSoundFrames, [](size_t, uint32_t) { return 0.25f; });
        CHECK(errorIn(stage.audio->play(*stage.assets, {.sound = stage.idOf("words.wav")})).code ==
              ErrorCode::ParseError);
        REQUIRE(stage.assets->refresh().has_value());
        CHECK(stage.audio->play(*stage.assets, {.sound = stage.idOf("words.wav")}).has_value());
    }

    TEST_CASE("no more sounds are heard at once than there are voices") {
        Stage stage({.maxVoices = 3});
        std::vector<Entity> entities;
        for (int index = 0; index < 5; ++index) {
            entities.push_back(stage.source("quiet.wav"));
        }
        // The first three in the hierarchy play; the others wait, and are counted.
        CHECK(stage.level() == Vec2(0.375f));
        CHECK(stage.statistics.sources == 3);
        CHECK(stage.statistics.voices == 3);
        CHECK(stage.statistics.waiting == 2);
        for (const Entity entity : entities) {
            CHECK(stage.sourceOf(entity).playing);
        }

        // Nothing can be played on top of them.
        const Error full = errorIn(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")}));
        CHECK(full.code == ErrorCode::OutOfRange);
        CHECK(full.message == "3 sounds are playing already, which is as many as can be");

        // When one is over, the next in line begins.
        stage.sourceOf(entities[0]).playing = false;
        CHECK(stage.level() == Vec2(0.375f));
        CHECK(stage.statistics.waiting == 1);
        // And what play() starts takes a voice like any other: the last source waits on.
        stage.sourceOf(entities[1]).playing = false;
        stage.sourceOf(entities[2]).playing = false;
        stage.sourceOf(entities[3]).playing = false;
        stage.sourceOf(entities[4]).playing = false;
        stage.hear();
        CHECK(stage.statistics.voices == 0);
        for (int index = 0; index < 3; ++index) {
            CHECK(stage.audio->play(*stage.assets, {.sound = stage.idOf("quiet.wav")}).has_value());
        }
        stage.sourceOf(entities[4]).playing = true;
        CHECK(stage.level() == Vec2(0.375f));
        CHECK(stage.statistics.sources == 0);
        CHECK(stage.statistics.voices == 3);
        CHECK(stage.statistics.waiting == 1);
        // The three are over after a tenth of a second, and the source is heard.
        for (int update = 0; update < 10; ++update) {
            stage.hear();
        }
        CHECK(stage.level() == Vec2(0.125f));
        CHECK(stage.statistics.sources == 1);
        CHECK(stage.statistics.waiting == 0);
    }

    TEST_CASE("while a world is paused nothing is heard, and nothing moves on") {
        Stage stage;
        const Entity entity = stage.source("count.wav");
        const AudioVoiceId shot = unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("quiet.wav")}));
        const Heard before = stage.hear();
        const long reached = std::lround((before.last().x - 0.125f) * 32768.0f);
        CHECK(reached >= static_cast<long>(kTickFrames) - 2);

        stage.audio->pause();
        CHECK(stage.audio->mixed().empty());
        stage.audio->pause();
        // Whatever is asked of it meanwhile.
        CHECK(stage.audio->isPlaying(shot));
        CHECK(stage.sourceOf(entity).playing);

        // The next update goes on where it was held.
        const Heard after = stage.hear();
        CHECK(std::lround((after.left(0) - 0.125f) * 32768.0f) == reached + 1);
        CHECK(std::lround((after.last().x - 0.125f) * 32768.0f) == reached + static_cast<long>(kTickFrames));

        // What is started while it is held waits with the rest.
        stage.audio->pause();
        const AudioVoiceId held = unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")}));
        CHECK(stage.audio->isPlaying(held));
        CHECK(stage.audio->mixed().empty());
        CHECK(stage.hear().last().x > 0.5f);
    }

    TEST_CASE("clearing stops everything, and keeps how loud things are") {
        Stage stage;
        REQUIRE(stage.audio->setVolume(AudioBus::Effects, 0.5f).has_value());
        const Entity entity = stage.source("count.wav");
        const AudioVoiceId shot =
            unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav"), .loop = true}));
        CHECK(stage.hear().last().x > 0.25f);

        stage.audio->clear();
        CHECK(stage.audio->mixed().empty());
        CHECK_FALSE(stage.audio->isPlaying(shot));
        CHECK(stage.audio->volume(AudioBus::Effects) == 0.5f);

        // A world that is still there afterwards is played from the beginning: its
        // sources say they are playing. What play() had started is gone.
        const Heard after = stage.hear();
        CHECK(stage.statistics.voices == 1);
        CHECK(after.count(kTickFrames - 1) * 2 <= static_cast<long>(kTickFrames));
        CHECK(stage.sourceOf(entity).playing);
        // And a world that is gone is silence.
        stage.audio->clear();
        stage.world.destroyEntity(entity);
        CHECK(stage.hear().silent());
        // Ids are not handed out again.
        CHECK(unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")})) > shot);
    }

    TEST_CASE("a sound that cannot be loaded is reported once, and nothing else stops") {
        Stage stage;
        const LogCapture log(LogLevel::Error);
        const Entity good = stage.source("quiet.wav");
        stage.sourceOf(good).loop = true;
        const Entity broken = stage.source("words.wav");
        const Entity wrong = stage.source("notes.lua");
        const Entity missing = stage.source("quiet.wav");
        stage.sourceOf(missing).sound = *Uuid::parse("11111111-1111-4111-8111-111111111111");

        for (int update = 0; update < 3; ++update) {
            CHECK(stage.level() == Vec2(0.125f));
        }
        CHECK(stage.statistics.sources == 1);
        // They are not waiting for a voice, and they are not over: put right, they play.
        CHECK(stage.statistics.waiting == 0);
        CHECK(stage.sourceOf(broken).playing);
        CHECK(log.count(LogLevel::Error) == 3);
        CHECK(log.contains("an audio source cannot use the asset " + stage.idOf("words.wav").toString()));
        CHECK(log.contains("words.wav: it is no WAV, FLAC or MP3 file that can be read"));
        CHECK(log.contains("notes.lua is a script, not a sound"));
        CHECK(log.contains("there is no asset with the id 11111111-1111-4111-8111-111111111111"));

        // A second source with the same broken sound says nothing new.
        stage.source("words.wav");
        stage.hear();
        CHECK(log.count(LogLevel::Error) == 3);

        // And the file was read once, not for every frame its sources wait: gone from
        // the disk, it is still known as the file that holds no sound.
        const Uuid words = stage.idOf("words.wav");
        REQUIRE(std::filesystem::remove(stage.directory.path() / "assets" / "words.wav"));
        stage.hear();
        CHECK(log.count(LogLevel::Error) == 3);
        CHECK(errorIn(stage.audio->play(*stage.assets, {.sound = words})).code == ErrorCode::ParseError);

        // The file is put right, and the assets are read again: it plays.
        stage.writeSound("words.wav", 48000, 1, kSoundFrames, [](size_t, uint32_t) { return 0.25f; });
        REQUIRE(stage.assets->refresh().has_value());
        stage.world.destroyEntity(wrong);
        stage.world.destroyEntity(missing);
        CHECK(stage.level() == Vec2(0.625f));
        CHECK(stage.statistics.sources == 3);
        CHECK(log.count(LogLevel::Error) == 3);

        // What is still wrong after that is said again, once.
        const Entity still = stage.source("notes.lua");
        stage.hear();
        stage.hear();
        CHECK(log.count(LogLevel::Error) == 4);
        stage.world.destroyEntity(still);

        // Clearing forgets what was said, too.
        stage.audio->clear();
        stage.source("notes.lua");
        stage.hear();
        CHECK(log.count(LogLevel::Error) == 5);
    }

    TEST_CASE("as much sound is mixed as time passes, to the frame") {
        Stage stage;
        const Entity entity = stage.source("steady.wav");
        stage.sourceOf(entity).loop = true;
        CHECK(stage.hear(0.01).frames() == 480);
        CHECK(stage.hear(0.5 / 48000.0).frames() == 0);
        // Two halves make a frame: what is left over of one update is in the next.
        CHECK(stage.hear(0.5 / 48000.0).frames() == 1);
        size_t frames = 0;
        for (int update = 0; update < 60; ++update) {
            frames += stage.hear(1.0 / 60.0).frames();
        }
        CHECK(frames == 48000);

        // No time, less than none and what is no time at all is none.
        CHECK(stage.hear(0.0).frames() == 0);
        CHECK(stage.hear(-1.0).frames() == 0);
        CHECK(stage.hear(std::numeric_limits<double>::quiet_NaN()).frames() == 0);
        CHECK(stage.hear(-std::numeric_limits<double>::infinity()).frames() == 0);
        // A frame that took far too long is heard as a quarter of a second.
        CHECK(kMaxAudioUpdateSeconds == 0.25);
        CHECK(stage.hear(10.0).frames() == 12000);
        CHECK(stage.hear(std::numeric_limits<double>::infinity()).frames() == 0);
        CHECK(stage.level() == Vec2(0.5f));
    }

    TEST_CASE("the same world is heard the same way twice") {
        const auto run = [] {
            Stage stage;
            const Entity ears = stage.world.createEntity("Ears");
            stage.world.add<AudioListenerComponent>(ears);
            const Entity mover = stage.sourceAt("count.wav", Vec3(3.0f, 0.0f, -2.0f));
            stage.sourceOf(mover).loop = true;
            stage.sourceOf(mover).pitch = 1.3f;
            stage.sourceAt("slow.wav", Vec3(-4.0f, 1.0f, 0.5f));
            const Entity sides = stage.source("sides.wav");
            stage.sourceOf(sides).volume = 0.3f;
            Heard all;
            for (int update = 0; update < 20; ++update) {
                stage.world.tryGet<Transform>(mover)->position.x -= 0.25f;
                if (update == 7) {
                    static_cast<void>(unwrap(stage.audio->play(
                        *stage.assets, {.sound = stage.idOf("quiet.wav"), .position = Vec3(0.0f, 0.0f, 6.0f)})));
                }
                all.append(stage.hear(1.0 / 60.0));
            }
            return all.samples;
        };
        const std::vector<float> first = run();
        REQUIRE(first.size() == 32000);
        CHECK_FALSE(std::ranges::all_of(first, [](float sample) { return sample == 0.0f; }));
        CHECK(run() == first);
    }

    TEST_CASE("what is playing is counted: voices, and the sounds they play") {
        Stage stage;
        CHECK(stage.statistics == AudioStatistics());
        stage.hear();
        CHECK(stage.statistics == AudioStatistics());

        stage.source("steady.wav");
        stage.source("steady.wav");
        stage.source("sides.wav");
        const Entity idle = stage.source("count.wav");
        stage.sourceOf(idle).playing = false;
        static_cast<void>(unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")})));
        static_cast<void>(unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("slow.wav")})));
        stage.world.add<AudioListenerComponent>(stage.world.createEntity("Ears"));
        stage.hear();
        CHECK(stage.statistics.sources == 3);
        CHECK(stage.statistics.voices == 5);
        CHECK(stage.statistics.waiting == 0);
        CHECK(stage.statistics.listeners == 1);
        // Three sounds between them: one of 4800 frames, one of as many in two
        // channels, and one of 2400 - four bytes a sample.
        CHECK(stage.statistics.sounds == 3);
        CHECK(stage.statistics.soundBytes == (4800 + 9600 + 2400) * 4);
    }

    TEST_CASE("a device that plays to nobody plays at the speed of a real one") {
        Stage stage({.output = AudioOutput::Silent});
        CHECK(stage.audio->output() == AudioOutput::Silent);
        const Entity entity = stage.source("steady.wav");
        const AudioVoiceId shot = unwrap(stage.audio->play(*stage.assets, {.sound = stage.idOf("steady.wav")}));

        // A tenth of a second of sound is over by itself - nobody has to call update()
        // for it to be - and long before ten seconds have passed.
        const auto began = std::chrono::steady_clock::now();
        while (stage.audio->isPlaying(shot)) {
            REQUIRE(std::chrono::steady_clock::now() - began < std::chrono::seconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // A source is played from the first update that meets it, whatever that update
        // is told of time, and says that it is over to the first one after its end.
        while (stage.sourceOf(entity).playing) {
            REQUIRE(std::chrono::steady_clock::now() - began < std::chrono::seconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            // What it mixes is not for this thread to see.
            CHECK(stage.hear(0.0).frames() == 0);
        }
        CHECK(stage.statistics.voices == 0);

        // Held, it is not: what is paused waits, however long.
        stage.sourceOf(entity).playing = true;
        stage.hear(0.0);
        stage.audio->pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // (Looking is what update() does, and update() lets it go on.)
        stage.hear(0.0);
        CHECK(stage.sourceOf(entity).playing);
        stage.audio->clear();
    }
}
