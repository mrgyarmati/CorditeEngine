#include "cordite/audio/AudioComponents.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/math/Math.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// A stage that listens, with two sounds in it: `long.wav`, two seconds of a quarter of
/// all there is, and `short.wav`, a tenth of a second of a half. And a file that is no
/// sound.
struct Studio {
    explicit Studio(const AudioSystemDesc& desc = {}) {
        stage.listen(desc);
        longSound = stage.writeSound("sounds/long.wav", 0.25f, 2.0);
        shortSound = stage.writeSound("sounds/short.wav", 0.5f, 0.1);
        stage.write("scripts/notes.lua", "return {}");
    }

    /// What both ears heard at the end of the last frame of the stage.
    [[nodiscard]] Vec2 level() const {
        const std::span<const float> mixed = stage.audio->mixed();
        REQUIRE(mixed.size() >= 2);
        return {mixed[mixed.size() - 2], mixed[mixed.size() - 1]};
    }

    /// Whether the last frame of the stage was silence from beginning to end.
    [[nodiscard]] bool silent() const {
        return std::ranges::all_of(stage.audio->mixed(), [](float sample) { return sample == 0.0f; });
    }

    /// Plays `long.wav` with `options`, written as Lua, lets half a second pass, and
    /// returns what both ears heard at the end of it. Nothing else is playing.
    Vec2 heardWith(std::string_view options) {
        stage.audio->clear();
        static_cast<void>(stage.lines("cordite.audio.play('sounds/long.wav', " + std::string(options) + ")"));
        return level();
    }

    ScriptStage stage;
    Uuid longSound;
    Uuid shortSound;
};

/// Whether two levels are the same, as closely as the mixer works out a distance.
bool about(const Vec2& heard, const Vec2& expected) {
    return std::abs(heard.x - expected.x) < 1.0e-3f && std::abs(heard.y - expected.y) < 1.0e-3f;
}

} // namespace

