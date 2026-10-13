#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/StringUtils.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {
namespace {

// What an entity sounds like is its AudioSource - a component, which a script reaches like
// any other and plays by setting `playing`. What is here is the rest: a sound that
// belongs to no entity, played once, and how loud the buses are.
//
// Scripts run before the sounds of a frame are brought up to date: what a script plays
// is heard in the frame it played it in.

/// What `cordite.audio.volume` and `setVolume` call the volume of everything.
constexpr std::string_view kMaster = "master";

/// The options of a sound, for a message.
constexpr const char* kOptionNames = "bus, volume, pitch, loop, position, rolloff, minDistance, maxDistance";

/// The sounds of the update that is under way. Raises where nothing is heard.
AudioSystem& checkAudio(lua_State* state) {
    AudioSystem* audio = checkContext(state).audio;
    if (audio == nullptr) {
        luaL_error(state, "nothing is heard here: this world has no sound to play");
    }
    return *audio;
}

std::string_view checkText(lua_State* state, int index) {
    size_t length = 0;
    const char* text = luaL_checklstring(state, index, &length);
    return {text, length};
}

/// The names of the buses, for a message: "effects, music, ambience, voice, interface".
std::string busNames() {
    std::string names;
    for (const AudioBus bus : kAudioBuses) {
        names += names.empty() ? "" : ", ";
        names += nameOf(bus);
    }
    return names;
}

/// The sound asset named at `index`: by where its file is among the assets, or by its
/// id, as cordite.assets.find() gives it. Raises for what is no asset, and no sound.
Uuid checkSound(lua_State* state, int index) {
    const std::string reference(checkText(state, index));
    const AssetDatabase& database = checkContext(state).assets.database();
    const AssetInfo* asset = database.findByReference(reference);
    if (const std::optional<Uuid> id = Uuid::parse(reference); asset == nullptr && id) {
        asset = database.find(*id);
    }
    if (asset == nullptr) {
        luaL_argerror(state, index, lua_pushfstring(state, "there is no asset '%s'", reference.c_str()));
        return {};
    }
    if (asset->type != AssetType::Sound) {
        const std::string kind(nameOf(asset->type));
        luaL_argerror(state, index,
                      lua_pushfstring(state, "'%s' is of the kind %s, not a sound", reference.c_str(), kind.c_str()));
    }
    return asset->id;
}

/// The value at the top of the stack as the number of the option `name`. Raises for
/// anything else. Whether it is a number a sound can have is for AudioSystem to say.
float optionNumber(lua_State* state, const char* name) {
    if (lua_type(state, -1) != LUA_TNUMBER) {
        luaL_error(state, "the option '%s' is a number, not a %s", name, luaL_typename(state, -1));
    }
    return static_cast<float>(lua_tonumber(state, -1));
}

/// The value at the top of the stack as the text of the option `name`.
std::string_view optionText(lua_State* state, const char* name) {
    if (lua_type(state, -1) != LUA_TSTRING) {
        luaL_error(state, "the option '%s' is a name, not a %s", name, luaL_typename(state, -1));
    }
    size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    return {text, length};
}

/// Reads the option called `name`, whose value is at the top of the stack, into `desc`.
/// Raises for a name that is no option, and for a value of the wrong kind.
void readOption(lua_State* state, std::string_view name, AudioPlayDesc& desc) {
    if (name == "bus") {
        const std::string bus(optionText(state, "bus"));
        const std::optional<AudioBus> found = audioBusFromName(bus);
        if (!found) {
            const std::string known = busNames();
            luaL_error(state, "the option 'bus' is one of %s, not '%s'", known.c_str(), bus.c_str());
        }
        desc.bus = found.value_or(AudioBus::Effects);
    } else if (name == "volume") {
        desc.volume = optionNumber(state, "volume");
    } else if (name == "pitch") {
        desc.pitch = optionNumber(state, "pitch");
    } else if (name == "loop") {
        if (!lua_isboolean(state, -1)) {
            luaL_error(state, "the option 'loop' is true or false, not a %s", luaL_typename(state, -1));
        }
        desc.loop = lua_toboolean(state, -1) != 0;
    } else if (name == "position") {
        std::array<double, 3> numbers{};
        if (!lua_istable(state, -1) || !readNumbers(state, lua_gettop(state), 3, numbers.data())) {
            luaL_error(state, "the option 'position' is a vector: a table with x, y and z");
        }
        desc.position =
            Vec3(static_cast<float>(numbers[0]), static_cast<float>(numbers[1]), static_cast<float>(numbers[2]));
    } else if (name == "rolloff") {
        const std::string_view rolloff = optionText(state, "rolloff");
        if (rolloff != nameOf(AudioRolloff::Inverse) && rolloff != nameOf(AudioRolloff::Linear)) {
            luaL_error(state, R"(the option 'rolloff' is "inverse" or "linear")");
        }
        desc.rolloff = rolloff == nameOf(AudioRolloff::Linear) ? AudioRolloff::Linear : AudioRolloff::Inverse;
    } else if (name == "minDistance") {
        desc.minDistance = optionNumber(state, "minDistance");
    } else if (name == "maxDistance") {
        desc.maxDistance = optionNumber(state, "maxDistance");
    } else {
        const std::string unknown(name);
        luaL_error(state, "a sound has no option '%s'; it has: %s", unknown.c_str(), kOptionNames);
    }
}

/// The options of a sound at `index` - a table of names, or nothing - put into `desc`.
/// A name that is no option is a mistake, and said: an option that is spelled wrong
/// must not be one that is silently left as it was.
void readOptions(lua_State* state, int index, AudioPlayDesc& desc) {
    if (lua_isnoneornil(state, index)) {
        return;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        // The key is looked at before it is read: reading a number as text would make
        // it text where it lies, and that is no key lua_next can go on from.
        if (lua_type(state, -2) != LUA_TSTRING) {
            luaL_error(state, "the options of a sound have names: %s", kOptionNames);
        }
        size_t length = 0;
        const char* name = lua_tolstring(state, -2, &length);
        readOption(state, std::string_view(name, length), desc);
        lua_pop(state, 1);
    }
}

/// What play() gave at `index`, to stop a sound or to ask about it by: a whole number,
/// or nothing - which is what play() gives when no voice is free, and names no sound.
AudioVoiceId checkVoice(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return 0;
    }
    // A number below nothing becomes one far above every number that was ever handed
    // out, and names no sound either.
    return static_cast<AudioVoiceId>(luaL_checkinteger(state, index));
}

