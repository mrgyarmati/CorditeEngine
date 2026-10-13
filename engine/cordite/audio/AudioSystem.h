#pragma once

/// @file
/// Plays the sounds of a world: what its audio sources say is heard from where they are,
/// by the entity that listens.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<AudioSystem> audio,
///                    AudioSystem::create({.output = AudioOutput::Device}));
///
/// // A game, every frame, after everything has been moved:
/// audio->update(world, assets, frameSeconds);
/// // While time does not pass - a pause, an editor - nothing is heard:
/// audio->pause();
///
/// // A sound that belongs to no entity, once:
/// CORDITE_TRY_ASSIGN(const AudioVoiceId shot,
///                    audio->play(assets, {.sound = shotId, .position = Vec3(4.0f, 1.0f, 0.0f)}));
/// CORDITE_TRY(audio->setVolume(AudioBus::Music, 0.5f));
/// @endcode
///
/// Mixing, placing sound in space and the devices that play it are miniaudio's. Sound is
/// mixed in stereo. What is heard of a spatial source depends on where it is from the
/// listener: how far decides how loud (AudioRolloff), and the side decides the ear - a
/// source straight ahead is half as loud in both, one at a side as loud as it gets in
/// that ear and a fifth of it in the other.
///
/// Where the mix goes is chosen once (AudioOutput). With a device it is mixed on a thread
/// of the device, as the device asks for it; this class is still used from one thread
/// only. Offline nothing is heard and no thread runs: update() mixes as much sound as it
/// is told time has passed, the same every time, and mixed() is what it came to - which
/// is how the tests hear.

#include "cordite/assets/AssetManager.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace cordite {

/// Where what is mixed goes.
enum class AudioOutput : uint8_t {
    /// Nowhere, and no faster than update() says time passes: the same every run. Tests,
    /// tools, and programs nobody listens to.
    Offline,
    /// To what the machine plays sound with. A machine that has nothing of the kind
    /// gets Silent instead, and a warning.
    Device,
    /// To a device that plays to nobody, at the speed of a real one.
    Silent,
};

/// The lowercase name: "offline", "device", "silent".
std::string_view nameOf(AudioOutput output);

struct AudioSystemDesc {
    AudioOutput output = AudioOutput::Offline;
    /// Frames per second everything is mixed at; sounds of another rate are converted
    /// as they play. From kMinSoundSampleRate to kMaxSoundSampleRate.
    uint32_t sampleRate = 48000;
    /// How many sounds are heard at once, from 1 to kMaxAudioVoices. What would be one
    /// more is not started: a source waits, play() fails.
    uint32_t maxVoices = 64;
};

/// The most voices an AudioSystem can be made for.
inline constexpr uint32_t kMaxAudioVoices = 1024;

/// The longest time one update() lets pass, in seconds. A frame that took longer -
/// a breakpoint, a window that was dragged - is heard as this much.
inline constexpr double kMaxAudioUpdateSeconds = 0.25;

/// What was heard, the last time the sounds of a world were brought up to date.
struct AudioStatistics {
    /// Audio sources that are playing.
    uint32_t sources = 0;
    /// Everything that is playing: those, and what play() started.
    uint32_t voices = 0;
    /// Sources that are to play and are not, because as much as can be is playing.
    uint32_t waiting = 0;
    /// Enabled listeners in the world. One is right; the first is the one that hears.
    uint32_t listeners = 0;
    /// The sounds that are playing, each counted once, and the bytes of their samples.
    uint32_t sounds = 0;
    uint64_t soundBytes = 0;

    bool operator==(const AudioStatistics&) const = default;
};

/// Names something play() started. Never 0, and never the same twice.
using AudioVoiceId = uint64_t;

