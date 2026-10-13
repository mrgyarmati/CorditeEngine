#include "cordite/audio/AudioComponents.h"

#include "cordite/core/Reflection.h"
#include "cordite/core/StringUtils.h"

#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

std::vector<std::pair<AudioBus, std::string>> busOptions() {
    std::vector<std::pair<AudioBus, std::string>> options;
    options.reserve(kAudioBuses.size());
    for (const AudioBus bus : kAudioBuses) {
        options.emplace_back(bus, std::string(nameOf(bus)));
    }
    return options;
}

TypeInfo describeAudioSource() {
    return TypeBuilder<AudioSourceComponent>("AudioSource")
        .property("sound", &AudioSourceComponent::sound,
                  {.description = "The sound that is played; without one there is silence",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Sound"})
        .enumProperty("bus", &AudioSourceComponent::bus, busOptions(),
                      {.description = "What it is mixed with, and whose volume it shares"})
        .property("playing", &AudioSourceComponent::playing,
                  {.description = "Whether it is playing: it starts when this becomes true, and a sound that does "
                                  "not loop sets it to false at its end. In a scene file, whether it plays when "
                                  "the scene starts"})
        .property("loop", &AudioSourceComponent::loop,
                  {.description = "At its end the sound starts again; without, it is over"})
        .property("volume", &AudioSourceComponent::volume,
                  {.description = "How loud it is: 1 as the sound was made, 0 not at all",
                   .minimum = 0.0,
                   .maximum = kMaxAudioVolume,
                   .step = 0.01})
        .property("pitch", &AudioSourceComponent::pitch,
                  {.description = "How fast it is played: 1 as it was made, 2 twice as fast and an octave higher",
                   .minimum = kMinAudioPitch,
                   .maximum = kMaxAudioPitch,
                   .step = 0.01})
        .property("spatial", &AudioSourceComponent::spatial,
                  {.description = "Heard from where the entity is, quieter with distance; without, the same in "
                                  "both ears wherever it is"})
        .enumProperty("rolloff", &AudioSourceComponent::rolloff,
                      {{AudioRolloff::Inverse, std::string(nameOf(AudioRolloff::Inverse))},
                       {AudioRolloff::Linear, std::string(nameOf(AudioRolloff::Linear))}},
                      {.description = "How it gets quieter with distance: inverse as in the open, never silent; "
                                      "linear evenly, to nothing at maxDistance"})
        .property("minDistance", &AudioSourceComponent::minDistance,
                  {.description = "Up to this distance from the listener it is as loud as it gets",
                   .minimum = kMinAudioDistance,
                   .maximum = kMaxAudioDistance,
                   .step = 0.1,
                   .unit = "m"})
        .property("maxDistance", &AudioSourceComponent::maxDistance,
                  {.description = "Beyond this distance it gets no quieter, or with a linear rolloff is not heard",
                   .minimum = kMinAudioDistance,
                   .maximum = kMaxAudioDistance,
                   .step = 0.1,
                   .unit = "m"})
        .build();
}

TypeInfo describeAudioListener() {
    return TypeBuilder<AudioListenerComponent>("AudioListener")
        .property("volume", &AudioListenerComponent::volume,
                  {.description = "How loud everything is to this listener: 1 as it is, 0 nothing",
                   .minimum = 0.0,
                   .maximum = kMaxAudioVolume,
                   .step = 0.01})
        .build();
}

} // namespace

std::string_view nameOf(AudioBus bus) {
    switch (bus) {
        case AudioBus::Effects:
            return "effects";
        case AudioBus::Music:
            return "music";
        case AudioBus::Ambience:
            return "ambience";
        case AudioBus::Voice:
            return "voice";
        case AudioBus::Interface:
            return "interface";
    }
    return "unknown";
}

std::optional<AudioBus> audioBusFromName(std::string_view name) {
    for (const AudioBus bus : kAudioBuses) {
        if (equalsIgnoreCase(name, nameOf(bus))) {
            return bus;
        }
    }
    return std::nullopt;
}

std::string_view nameOf(AudioRolloff rolloff) {
    switch (rolloff) {
        case AudioRolloff::Inverse:
            return "inverse";
        case AudioRolloff::Linear:
            return "linear";
    }
    return "unknown";
}

Entity findAudioListener(const World& world) {
    Entity found;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (found.isNull() && world.has<AudioListenerComponent>(entity) && world.isEnabledInHierarchy(entity)) {
            found = entity;
        }
    });
    return found;
}

Status registerAudioComponents(ComponentRegistry& registry) {
    CORDITE_TRY(registry.add<AudioSourceComponent>(
        describeAudioSource(), {.description = "A sound that is heard from the entity: which one, how loud, and "
                                               "how it carries."}));
    CORDITE_TRY(registry.add<AudioListenerComponent>(
        describeAudioListener(), {.description = "The entity that hears: sound is heard from where its sources are "
                                                 "in relation to it. The first enabled one counts."}));
    return {};
}

} // namespace cordite