/// The bus named at `index`, or nothing for "master". Raises for any other name.
std::optional<AudioBus> checkBusOrMaster(lua_State* state, int index) {
    const std::string name(checkText(state, index));
    if (equalsIgnoreCase(name, kMaster)) {
        return std::nullopt;
    }
    const std::optional<AudioBus> bus = audioBusFromName(name);
    if (!bus) {
        const std::string known = busNames();
        luaL_argerror(state, index,
                      lua_pushfstring(state, "there is no bus called '%s'; there are: %s, and master for everything",
                                      name.c_str(), known.c_str()));
    }
    return bus;
}

int audioPlay(lua_State* state) {
    AudioSystem& audio = checkAudio(state);
    AudioPlayDesc desc;
    desc.sound = checkSound(state, 1);
    readOptions(state, 2, desc);
    const Result<AudioVoiceId> voice = audio.play(checkContext(state).assets, desc);
    if (voice) {
        lua_pushinteger(state, static_cast<lua_Integer>(*voice));
        return 1;
    }
    // As much as can be is playing: that is no mistake of the script, and nothing to
    // stop it for.
    if (voice.error().code == ErrorCode::OutOfRange) {
        lua_pushnil(state);
        return 1;
    }
    return luaL_error(state, "%s", voice.error().message.c_str());
}

int audioStop(lua_State* state) {
    checkAudio(state).stop(checkVoice(state, 1));
    return 0;
}

int audioIsPlaying(lua_State* state) {
    lua_pushboolean(state, checkAudio(state).isPlaying(checkVoice(state, 1)) ? 1 : 0);
    return 1;
}

int audioVolume(lua_State* state) {
    const AudioSystem& audio = checkAudio(state);
    const std::optional<AudioBus> bus = checkBusOrMaster(state, 1);
    lua_pushnumber(state, scriptNumber(bus ? audio.volume(*bus) : audio.masterVolume()));
    return 1;
}

int audioSetVolume(lua_State* state) {
    AudioSystem& audio = checkAudio(state);
    const std::optional<AudioBus> bus = checkBusOrMaster(state, 1);
    const auto volume = static_cast<float>(luaL_checknumber(state, 2));
    const Status set = bus ? audio.setVolume(*bus, volume) : audio.setMasterVolume(volume);
    if (!set) {
        return luaL_argerror(state, 2, lua_pushfstring(state, "%s", set.error().message.c_str()));
    }
    return 0;
}

constexpr std::array kAudioFunctions = {
    ScriptFunction{"play", "cordite.audio.play(sound, options) -> voice or nil",
                   "Plays a sound once, without an entity: `sound` is where its file is among the assets - "
                   "\"sounds/shot.wav\" - or its id. `options` is a table of any of `bus` (\"effects\" when left "
                   "out), `volume`, `pitch`, `loop`, `position` - a vector: heard from there, quieter with "
                   "distance; without it the same in both ears - and for that `rolloff` (\"inverse\" or "
                   "\"linear\"), `minDistance` and `maxDistance`, as an AudioSource has them. Returns a number "
                   "to stop it by, or nil when as many sounds as can be are playing. What an entity sounds like "
                   "is its AudioSource: set its `playing`.",
                   audioPlay},
    ScriptFunction{"stop", "cordite.audio.stop(voice)",
                   "Stops what play() started. Nothing happens for a sound that is over, and for nil.", audioStop},
    ScriptFunction{"isPlaying", "cordite.audio.isPlaying(voice) -> boolean",
                   "Whether what play() started is still to be heard: not stopped, and not at its end.",
                   audioIsPlaying},
    ScriptFunction{"volume", "cordite.audio.volume(bus) -> number",
                   "How loud everything on a bus is - \"effects\", \"music\", \"ambience\", \"voice\" or "
                   "\"interface\" - or, for \"master\", everything there is: 1 as it is, 0 not at all.",
                   audioVolume},
    ScriptFunction{"setVolume", "cordite.audio.setVolume(bus, volume)",
                   "Makes everything on a bus, or with \"master\" everything, that loud, from 0 to 4: what the "
                   "sliders of a settings menu do. It stays so when another scene is loaded.",
                   audioSetVolume},
};

} // namespace

Status registerAudioApi(ScriptRuntime& runtime) {
    return runtime.registerLibrary("cordite.audio", kAudioFunctions);
}

} // namespace cordite