/// A sound to play once, without an entity. See AudioSystem::play().
struct AudioPlayDesc {
    Uuid sound;
    AudioBus bus = AudioBus::Effects;
    /// From 0 to kMaxAudioVolume.
    float volume = 1.0f;
    /// From kMinAudioPitch to kMaxAudioPitch.
    float pitch = 1.0f;
    /// Until it is stopped, instead of once.
    bool loop = false;
    /// Where in the world it is heard from. Without, it is the same in both ears.
    std::optional<Vec3> position;
    /// How it carries, when it has a position. See AudioSourceComponent.
    AudioRolloff rolloff = AudioRolloff::Inverse;
    float minDistance = 1.0f;
    float maxDistance = 50.0f;
};

class AudioSystem {
public:
    /// Fails with InvalidArgument for a description outside its limits, and with
    /// DeviceError when the mixer cannot be made. A machine without a sound device is
    /// not a failure; see AudioOutput::Device.
    [[nodiscard]] static Result<std::unique_ptr<AudioSystem>> create(const AudioSystemDesc& desc = {});
    ~AudioSystem();
    CORDITE_NON_MOVABLE(AudioSystem);

    /// Where the mix goes - Silent when a device was asked for and there is none.
    [[nodiscard]] AudioOutput output() const;
    /// Frames per second of the mix.
    [[nodiscard]] uint32_t sampleRate() const;

    /// Brings what is heard up to date with the world, and lets `deltaSeconds` pass.
    ///
    /// Every enabled entity whose AudioSource is `playing` and names a sound is given a
    /// voice, in the order of the hierarchy, and the voice follows the source: its
    /// volume, pitch, looping and how it carries, and where the entity is. Changing the
    /// sound or the bus of a source that plays starts it again. Sources that are not
    /// playing any more, are disabled or gone fall silent. A sound that does not loop
    /// and has ended sets `playing` of its source to false. The first enabled
    /// AudioListener is where everything is heard from.
    ///
    /// A sound that cannot be loaded is reported once, and its source waits in silence.
    /// Its file is not read again until the assets are (AssetManager::refresh()) or
    /// clear() is called: a file that is broken is not read for every frame.
    /// Less than no time, and a time that is no number, is no time; more than
    /// kMaxAudioUpdateSeconds is that much. World matrices must be up to date
    /// (World::updateWorldTransforms).
    AudioStatistics update(World& world, AssetManager& assets, double deltaSeconds);

    /// Holds everything where it is: nothing is heard and no sound moves on until the
    /// next update(). For a world whose time does not pass.
    void pause();

    /// Stops everything, and forgets which sounds could not be played. For a world that
    /// is replaced; one that is still there afterwards is heard from the beginning, as
    /// its sources say.
    void clear();

    /// Starts a sound that belongs to no entity. It is heard with the next update(),
    /// stops at its end unless it loops, and is over when the world is cleared. Fails
    /// with InvalidArgument for a description outside its limits (the message names
    /// what), as AssetManager::loadSound() does for a sound that cannot be loaded -
    /// which is tried once, as update() tries it - and with OutOfRange when as many
    /// voices as can be are playing.
    [[nodiscard]] Result<AudioVoiceId> play(AssetManager& assets, const AudioPlayDesc& desc);
    /// Stops what play() started. Nothing happens for what has ended or never was.
    void stop(AudioVoiceId voice);
    /// Whether what play() started is still to be heard: not stopped, not at its end.
    [[nodiscard]] bool isPlaying(AudioVoiceId voice) const;

    /// How loud everything on a bus is: 1 as it is, 0 not at all.
    [[nodiscard]] float volume(AudioBus bus) const;
    /// Fails with InvalidArgument for a volume that is no number or outside 0 to
    /// kMaxAudioVolume, and changes nothing then.
    [[nodiscard]] Status setVolume(AudioBus bus, float volume);
    /// How loud everything is, on top of the buses and of what the listener says.
    [[nodiscard]] float masterVolume() const;
    [[nodiscard]] Status setMasterVolume(float volume);

    /// AudioOutput::Offline only: what the last update() mixed - left and right for
    /// every frame, each from -1 to 1. Empty otherwise, and after pause() and clear().
    [[nodiscard]] std::span<const float> mixed() const;

private:
    struct Impl;
    explicit AudioSystem(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
