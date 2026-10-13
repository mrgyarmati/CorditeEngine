#pragma once

/// @file
/// The components of sound: what is heard from an entity, and the entity that hears.
///
/// @code
/// const Entity radio = world.createEntity("Radio");
/// world.add<AudioSourceComponent>(radio) = {.sound = tuneId, .bus = AudioBus::Music,
///                                           .loop = true};
///
/// const Entity ears = world.createEntity("Ears");
/// world.add<AudioListenerComponent>(ears);
///
/// audio.update(world, assets, frameSeconds);                           // AudioSystem
/// const Entity heardFrom = findAudioListener(world);
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "AudioSource": {"sound": "0a56c2f1-21e0-819d-8bac-567d1e1c4345", "bus": "music",
///                 "playing": true, "loop": true, "volume": 1, "pitch": 1,
///                 "spatial": true, "rolloff": "inverse", "minDistance": 1, "maxDistance": 50},
/// "AudioListener": {"volume": 1}
/// @endcode
///
/// A source says what is to be heard; AudioSystem is what plays it. `playing` is the one
/// thing of a source the system changes: a sound that does not loop is over at its end,
/// and the source says so. A scene file therefore holds what plays when the scene starts.
///
/// Sound that belongs to no entity - a shot, a step, a click - needs no component:
/// AudioSystem::play() plays it once.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

/// What a sound is mixed with. Each bus has a volume of its own - the sliders of a
/// settings menu - that everything on it shares; see AudioSystem::setVolume().
enum class AudioBus : uint8_t {
    /// What happens in the world: steps, shots, doors.
    Effects,
    Music,
    /// What a place sounds like: wind, machines, a crowd far off.
    Ambience,
    /// What is said.
    Voice,
    /// What the game itself says: clicks, alerts.
    Interface,
};

inline constexpr std::array<AudioBus, 5> kAudioBuses = {
    AudioBus::Effects, AudioBus::Music, AudioBus::Ambience, AudioBus::Voice, AudioBus::Interface,
};

/// The lowercase name used in files and by scripts: "effects", "music", ...
std::string_view nameOf(AudioBus bus);
/// The bus of a name, whatever its case; nothing for a name no bus has.
std::optional<AudioBus> audioBusFromName(std::string_view name);

/// How a sound that is heard from a place gets quieter with distance.
enum class AudioRolloff : uint8_t {
    /// As in the open: half as loud at twice `minDistance`, a third at three times. It
    /// is never silent - beyond `maxDistance` it only gets no quieter.
    Inverse,
    /// Evenly, from all of it at `minDistance` to nothing at `maxDistance`.
    Linear,
};

/// The lowercase name used in files and by scripts: "inverse", "linear".
std::string_view nameOf(AudioRolloff rolloff);

/// The loudest a source or a bus can be made: four times as loud as it is.
inline constexpr float kMaxAudioVolume = 4.0f;
/// How slow and how fast a sound can be played: two octaves down, two up.
inline constexpr float kMinAudioPitch = 0.25f;
inline constexpr float kMaxAudioPitch = 4.0f;
/// The distances of a source, in metres.
inline constexpr float kMinAudioDistance = 0.01f;
inline constexpr float kMaxAudioDistance = 10000.0f;

/// A sound that is heard from an entity.
struct AudioSourceComponent {
    /// The sound asset that is played. Nil is none, and silence.
    Uuid sound;
    /// What it is mixed with, and whose volume it shares.
    AudioBus bus = AudioBus::Effects;
    /// Whether it is playing. It starts, at its beginning, when this becomes true, and
    /// stops when it becomes false. A sound that does not loop sets it to false itself
    /// at its end. In a scene file: whether it plays when the scene starts.
    bool playing = true;
    /// At its end the sound starts again; without, it is over.
    bool loop = false;
    /// How loud it is: 1 as the sound was made, 0 not at all, up to kMaxAudioVolume.
    float volume = 1.0f;
    /// How fast it is played: 1 as it was made, 2 twice as fast and an octave higher,
    /// 0.5 half as fast and an octave lower.
    float pitch = 1.0f;
    /// Heard from where the entity is: quieter with distance, and from its side. Without,
    /// it is the same in both ears wherever the entity is - music, a voice in the head.
    bool spatial = true;
    /// How it gets quieter with distance. Only for a spatial source.
    AudioRolloff rolloff = AudioRolloff::Inverse;
    /// Up to this distance from the listener, in metres, it is as loud as it gets.
    float minDistance = 1.0f;
    /// Beyond this distance, in metres, it gets no quieter - or, with AudioRolloff::Linear,
    /// is not heard at all. Taken to be above `minDistance` when it is not.
    float maxDistance = 50.0f;
};

/// The entity that hears: sources are heard from where they are in relation to it,
/// along its -Z axis with +Y up. A world needs one; see findAudioListener().
struct AudioListenerComponent {
    /// How loud everything is to this listener: 1 as it is, 0 nothing - ears under
    /// water, a scene that fades out.
    float volume = 1.0f;
};

/// The entity sound is heard from: the first enabled one with an AudioListener, in the
/// order of the hierarchy. The null entity when there is none - then everything is heard
/// from the origin of the world, looking along -Z.
[[nodiscard]] Entity findAudioListener(const World& world);

/// Registers the components of the audio module: "AudioSource" and "AudioListener".
[[nodiscard]] Status registerAudioComponents(ComponentRegistry& registry);

} // namespace cordite