TEST_SUITE("ScriptAudioApi") {
    TEST_CASE("a script plays a sound by where its file is, or by its id, and is given a number for it") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        CHECK(stage.line("print(math.type(cordite.audio.play('sounds/long.wav')))") == "integer");
        // It is heard, the same in both ears, on top of nothing else; and it is a voice
        // that is no audio source.
        CHECK(studio.level() == Vec2(0.25f));
        CHECK(stage.heard.voices == 1);
        CHECK(stage.heard.sources == 0);

        // By its id, as the assets give it: a second one, on top of the first.
        CHECK(stage.line("print(cordite.audio.play(cordite.assets.find('sounds/long.wav')) > 0)") == "true");
        CHECK(studio.level() == Vec2(0.5f));
        CHECK(stage.line("print(cordite.audio.play('" + studio.longSound.toString() + "') > 0)") == "true");
        CHECK(studio.level() == Vec2(0.75f));
        CHECK(stage.heard.voices == 3);

        // Every sound that is played has a number of its own.
        CHECK(stage.line("local a, b = cordite.audio.play('sounds/short.wav'), cordite.audio.play('sounds/short.wav')\n"
                         "print(a ~= b, cordite.audio.isPlaying(a), cordite.audio.isPlaying(b))") ==
              "true\ttrue\ttrue");
    }

    TEST_CASE("a sound that a script plays is over at its end, and one that loops when the script stops it") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        stage.add("Player", stage.write("scripts/player.lua", R"(
            local Player = {}
            function Player:start()
                self.frames = 0
                self.once = cordite.audio.play("sounds/short.wav")
                self.round = cordite.audio.play("sounds/short.wav", { loop = true, volume = 0.5 })
            end
            function Player:update()
                self.frames = self.frames + 1
                if self.frames == 5 then cordite.audio.stop(self.round) end
                print(cordite.audio.isPlaying(self.once), cordite.audio.isPlaying(self.round))
                if self.frames == 6 then
                    -- What is over, what never was and nothing at all can be stopped, and
                    -- asked about: nothing happens, and none of it is playing.
                    cordite.audio.stop(self.round)
                    cordite.audio.stop(self.once)
                    cordite.audio.stop(nil)
                    cordite.audio.stop()
                    cordite.audio.stop(123456)
                    print(cordite.audio.isPlaying(nil), cordite.audio.isPlaying(), cordite.audio.isPlaying(0),
                          cordite.audio.isPlaying(-3), cordite.audio.isPlaying(123456))
                end
            end
            return Player
        )"));

        // A tenth of a second, in frames of a twentieth: heard through two of them, both
        // at once, and at its end in the third - which is what it says a frame later.
        stage.update(0.05);
        CHECK(studio.level() == Vec2(0.75f));
        stage.update(0.05);
        CHECK(studio.level() == Vec2(0.75f));
        stage.update(0.05);
        // The one that goes round is heard on.
        CHECK(studio.level() == Vec2(0.25f));
        CHECK(stage.heard.voices == 1);
        CHECK(stage.said() == Lines{"true\ttrue", "true\ttrue", "true\ttrue"});
        stage.update(0.05);
        CHECK(stage.said() == Lines{"false\ttrue"});
        CHECK(studio.level() == Vec2(0.25f));

        // Stopped, it is silent in the frame it was stopped in.
        stage.update(0.05);
        CHECK(stage.said() == Lines{"false\tfalse"});
        CHECK(studio.silent());
        CHECK(stage.heard.voices == 0);
        stage.update(0.05);
        CHECK(stage.said() == Lines{"false\tfalse", "false\tfalse\tfalse\tfalse\tfalse"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("the options of a sound say how it is heard") {
        Studio studio;
        // As it is: a quarter, in both ears.
        CHECK(studio.heardWith("nil") == Vec2(0.25f));
        CHECK(studio.heardWith("{}") == Vec2(0.25f));
        CHECK(studio.heardWith("{volume = 2}") == Vec2(0.5f));
        CHECK(studio.heardWith("{volume = 0}") == Vec2(0.0f));

        // On the bus it names, whose volume it shares - and on that of the effects when
        // it names none.
        REQUIRE(studio.stage.audio->setVolume(AudioBus::Music, 0.5f).has_value());
        CHECK(studio.heardWith("{bus = 'music'}") == Vec2(0.125f));
        CHECK(studio.heardWith("{bus = 'Music', volume = 2}") == Vec2(0.25f));
        CHECK(studio.heardWith("{bus = 'voice'}") == Vec2(0.25f));
        REQUIRE(studio.stage.audio->setVolume(AudioBus::Effects, 0.0f).has_value());
        CHECK(studio.heardWith("{}") == Vec2(0.0f));
        CHECK(studio.heardWith("{bus = 'effects'}") == Vec2(0.0f));
        REQUIRE(studio.stage.audio->setVolume(AudioBus::Effects, 1.0f).has_value());

        // From a place: five metres to the right it is a fifth as loud, and a fifth of
        // that in the far ear. A vector may be written in passing, or be one that computes.
        CHECK(about(studio.heardWith("{position = {5, 0, 0}}"), Vec2(0.01f, 0.05f)));
        CHECK(about(studio.heardWith("{position = cordite.vec3(-5, 0, 0)}"), Vec2(0.05f, 0.01f)));
        CHECK(about(studio.heardWith("{position = {x = 0, y = 0, z = 0}}"), Vec2(0.25f)));
        // How it carries: evenly, to nothing at the far distance.
        CHECK(about(studio.heardWith("{position = {-5, 0, 0}, rolloff = 'linear', minDistance = 1, maxDistance = 9}"),
                    Vec2(0.125f, 0.025f)));
        CHECK(about(studio.heardWith("{position = {-5, 0, 0}, rolloff = 'inverse', minDistance = 2.5}"),
                    Vec2(0.125f, 0.025f)));
        CHECK(studio.heardWith("{position = {0, 0, -20}, rolloff = 'linear', maxDistance = 10}") == Vec2(0.0f));
        // Without a place, how it would carry is not asked.
        CHECK(studio.heardWith("{rolloff = 'linear', minDistance = 0.5, maxDistance = 0.6}") == Vec2(0.25f));

        // Twice as fast, two seconds of sound are over in one - and not before.
        ScriptStage& stage = studio.stage;
        static_cast<void>(studio.heardWith("{pitch = 2}"));
        stage.update(0.25);
        CHECK(stage.heard.voices == 1);
        stage.update(0.25);
        stage.update(0.25);
        CHECK(stage.heard.voices == 0);
        // As it was made, it is not over by then.
        static_cast<void>(studio.heardWith("{pitch = 1}"));
        for (int frame = 0; frame < 3; ++frame) {
            stage.update(0.25);
        }
        CHECK(stage.heard.voices == 1);
    }

    TEST_CASE("what cannot be played is refused, and the script is told why") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        const auto refused = [&](std::string_view arguments) {
            return stage.refusal("cordite.audio.play(" + std::string(arguments) + ")");
        };
        // What is no sound.
        CHECK(refused("") == "bad argument #1 to 'play' (string expected, got no value)");
        CHECK(refused("{}") == "bad argument #1 to 'play' (string expected, got table)");
        CHECK(refused("'sounds/none.wav'") == "bad argument #1 to 'play' (there is no asset 'sounds/none.wav')");
        CHECK(refused("'11111111-1111-4111-8111-111111111111'") ==
              "bad argument #1 to 'play' (there is no asset '11111111-1111-4111-8111-111111111111')");
        CHECK(refused("'scripts/notes.lua'") ==
              "bad argument #1 to 'play' ('scripts/notes.lua' is of the kind script, not a sound)");

        // Options that are none.
        const std::string options = "bus, volume, pitch, loop, position, rolloff, minDistance, maxDistance";
        CHECK(refused("'sounds/long.wav', 5") == "bad argument #2 to 'play' (table expected, got number)");
        CHECK(refused("'sounds/long.wav', {loud = 1}") == "a sound has no option 'loud'; it has: " + options);
        CHECK(refused("'sounds/long.wav', {Volume = 1}") == "a sound has no option 'Volume'; it has: " + options);
        CHECK(refused("'sounds/long.wav', {1, 2}") == "the options of a sound have names: " + options);
        CHECK(refused("'sounds/long.wav', {volume = 1, [7] = true}") ==
              "the options of a sound have names: " + options);

        // Options of the wrong kind.
        CHECK(refused("'sounds/long.wav', {volume = 'loud'}") == "the option 'volume' is a number, not a string");
        CHECK(refused("'sounds/long.wav', {pitch = true}") == "the option 'pitch' is a number, not a boolean");
        CHECK(refused("'sounds/long.wav', {minDistance = {}}") == "the option 'minDistance' is a number, not a table");
        CHECK(refused("'sounds/long.wav', {maxDistance = 'far'}") ==
              "the option 'maxDistance' is a number, not a string");
        CHECK(refused("'sounds/long.wav', {loop = 1}") == "the option 'loop' is true or false, not a number");
        CHECK(refused("'sounds/long.wav', {bus = 3}") == "the option 'bus' is a name, not a number");
        CHECK(refused("'sounds/long.wav', {bus = 'noise'}") ==
              "the option 'bus' is one of effects, music, ambience, voice, interface, not 'noise'");
        CHECK(refused("'sounds/long.wav', {bus = 'master'}") ==
              "the option 'bus' is one of effects, music, ambience, voice, interface, not 'master'");
        CHECK(refused("'sounds/long.wav', {rolloff = 'cubic'}") == "the option 'rolloff' is \"inverse\" or \"linear\"");
        CHECK(refused("'sounds/long.wav', {rolloff = 1}") == "the option 'rolloff' is a name, not a number");
        CHECK(refused("'sounds/long.wav', {position = 5}") ==
              "the option 'position' is a vector: a table with x, y and z");
        CHECK(refused("'sounds/long.wav', {position = {1, 2}}") ==
              "the option 'position' is a vector: a table with x, y and z");
        CHECK(refused("'sounds/long.wav', {position = {x = 1, y = 'up', z = 0}}") ==
              "the option 'position' is a vector: a table with x, y and z");

        // Options that are more or less than a sound can have: the audio system says so.
        CHECK(refused("'sounds/long.wav', {volume = 9}") == "the volume of a sound is from 0 to 4, not 9");
        CHECK(refused("'sounds/long.wav', {volume = -0.5}") == "the volume of a sound is from 0 to 4, not -0.5");
        CHECK(refused("'sounds/long.wav', {pitch = 0}") == "the pitch of a sound is from 0.25 to 4, not 0");
        // (How what is no number is written differs from machine to machine.)
        CHECK(refused("'sounds/long.wav', {pitch = 0/0}").starts_with("the pitch of a sound is from 0.25 to 4, not "));
        CHECK(refused("'sounds/long.wav', {minDistance = 0}") ==
              "the distances of a sound are from 0.01 to 10000 m, not 0");
        CHECK(refused("'sounds/long.wav', {maxDistance = 1e9}") ==
              "the distances of a sound are from 0.01 to 10000 m, not 1e+09");
        CHECK(refused("'sounds/long.wav', {position = {0, 1/0, 0}}") == "the position of a sound is three numbers");

        // Nothing of all that is playing.
        stage.update(0.01);
        CHECK(stage.heard.voices == 0);
        CHECK(studio.silent());
    }

    TEST_CASE("when as many sounds as can be are playing, a script is given nothing, not an error") {
        Studio studio({.maxVoices = 2});
        ScriptStage& stage = studio.stage;
        CHECK(stage.line("print(cordite.audio.play('sounds/long.wav'), cordite.audio.play('sounds/long.wav'),\n"
                         "      cordite.audio.play('sounds/long.wav'), cordite.audio.play('sounds/long.wav'))") ==
              "1\t2\tnil\tnil");
        CHECK(stage.heard.voices == 2);
        // Nothing is what can be stopped and asked about without harm.
        CHECK(stage.line("local none = cordite.audio.play('sounds/long.wav')\n"
                         "cordite.audio.stop(none)\n"
                         "print(none, cordite.audio.isPlaying(none))") == "nil\tfalse");
        // When one is over, there is a voice again - with a number that was never used.
        CHECK(stage.line("cordite.audio.stop(1)\nprint(cordite.audio.play('sounds/long.wav'))") == "3");
    }

    TEST_CASE("a number for a sound is a whole number, or nothing") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        CHECK(stage.refusal("cordite.audio.stop('one')") == "bad argument #1 to 'stop' (number expected, got string)");
        CHECK(stage.refusal("cordite.audio.stop({})") == "bad argument #1 to 'stop' (number expected, got table)");
        CHECK(stage.refusal("cordite.audio.isPlaying(1.5)") ==
              "bad argument #1 to 'isPlaying' (number has no integer representation)");
        CHECK(stage.refusal("cordite.audio.isPlaying(true)") ==
              "bad argument #1 to 'isPlaying' (number expected, got boolean)");
        // A number written with a point is whole all the same.
        CHECK(stage.line("print(cordite.audio.isPlaying(1.0))") == "false");
    }

    TEST_CASE("a script asks how loud a bus is, and says how loud it is to be") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        CHECK(stage.line("local loud = {}\n"
                         "for _, bus in ipairs({'effects', 'music', 'ambience', 'voice', 'interface', 'master'}) do\n"
                         "    loud[#loud + 1] = cordite.audio.volume(bus)\n"
                         "end\n"
                         "print(table.concat(loud, ' '))") == "1.0 1.0 1.0 1.0 1.0 1.0");

        CHECK(stage
                  .lines("cordite.audio.setVolume('music', 0.25)\n"
                         "cordite.audio.setVolume('master', 0.5)\n"
                         "cordite.audio.setVolume('Interface', 4)\n"
                         "cordite.audio.setVolume('voice', 0)")
                  .empty());
        CHECK(stage.audio->volume(AudioBus::Music) == 0.25f);
        CHECK(stage.audio->masterVolume() == 0.5f);
        CHECK(stage.audio->volume(AudioBus::Interface) == 4.0f);
        CHECK(stage.audio->volume(AudioBus::Voice) == 0.0f);
        CHECK(stage.audio->volume(AudioBus::Effects) == 1.0f);
        // Names are found whatever their case, as the names of assets types and buses are
        // everywhere; and what comes back is the number a script wrote, not a float.
        CHECK(stage.line("print(cordite.audio.volume('music'), cordite.audio.volume('MASTER'),\n"
                         "      cordite.audio.volume('Effects'), cordite.audio.volume('interface'))") ==
              "0.25\t0.5\t1.0\t4.0");
        CHECK(stage.line("cordite.audio.setVolume('ambience', 0.1)\nprint(cordite.audio.volume('ambience'))") == "0.1");

        // It is heard: a quarter, on a bus at a quarter, with everything at a half.
        static_cast<void>(stage.lines("cordite.audio.play('sounds/long.wav', {bus = 'music'})"));
        CHECK(studio.level() == Vec2(0.03125f));

        // A bus that there is not, and a volume that is none: nothing changes.
        CHECK(stage.refusal("cordite.audio.volume('noise')") ==
              "bad argument #1 to 'volume' (there is no bus called 'noise'; there are: effects, music, ambience, "
              "voice, interface, and master for everything)");
        CHECK(stage.refusal("cordite.audio.setVolume('sound', 1)") ==
              "bad argument #1 to 'setVolume' (there is no bus called 'sound'; there are: effects, music, ambience, "
              "voice, interface, and master for everything)");
        CHECK(stage.refusal("cordite.audio.volume()") == "bad argument #1 to 'volume' (string expected, got no value)");
        CHECK(stage.refusal("cordite.audio.setVolume('music')") ==
              "bad argument #2 to 'setVolume' (number expected, got no value)");
        CHECK(stage.refusal("cordite.audio.setVolume('music', 9)") ==
              "bad argument #2 to 'setVolume' (the volume of a bus is from 0 to 4, not 9)");
        CHECK(stage.refusal("cordite.audio.setVolume('master', -1)") ==
              "bad argument #2 to 'setVolume' (the volume of everything is from 0 to 4, not -1)");
        CHECK(stage.refusal("cordite.audio.setVolume('music', 0/0)")
                  .starts_with("bad argument #2 to 'setVolume' (the volume of a bus is from 0 to 4, not "));
        CHECK(stage.audio->volume(AudioBus::Music) == 0.25f);
        CHECK(stage.audio->masterVolume() == 0.5f);
    }

    TEST_CASE("what an entity sounds like is its AudioSource, which a script plays by setting it") {
        Studio studio;
        ScriptStage& stage = studio.stage;
        // A component like the others: given to an entity with what it is to be.
        CHECK(stage
                  .lines("local speaker = cordite.world.create('Speaker')\n"
                         "speaker:add('AudioSource', {sound = '" +
                         studio.shortSound.toString() + "', playing = false, spatial = false, bus = 'voice'})")
                  .empty());
        CHECK(stage.heard.sources == 0);
        CHECK(studio.silent());

        // Played by saying that it is playing; and it says when it is over.
        stage.add("Operator", stage.write("scripts/operator.lua", R"(
            local Operator = {}
            function Operator:start()
                self.speaker = cordite.world.find("Speaker")
                self.speaker:set("AudioSource", { playing = true })
            end
            function Operator:update() print(self.speaker:get("AudioSource").playing) end
            return Operator
        )"));
        stage.update(0.05);
        CHECK(stage.heard.sources == 1);
        CHECK(studio.level() == Vec2(0.5f));
        stage.update(0.05);
        stage.update(0.05);
        stage.update(0.05);
        CHECK(stage.said() == Lines{"true", "true", "true", "false"});
        CHECK(stage.heard.sources == 0);
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a world in which nothing is heard says so") {
        ScriptStage stage;
        const std::string none = "nothing is heard here: this world has no sound to play";
        for (const char* asked :
             {"cordite.audio.play('sounds/long.wav')", "cordite.audio.stop(1)", "cordite.audio.isPlaying(1)",
              "cordite.audio.volume('music')", "cordite.audio.setVolume('music', 1)"}) {
            CAPTURE(asked);
            CHECK(stage.refusal(asked) == none);
        }
    }

    TEST_CASE("the functions for sound are said in words") {
        const ScriptStage stage;
        Lines names;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            if (function.name.starts_with("cordite.audio.")) {
                names.push_back(function.name);
                CHECK(function.signature.starts_with(function.name + "("));
                CHECK_FALSE(function.description.empty());
            }
        }
        CHECK(names == Lines{"cordite.audio.play", "cordite.audio.stop", "cordite.audio.isPlaying",
                             "cordite.audio.volume", "cordite.audio.setVolume"});
    }
}
