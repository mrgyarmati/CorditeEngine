#include "cordite/audio/AudioComponents.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <set>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::unwrap;

TEST_SUITE("AudioComponents") {
    TEST_CASE("the components of sound are registered under the names scene files use, and described") {
        ComponentRegistry registry;
        REQUIRE(registerAudioComponents(registry).has_value());
        REQUIRE(registry.components().size() == 2);
        for (const char* name : {"AudioSource", "AudioListener"}) {
            CAPTURE(name);
            const ComponentDescriptor* descriptor = registry.find(name);
            REQUIRE(descriptor != nullptr);
            CHECK(descriptor->traits.optional);
            CHECK_FALSE(descriptor->traits.description.empty());
            for (const PropertyInfo& property : descriptor->type.properties()) {
                CAPTURE(property.name());
                CHECK_FALSE(property.attributes().description.empty());
            }
        }
        // Registered once.
        CHECK_FALSE(registerAudioComponents(registry).has_value());

        const TypeInfo& source = registry.find("AudioSource")->type;
        REQUIRE(source.properties().size() == 10);
        const PropertyInfo* sound = source.findProperty("sound");
        REQUIRE(sound != nullptr);
        CHECK(sound->attributes().semantic == PropertySemantic::AssetReference);
        CHECK(sound->attributes().referenceType == "Sound");
        CHECK(source.findProperty("minDistance")->attributes().unit == "m");
        CHECK(source.findProperty("maxDistance")->attributes().unit == "m");
        // A source is as loud and as fast as the limits say, and no more.
        CHECK(source.findProperty("volume")->attributes().minimum == 0.0);
        CHECK(source.findProperty("volume")->attributes().maximum == 4.0);
        CHECK(source.findProperty("pitch")->attributes().minimum == 0.25);
        CHECK(source.findProperty("pitch")->attributes().maximum == 4.0);

        // Every bus can be chosen, and by the name it has everywhere.
        const PropertyInfo* bus = source.findProperty("bus");
        REQUIRE(bus != nullptr);
        REQUIRE(bus->enumOptions().size() == kAudioBuses.size());
        for (size_t index = 0; index < kAudioBuses.size(); ++index) {
            CHECK(bus->enumOptions()[index].name == nameOf(kAudioBuses[index]));
        }
        const PropertyInfo* rolloff = source.findProperty("rolloff");
        REQUIRE(rolloff != nullptr);
        REQUIRE(rolloff->enumOptions().size() == 2);
        CHECK(rolloff->enumOptions()[0].name == "inverse");
        CHECK(rolloff->enumOptions()[1].name == "linear");

        const TypeInfo& listener = registry.find("AudioListener")->type;
        REQUIRE(listener.properties().size() == 1);
        CHECK(listener.findProperty("volume")->attributes().maximum == 4.0);
    }

    TEST_CASE("every bus has a name of its own, and is found by it") {
        CHECK(kAudioBuses.size() == 5);
        CHECK(nameOf(AudioBus::Effects) == "effects");
        CHECK(nameOf(AudioBus::Music) == "music");
        CHECK(nameOf(AudioBus::Ambience) == "ambience");
        CHECK(nameOf(AudioBus::Voice) == "voice");
        CHECK(nameOf(AudioBus::Interface) == "interface");
        std::set<std::string_view> names;
        for (size_t index = 0; index < kAudioBuses.size(); ++index) {
            const AudioBus bus = kAudioBuses[index];
            // In the order of their values: a bus is the index of its volume.
            CHECK(static_cast<size_t>(bus) == index);
            CHECK(audioBusFromName(nameOf(bus)) == bus);
            CHECK(names.insert(nameOf(bus)).second);
        }
        CHECK(audioBusFromName("MUSIC") == AudioBus::Music);
        CHECK(audioBusFromName("Voice") == AudioBus::Voice);
        CHECK_FALSE(audioBusFromName("").has_value());
        CHECK_FALSE(audioBusFromName("master").has_value());
        CHECK_FALSE(audioBusFromName("musics").has_value());
        CHECK(nameOf(static_cast<AudioBus>(9)) == "unknown");

        CHECK(nameOf(AudioRolloff::Inverse) == "inverse");
        CHECK(nameOf(AudioRolloff::Linear) == "linear");
        CHECK(nameOf(static_cast<AudioRolloff>(9)) == "unknown");
    }

    TEST_CASE("a source is saved with every property and read back the same") {
        ComponentRegistry registry;
        REQUIRE(registerAudioComponents(registry).has_value());
        const TypeInfo& type = registry.find("AudioSource")->type;

        // A new one plays what it is given once, from where it is.
        const AudioSourceComponent fresh;
        CHECK(type.toJson(&fresh) == unwrap(parseJson(R"({"sound": null, "bus": "effects", "playing": true,
                                                         "loop": false, "volume": 1.0, "pitch": 1.0,
                                                         "spatial": true, "rolloff": "inverse",
                                                         "minDistance": 1.0, "maxDistance": 50.0})")));

        const AudioSourceComponent source{.sound = *Uuid::parse("0a56c2f1-21e0-819d-8bac-567d1e1c4345"),
                                          .bus = AudioBus::Ambience,
                                          .playing = false,
                                          .loop = true,
                                          .volume = 0.25f,
                                          .pitch = 1.5f,
                                          .spatial = false,
                                          .rolloff = AudioRolloff::Linear,
                                          .minDistance = 2.5f,
                                          .maxDistance = 80.0f};
        const Json saved = type.toJson(&source);
        CHECK(saved == unwrap(parseJson(R"({"sound": "0a56c2f1-21e0-819d-8bac-567d1e1c4345", "bus": "ambience",
                                           "playing": false, "loop": true, "volume": 0.25, "pitch": 1.5,
                                           "spatial": false, "rolloff": "linear",
                                           "minDistance": 2.5, "maxDistance": 80.0})")));

        AudioSourceComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.sound == source.sound);
        CHECK(loaded.bus == AudioBus::Ambience);
        CHECK_FALSE(loaded.playing);
        CHECK(loaded.loop);
        CHECK(loaded.volume == 0.25f);
        CHECK(loaded.pitch == 1.5f);
        CHECK_FALSE(loaded.spatial);
        CHECK(loaded.rolloff == AudioRolloff::Linear);
        CHECK(loaded.minDistance == 2.5f);
        CHECK(loaded.maxDistance == 80.0f);

        // What is out of range, of the wrong kind or not a property is refused, and
        // refused whole.
        for (const char* text : {
                 R"({"volume": -0.1})",
                 R"({"volume": 4.5})",
                 R"({"pitch": 0.2})",
                 R"({"pitch": 4.1})",
                 R"({"minDistance": 0.0})",
                 R"({"maxDistance": 10001.0})",
                 R"({"bus": "master"})",
                 R"({"bus": 1})",
                 R"({"rolloff": "exponential"})",
                 R"({"sound": "sounds/click.wav"})",
                 R"({"loop": "yes"})",
                 R"({"time": 1.0})",
                 R"({"volume": 0.5, "pitch": 9.0})",
             }) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&loaded, unwrap(parseJson(text))).has_value());
            CHECK(loaded.volume == 0.25f);
            CHECK(loaded.pitch == 1.5f);
        }
        // The ends of what is allowed are allowed.
        REQUIRE(type.fromJson(&loaded, unwrap(parseJson(R"({"volume": 4.0, "pitch": 0.25, "minDistance": 0.01,
                                                            "maxDistance": 10000.0})")))
                    .has_value());
        CHECK(loaded.volume == 4.0f);
        CHECK(loaded.pitch == 0.25f);
    }

    TEST_CASE("a listener is saved with how loud it hears") {
        ComponentRegistry registry;
        REQUIRE(registerAudioComponents(registry).has_value());
        const TypeInfo& type = registry.find("AudioListener")->type;

        AudioListenerComponent listener;
        CHECK(type.toJson(&listener) == unwrap(parseJson(R"({"volume": 1.0})")));
        REQUIRE(type.fromJson(&listener, unwrap(parseJson(R"({"volume": 0.5})"))).has_value());
        CHECK(listener.volume == 0.5f);
        CHECK_FALSE(type.fromJson(&listener, unwrap(parseJson(R"({"volume": -1.0})"))).has_value());
        CHECK_FALSE(type.fromJson(&listener, unwrap(parseJson(R"({"volume": 5.0})"))).has_value());
        CHECK(listener.volume == 0.5f);
    }

    TEST_CASE("the listener of a world is the first enabled one, in the order of the hierarchy") {
        World world;
        CHECK(findAudioListener(world).isNull());

        const Entity room = world.createEntity("Room");
        const Entity player = world.createEntity("Player");
        const Entity head = world.createEntity("Head");
        const Entity camera = world.createEntity("Camera");
        REQUIRE(world.setParent(head, player).has_value());
        // Nobody listens yet.
        CHECK(findAudioListener(world).isNull());

        world.add<AudioListenerComponent>(camera);
        CHECK(findAudioListener(world) == camera);
        // One that comes earlier takes over: the head of the player, below the second
        // root, is before the fourth root.
        world.add<AudioListenerComponent>(head);
        CHECK(findAudioListener(world) == head);
        world.add<AudioListenerComponent>(room);
        CHECK(findAudioListener(world) == room);

        // Switched off, itself or with what it hangs under, it does not count.
        world.setEnabled(room, false);
        CHECK(findAudioListener(world) == head);
        world.setEnabled(player, false);
        CHECK(findAudioListener(world) == camera);
        world.setEnabled(camera, false);
        CHECK(findAudioListener(world).isNull());
        world.setEnabled(player, true);
        CHECK(findAudioListener(world) == head);
        world.remove<AudioListenerComponent>(head);
        CHECK(findAudioListener(world).isNull());
    }
}
