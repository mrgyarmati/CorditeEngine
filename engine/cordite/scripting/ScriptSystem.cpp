#include "cordite/scripting/ScriptSystem.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/platform/InputMap.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptValue.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cordite {
namespace {

/// The functions the engine calls in a script, in the order a frame calls them.
enum class Callback : uint8_t {
    Start,
    Input,
    Update,
    CollisionBegan,
    CollisionEnded,
    TriggerEntered,
    TriggerLeft,
    Destroy,
    Count,
};

constexpr size_t kCallbackCount = static_cast<size_t>(Callback::Count);

/// What a script calls one of them, how it is written, and what it is for: this is what
/// whoever writes a script is told (ScriptSystem::callbacks()).
struct CallbackWords {
    const char* name = "";
    const char* signature = "";
    const char* description = "";
};

constexpr std::array<CallbackWords, kCallbackCount> kCallbacks = {{
    {"start", "function Script:start()",
     "Called once, before the first update: for parents before what hangs under them. What it sets up - "
     "`self.count = 0` - is the script's to keep in `self`."},
    {"input", "function Script:input(action, pressed)",
     "Called for every action of the input map that was pressed or released in the frame, with its name and "
     "true for pressed: after the starts of the frame, before its timers and updates."},
    {"update", "function Script:update(deltaSeconds)",
     "Called every frame, with the seconds that have passed since the frame before."},
    {"collisionBegan", "function Script:collisionBegan(other, contact)",
     "Called when the body of the entity has begun to touch that of another, right after the step of the "
     "simulation in which it did. `contact` holds `position`, where the two touch in the world; `normal`, "
     "which way the other lies from there; and `speed`, how fast they were closing in, in metres per second."},
    {"collisionEnded", "function Script:collisionEnded(other)",
     "Called when the two bodies have parted, or one of them is gone: `other` may no longer be valid."},
    {"triggerEntered", "function Script:triggerEntered(other, contact)",
     "Called when a body has moved into a trigger: for the entity that has the trigger, and for the one that "
     "moved into it."},
    {"triggerLeft", "function Script:triggerLeft(other)",
     "Called for both when the body has left the trigger again, or one of the two is gone."},
    {"destroy", "function Script:destroy()",
     "Called when the script ends while its entity is still there: its Script was taken off or changed, a "
     "script destroyed the entity, or another scene is loaded."},
}};

/// A script that was loaded - or that could not be.
struct LoadedScript {
    Uuid id;
    /// Its file, as messages name it: "scripts/door.lua". The id, while no file is known.
    std::string path;
    /// The text it was last made of: what a refresh of the assets compares with.
    std::string text;
    /// What the script returned, and the metatable that leads its instances to it: two
    /// references into Lua's registry. None while the script could not be loaded.
    int table = LUA_NOREF;
    int metatable = LUA_NOREF;
    /// The properties it declares, as data.
    Json defaults = Json::object();
    /// Which of the functions the engine calls it has.
    std::array<bool, kCallbackCount> has{};
    /// Why it cannot be loaded - or why its newest text cannot, while an older runs.
    std::string problem;

    [[nodiscard]] bool isLoaded() const { return table != LUA_NOREF; }

    [[nodiscard]] bool hears(Callback callback) const { return has[static_cast<size_t>(callback)]; }
};

enum class Phase : uint8_t {
    /// Made, and not started yet.
    Created,
    Running,
    /// An error stopped it, or it could not be made. Nothing of it is called.
    Stopped,
};

/// The script of one entity.
struct Instance {
    /// What its component said when it was made. A component that says something else
    /// asks for another instance.
    ScriptComponent blueprint;
    /// Its table, in Lua's registry. None when it could not be made.
    int self = LUA_NOREF;
    Phase phase = Phase::Created;
    /// Whether `start` has been through, to its end.
    bool started = false;
    /// What stopped it.
    std::string problem;
};

/// How two bodies met, as the script of one of them is told it.
struct Contact {
    /// Where they touch, in the world.
    Vec3 position{0.0f};
    /// Which way the other lies from the one that is told, there.
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// How fast they were closing in on each other, in metres per second.
    float speed = 0.0f;
};

/// Something a script asked to have done later: a function that is called when its time
/// has come, or a task - a coroutine - that goes on then.
struct Timer {
    /// The number scripts know it by. None once it is cancelled, called for the last
    /// time or at its end: it is then only waiting to be taken off the list.
    uint64_t id = 0;
    /// The entity whose script made it. It is called for the instance of that entity,
    /// and goes when that instance does.
    Entity owner;
    /// The function to call, or the coroutine of a task: references into Lua's registry.
    int function = LUA_NOREF;
    int thread = LUA_NOREF;
    /// When it is due, by the clock of the scripts.
    double due = 0.0;
    /// How long after that it is due again; none for what is called once.
    double interval = 0.0;
};

/// The most timers and tasks the script of one entity may have waiting: far more than a
/// script has use for, and few enough that one which makes a timer in every frame and
/// never lets one go is told so within seconds.
constexpr size_t kMaxTimersPerInstance = 1024;

/// What a function of a script is called with besides its instance: numbers, yes or no,
/// text that outlives the call, entities - also ones that are gone - and contacts.
using Argument = std::variant<double, bool, std::string_view, Entity, Contact>;
using Arguments = std::span<const Argument>;

/// Pushes what a script is called with. Raises as pushing a value does.
void pushArgument(lua_State* state, const Argument& argument) {
    if (const double* number = std::get_if<double>(&argument); number != nullptr) {
        lua_pushnumber(state, static_cast<lua_Number>(*number));
    } else if (const bool* flag = std::get_if<bool>(&argument); flag != nullptr) {
        lua_pushboolean(state, *flag ? 1 : 0);
    } else if (const std::string_view* text = std::get_if<std::string_view>(&argument); text != nullptr) {
        lua_pushlstring(state, text->data(), text->size());
    } else if (const Entity* entity = std::get_if<Entity>(&argument); entity != nullptr) {
        pushEntity(state, *entity);
    } else {
        const auto& contact = std::get<Contact>(argument);
        lua_createtable(state, 0, 3);
        pushVec3(state, contact.position);
        lua_setfield(state, -2, "position");
        pushVec3(state, contact.normal);
        lua_setfield(state, -2, "normal");
        lua_pushnumber(state, static_cast<lua_Number>(scriptNumber(contact.speed)));
        lua_setfield(state, -2, "speed");
    }
}

/// What kind of value JSON holds, as far as a property of a script cares.
enum class Kind : uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

Kind kindOf(const Json& value) {
    if (value.is_boolean()) {
        return Kind::Boolean;
    }
    if (value.is_number()) {
        return Kind::Number;
    }
    if (value.is_string()) {
        return Kind::String;
    }
    if (value.is_array()) {
        return Kind::Array;
    }
    return value.is_object() ? Kind::Object : Kind::Null;
}

std::string_view withArticle(Kind kind) {
    switch (kind) {
        case Kind::Null:
            return "nothing";
        case Kind::Boolean:
            return "true or false";
        case Kind::Number:
            return "a number";
        case Kind::String:
            return "text";
        case Kind::Array:
            return "a list";
        case Kind::Object:
            return "a table of names";
    }
    return "nothing";
}

/// Whether a value that a component gives fits what the script declares. A table
/// without anything in it is neither a list nor a table of names, and takes either.
bool fits(const Json& declared, const Json& given) {
    const Kind expected = kindOf(declared);
    const Kind actual = kindOf(given);
    if (expected == Kind::Null || expected == actual) {
        return true;
    }
    return expected == Kind::Object && declared.empty() && actual == Kind::Array;
}

/// Whether the table on top of the stack has a function called `name`. Raises a Lua
/// error for something else of that name: a script that says `update = 5` has made a
/// mistake that would otherwise only show as nothing happening.
bool hasFunction(lua_State* state, const char* name) {
    const int type = lua_getfield(state, -1, name);
    lua_pop(state, 1);
    if (type != LUA_TNIL && type != LUA_TFUNCTION) {
        luaL_error(state, "its '%s' is a %s, not a function", name, lua_typename(state, type));
    }
    return type == LUA_TFUNCTION;
}

} // namespace

struct ScriptSystem::Impl {
    std::unique_ptr<ScriptRuntime> runtime;
    /// The update that is under way.
    const ScriptContext* context = nullptr;

    std::unordered_map<Uuid, LoadedScript> scripts;
    /// The same, in the order they were first asked for: what is done to all of them is
    /// done in that order.
    std::vector<Uuid> scriptOrder;
    std::unordered_map<Entity, Instance> instances;
    /// The entities that have instances, in the order those were made.
    std::vector<Entity> order;
    /// Entities that scripts have asked to have destroyed.
    std::vector<Entity> doomed;
    std::vector<ScriptProblem> problems;

    /// AssetDatabase::revision() when the scripts were last compared with their files.
    uint64_t assetRevision = 0;
    bool knowsRevision = false;
    /// Functions of scripts called since the last update began, and how often each of
    /// those the engine calls has been called since the system was made.
    uint32_t calls = 0;
    std::array<uint64_t, kCallbackCount> callbackCalls{};

    /// What scripts asked to have done later, in the order they asked.
    std::vector<Timer> timers;
    uint64_t nextTimer = 1;
    /// The entity whose script is being called: who a timer that is made now belongs
    /// to. Null while no script of an entity runs.
    Entity calling;
    /// The coroutine of the task that is being resumed, which is where wait() may be
    /// called. Null while none is.
    lua_State* task = nullptr;
    /// The scene a script asked for, until whoever owns the world takes the request.
    std::optional<Uuid> requestedScene;
    /// The time the updates have let pass, how many there were, and the time of the last.
    double seconds = 0.0;
    uint64_t updates = 0;
    double deltaSeconds = 0.0;

    /// Kept between updates for what they have allocated.
    std::vector<Entity> wanted;
    std::vector<bool> enabledAtDepth;

    /// How a load of a script went.
    enum class Loaded : uint8_t {
        /// Its text is what it was; nothing was done.
        Unchanged,
        /// It runs with the text its file has now.
        Fresh,
        /// It could not be loaded; what ran before, if anything, still does.
        Failed,
    };

    /// Says for as long as it lives whose script is being called.
    class Calling {
    public:
        Calling(Impl& impl, Entity entity) : m_impl(impl), m_previous(std::exchange(impl.calling, entity)) {}

        ~Calling() { m_impl.calling = m_previous; }

        CORDITE_NON_MOVABLE(Calling);

    private:
        Impl& m_impl;
        Entity m_previous;
    };

    /// Says for as long as it lives which world the scripts reach.
    class Reach {
    public:
        Reach(Impl& impl, const ScriptContext& context) : m_impl(impl) { impl.context = &context; }

        ~Reach() { m_impl.context = nullptr; }

        CORDITE_NON_MOVABLE(Reach);

    private:
        Impl& m_impl;
    };

    [[nodiscard]] LoadedScript& scriptOf(const Uuid& id);
    Loaded load(LoadedScript& script) const;
    void followAssets();
    void release(LoadedScript& script) const;

    /// Brings the instances in line with what the Script components of the world ask
    /// for, and lists in `wanted` who is to be called, parents first.
    void reconcile(const World& world);
    /// The instance of an entity that may be called now: one that is there and has not
    /// stopped, of an entity that is there and switched on, with all above it.
    [[nodiscard]] Instance* running(const World& world, Entity entity);
    void startAll(const World& world);
    void tellInput(const World& world, std::span<const ActionEvent> events);
    void updateAll(const World& world);
    /// Calls the timers that are due, and resumes the tasks, of instances that run: in
    /// the order they were made, each once at most.
    void fireTimers(const World& world);
    /// Resumes the task `id` with `arguments` values that are on top of its coroutine's
    /// stack, inside a protected call. Raises what the task raised.
    void resumeTask(lua_State* state, uint64_t id, int arguments);
    [[nodiscard]] Timer* findTimer(uint64_t id);
    /// Marks every timer and task of an entity as over; sweepTimers() lets them go.
    void forgetTimers(Entity owner);
    /// Lets go of what is over. Not while a script runs: a task may be among them.
    void sweepTimers();
    /// Tells the script of `entity` that its body and that of `other` began to touch,
    /// or stopped: `mirrored` for the second of the two, to whom the first lies the
    /// other way.
    void tellContact(const World& world, Entity entity, Entity other, const CollisionEvent& event, bool mirrored);

    void make(Entity entity, const ScriptComponent& component);
    void stop(Entity entity, Instance& instance, std::string_view problem) const;
    void call(Entity entity, Instance& instance, Callback callback, Arguments arguments = {});
    void end(Entity entity);
    void release(Instance& instance) const;
    void destroyDoomed();
    void listProblems();
    [[nodiscard]] ScriptStatistics statistics() const;

    /// What to call an entity in a message: its name, and its script.
    [[nodiscard]] std::string describe(Entity entity, const Instance& instance) const;
};

LoadedScript& ScriptSystem::Impl::scriptOf(const Uuid& id) {
    const auto [found, isNew] = scripts.try_emplace(id);
    LoadedScript& script = found->second;
    if (isNew) {
        script.id = id;
        script.path = id.toString();
        scriptOrder.push_back(id);
        load(script);
    }
    return script;
}

ScriptSystem::Impl::Loaded ScriptSystem::Impl::load(LoadedScript& script) const {
    const auto fail = [&](std::string problem) {
        // Said once: when it first is so, and again when it is something else.
        if (script.problem != problem) {
            CORDITE_LOG_ERROR("Script", "{}", problem);
            script.problem = std::move(problem);
        }
        return Loaded::Failed;
    };

    const Result<std::shared_ptr<const ScriptSource>> source = context->assets.loadScript(script.id);
    if (!source) {
        return fail(source.error().message);
    }
    if (const AssetInfo* asset = context->assets.database().find(script.id); asset != nullptr) {
        script.path = asset->path;
    }
    if (script.isLoaded() && script.problem.empty() && (*source)->text == script.text) {
        return Loaded::Unchanged;
    }
    // Text that failed is not tried again until it is another text.
    if (!script.problem.empty() && (*source)->text == script.text) {
        return Loaded::Failed;
    }
    script.text = (*source)->text;

    LoadedScript loaded;
    loaded.metatable = script.metatable;
    const std::string chunkName = "@" + script.path;
    const Status ran = runtime->protect([&](lua_State* state) {
        ScriptRuntime::runChunk(state, script.text, chunkName, 1);
        if (lua_isnil(state, -1)) {
            luaL_error(state, "it returns nothing; a script returns the table of its functions");
        }
        if (!lua_istable(state, -1)) {
            luaL_error(state, "it returns a %s; a script returns the table of its functions", luaL_typename(state, -1));
        }

        // What it declares as properties is taken as data: every instance gets a copy
        // of its own, and what a component gives is compared with it.
        const int declares = lua_getfield(state, -1, "properties");
        if (declares != LUA_TNIL && declares != LUA_TTABLE) {
            luaL_error(state, "its 'properties' are a %s, not a table", lua_typename(state, declares));
        }
        if (declares == LUA_TTABLE) {
            Result<Json> declared = readScriptValue(state, -1);
            if (!declared) {
                luaL_error(state, "its 'properties' cannot be kept as data: %s", declared.error().message.c_str());
            }
            if (!declared->is_object()) {
                luaL_error(state, "its 'properties' are a list; they have names");
            }
            loaded.defaults = std::move(*declared);
        }
        lua_pop(state, 1);
        for (size_t callback = 0; callback < kCallbackCount; ++callback) {
            loaded.has[callback] = hasFunction(state, kCallbacks[callback].name);
        }

        lua_pushvalue(state, -1);
        loaded.table = luaL_ref(state, LUA_REGISTRYINDEX);
        // The instances reach the script through one metatable, made the first time.
        // A script that is loaded again is put behind the same one - which, once
        // the script is held on to, nothing can keep from happening - and every
        // instance there is has the new functions.
        if (loaded.metatable == LUA_NOREF) {
            lua_newtable(state);
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, "__index");
            loaded.metatable = luaL_ref(state, LUA_REGISTRYINDEX);
        } else {
            lua_rawgeti(state, LUA_REGISTRYINDEX, loaded.metatable);
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, "__index");
        }
    });
    // What ran before is let go of, or - when this did not work - what was made of it.
    const int discarded = ran ? script.table : loaded.table;
    const int discardedMetatable = ran || loaded.metatable == script.metatable ? LUA_NOREF : loaded.metatable;
    if (const Status released = runtime->protect([&](lua_State* state) {
            luaL_unref(state, LUA_REGISTRYINDEX, discarded);
            luaL_unref(state, LUA_REGISTRYINDEX, discardedMetatable);
        });
        !released) {
        CORDITE_LOG_ERROR("Script", "{}: {}", script.path, released.error().message);
    }
    if (!ran) {
        // What Lua itself objects to names the file and the line; what is said above
        // does not.
        const std::string& message = ran.error().message;
        return fail(message.starts_with(script.path) ? message : fmt::format("{}: {}", script.path, message));
    }
    script.table = loaded.table;
    script.metatable = loaded.metatable;
    script.defaults = std::move(loaded.defaults);
    script.has = loaded.has;
    script.problem.clear();
    return Loaded::Fresh;
}

void ScriptSystem::Impl::followAssets() {
    const uint64_t revision = context->assets.database().revision();
    if (knowsRevision && revision == assetRevision) {
        return;
    }
    const bool first = !knowsRevision;
    assetRevision = revision;
    knowsRevision = true;
    if (first) {
        return;
    }

    for (const Uuid& id : scriptOrder) {
        LoadedScript& script = scripts.at(id);
        if (load(script) != Loaded::Fresh) {
            continue;
        }
        // Whatever stopped an instance of this script may have been put right. One that
        // had been made goes on where it was; one that could not be made is made anew
        // by the update, as if its entity had only now asked for it.
        for (const Entity entity : order) {
            const auto found = instances.find(entity);
            if (found == instances.end() || found->second.blueprint.script != id ||
                found->second.phase != Phase::Stopped) {
                continue;
            }
            Instance& instance = found->second;
            if (instance.self == LUA_NOREF) {
                instances.erase(found);
            } else {
                instance.phase = instance.started ? Phase::Running : Phase::Created;
                instance.problem.clear();
            }
        }
    }
}

void ScriptSystem::Impl::release(LoadedScript& script) const {
    if (const Status released = runtime->protect([&](lua_State* state) {
            luaL_unref(state, LUA_REGISTRYINDEX, script.table);
            luaL_unref(state, LUA_REGISTRYINDEX, script.metatable);
        });
        !released) {
        CORDITE_LOG_ERROR("Script", "{}: {}", script.path, released.error().message);
    }
    script.table = LUA_NOREF;
    script.metatable = LUA_NOREF;
}

std::string ScriptSystem::Impl::describe(Entity entity, const Instance& instance) const {
    const auto script = scripts.find(instance.blueprint.script);
    const std::string path = script != scripts.end() ? script->second.path : instance.blueprint.script.toString();
    return fmt::format("'{}' ({})", context->world.name(entity), path);
}

void ScriptSystem::Impl::stop(Entity entity, Instance& instance, std::string_view problem) const {
    instance.phase = Phase::Stopped;
    instance.problem = fmt::format("{}: {}", describe(entity, instance), problem);
    CORDITE_LOG_ERROR("Script", "{}", instance.problem);
}

void ScriptSystem::Impl::make(Entity entity, const ScriptComponent& component) {
    Instance& instance = instances[entity];
    order.push_back(entity);
    instance.blueprint = component;
    // From here on the copy: loading a script runs it, and what runs may move the
    // components of the world to another place.
    const ScriptComponent& blueprint = instance.blueprint;

    const LoadedScript& script = scriptOf(blueprint.script);
    if (!script.isLoaded()) {
        // Why not is said of the script, once; of the entity only that it waits.
        instance.phase = Phase::Stopped;
        instance.problem = fmt::format("{}: its script could not be loaded", describe(entity, instance));
        return;
    }

    Json properties = script.defaults;
    for (const auto& [name, value] : blueprint.properties.items()) {
        const auto declared = properties.find(name);
        if (declared == properties.end()) {
            stop(entity, instance,
                 fmt::format("its Script gives '{}' a value, which the script does not declare among its properties",
                             name));
            return;
        }
        if (!fits(*declared, value)) {
            stop(entity, instance,
                 fmt::format("its Script gives '{}' {}, where the script declares {}", name, withArticle(kindOf(value)),
                             withArticle(kindOf(*declared))));
            return;
        }
        *declared = value;
    }

    int self = LUA_NOREF;
    const Status made = runtime->protect([&](lua_State* state) {
        lua_createtable(state, 0, 4);
        pushEntity(state, entity);
        lua_setfield(state, -2, "entity");
        pushScriptValue(state, properties);
        lua_setfield(state, -2, "properties");
        lua_rawgeti(state, LUA_REGISTRYINDEX, script.metatable);
        lua_setmetatable(state, -2);
        self = luaL_ref(state, LUA_REGISTRYINDEX);
    });
    if (!made) {
        stop(entity, instance, made.error().message);
        return;
    }
    instance.self = self;
}

void ScriptSystem::Impl::call(Entity entity, Instance& instance, Callback callback, Arguments arguments) {
    ++calls;
    ++callbackCalls[static_cast<size_t>(callback)];
    const Calling by(*this, entity);
    const Status called = runtime->protect([&](lua_State* state) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, instance.self);
        // Found through the instance, so that one which has a function of its own for
        // this is called with that.
        if (lua_getfield(state, -1, kCallbacks[static_cast<size_t>(callback)].name) != LUA_TFUNCTION) {
            return;
        }
        lua_insert(state, -2);
        // An argument takes a place, and a few more while it is put together.
        luaL_checkstack(state, static_cast<int>(arguments.size()) + 4, "the scripts are out of stack");
        for (const Argument& argument : arguments) {
            pushArgument(state, argument);
        }
        lua_call(state, 1 + static_cast<int>(arguments.size()), 0);
    });
    if (!called) {
        stop(entity, instance, called.error().message);
    }
}

void ScriptSystem::Impl::release(Instance& instance) const {
    if (instance.self == LUA_NOREF) {
        return;
    }
    if (const Status released =
            runtime->protect([&](lua_State* state) { luaL_unref(state, LUA_REGISTRYINDEX, instance.self); });
        !released) {
        CORDITE_LOG_ERROR("Script", "{}", released.error().message);
    }
    instance.self = LUA_NOREF;
}

void ScriptSystem::Impl::end(Entity entity) {
    const auto found = instances.find(entity);
    if (found == instances.end()) {
        return;
    }
    Instance& instance = found->second;
    // Only what was started is told that it ends, and only while there is an entity
    // to end for.
    const auto script = scripts.find(instance.blueprint.script);
    if (instance.phase == Phase::Running && instance.started && script != scripts.end() &&
        script->second.hears(Callback::Destroy) && context->world.isValid(entity)) {
        call(entity, instance, Callback::Destroy);
    }
    // What it asked to have done later is not done: another instance of the same
    // entity is not the one that asked.
    forgetTimers(entity);
    release(instance);
    instances.erase(found);
}

Timer* ScriptSystem::Impl::findTimer(uint64_t id) {
    const auto found = std::ranges::find(timers, id, &Timer::id);
    return id != 0 && found != timers.end() ? &*found : nullptr;
}

void ScriptSystem::Impl::forgetTimers(Entity owner) {
    for (Timer& timer : timers) {
        if (timer.owner == owner) {
            timer.id = 0;
        }
    }
}

void ScriptSystem::Impl::sweepTimers() {
    if (std::ranges::none_of(timers, [](const Timer& timer) { return timer.id == 0; })) {
        return;
    }
    if (const Status released = runtime->protect([&](lua_State* state) {
            for (const Timer& timer : timers) {
                if (timer.id == 0) {
                    luaL_unref(state, LUA_REGISTRYINDEX, timer.function);
                    luaL_unref(state, LUA_REGISTRYINDEX, timer.thread);
                }
            }
        });
        !released) {
        CORDITE_LOG_ERROR("Script", "{}", released.error().message);
    }
    std::erase_if(timers, [](const Timer& timer) { return timer.id == 0; });
}

void ScriptSystem::Impl::resumeTask(lua_State* state, uint64_t id, int arguments) {
    const Timer* found = findTimer(id);
    if (found == nullptr) {
        return;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, found->thread);
    lua_State* thread = lua_tothread(state, -1);
    // The registry holds on to it: until the sweep, which is not while it runs.
    lua_pop(state, 1);

    int results = 0;
    int status = LUA_OK;
    {
        lua_State* const previous = std::exchange(task, thread);
        status = lua_resume(thread, state, arguments, &results);
        task = previous;
    }
    // The task may have made timers, and the list may be elsewhere now; it may also
    // have cancelled itself.
    Timer* timer = findTimer(id);
    if (status == LUA_YIELD) {
        // What it waits for is what it yielded: so many seconds, or - for nothing, and
        // for a yield of the script's own - the next update.
        double waits = 0.0;
        if (results >= 1 && lua_type(thread, -results) == LUA_TNUMBER) {
            waits = static_cast<double>(lua_tonumber(thread, -results));
        }
        lua_pop(thread, results);
        if (timer != nullptr) {
            timer->due = seconds + (std::isfinite(waits) ? std::max(waits, 0.0) : 0.0);
        }
        return;
    }
    // At its end, or stopped by an error: over either way.
    if (timer != nullptr) {
        timer->id = 0;
    }
    if (status != LUA_OK) {
        // Raised here as if the function had been called, with where the task was.
        const char* message = lua_tostring(thread, -1);
        luaL_traceback(state, thread, message != nullptr ? message : "(an error without a message)", 0);
        lua_error(state);
    }
}

void ScriptSystem::Impl::fireTimers(const World& world) {
    // Only those that were there when the walk began: what a timer makes is for a later
    // update, also when no time is to pass before it.
    const size_t count = timers.size();
    for (size_t index = 0; index < count; ++index) {
        // A copy: what is called may make timers, and the list may move.
        const Timer timer = timers[index];
        if (timer.id == 0 || timer.due > seconds) {
            continue;
        }
        // One whose script does not run waits until it does.
        Instance* instance = running(world, timer.owner);
        if (instance == nullptr || !instance->started) {
            continue;
        }
        if (timer.thread == LUA_NOREF && timer.interval > 0.0) {
            // Due again an interval after it was due - not after it was called, so that
            // it keeps time - and what a long frame has missed is not made up for.
            const double missed = std::floor((seconds - timer.due) / timer.interval);
            timers[index].due = timer.due + timer.interval * (missed + 1.0);
        } else if (timer.thread == LUA_NOREF) {
            timers[index].id = 0;
        }
        ++calls;
        const Calling by(*this, timer.owner);
        const Status called = runtime->protect([&](lua_State* state) {
            if (timer.thread != LUA_NOREF) {
                resumeTask(state, timer.id, 0);
                return;
            }
            lua_rawgeti(state, LUA_REGISTRYINDEX, timer.function);
            lua_rawgeti(state, LUA_REGISTRYINDEX, instance->self);
            lua_call(state, 1, 0);
        });
        if (!called) {
            stop(timer.owner, *instance, called.error().message);
        }
    }
}

void ScriptSystem::Impl::destroyDoomed() {
    World& world = context->world;
    // What `destroy` asks to have destroyed in its turn goes in the next round.
    while (!doomed.empty()) {
        const std::vector<Entity> batch = std::exchange(doomed, {});
        for (const Entity root : batch) {
            if (!world.isValid(root)) {
                continue;
            }
            std::vector<Entity> subtree;
            world.traverseSubtree(root, [&subtree](Entity member, uint32_t /*depth*/) { subtree.push_back(member); });
            for (const Entity member : subtree | std::views::reverse) {
                end(member);
            }
            // A script may have destroyed nothing by hand, but it may have moved things:
            // whatever hangs under the entity now goes with it.
            if (world.isValid(root)) {
                world.destroyEntity(root);
            }
        }
    }
}

void ScriptSystem::Impl::listProblems() {
    problems.clear();
    for (const Uuid& id : scriptOrder) {
        const LoadedScript& script = scripts.at(id);
        if (!script.problem.empty()) {
            problems.push_back({.script = id, .entity = Entity{}, .message = script.problem});
        }
    }
    for (const Entity entity : order) {
        const auto found = instances.find(entity);
        if (found != instances.end() && found->second.phase == Phase::Stopped) {
            problems.push_back(
                {.script = found->second.blueprint.script, .entity = entity, .message = found->second.problem});
        }
    }
}

ScriptStatistics ScriptSystem::Impl::statistics() const {
    ScriptStatistics result;
    for (const auto& [id, script] : scripts) {
        result.scripts += script.isLoaded() ? 1 : 0;
    }
    result.instances = static_cast<uint32_t>(instances.size());
    for (const auto& [entity, instance] : instances) {
        result.stopped += instance.phase == Phase::Stopped ? 1 : 0;
    }
    result.calls = calls;
    result.timers =
        static_cast<uint32_t>(std::ranges::count_if(timers, [](const Timer& timer) { return timer.id != 0; }));
    result.memoryBytes = runtime->memoryBytes();
    return result;
}

Result<std::unique_ptr<ScriptSystem>> ScriptSystem::create(const ScriptSystemDesc& desc) {
    auto impl = std::make_unique<Impl>();
    CORDITE_TRY_ASSIGN(impl->runtime, ScriptRuntime::create(desc.runtime));
    std::unique_ptr<ScriptSystem> system(new ScriptSystem(std::move(impl)));
    system->m_impl->runtime->setHost(system.get());
    ScriptRuntime& runtime = *system->m_impl->runtime;
    CORDITE_TRY(registerMathApi(runtime));
    CORDITE_TRY(registerEntityApi(runtime));
    CORDITE_TRY(registerWorldApi(runtime));
    CORDITE_TRY(registerComponentApi(runtime));
    CORDITE_TRY(registerInputApi(runtime));
    CORDITE_TRY(registerPhysicsApi(runtime));
    CORDITE_TRY(registerAudioApi(runtime));
    return system;
}

ScriptSystem::ScriptSystem(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

ScriptSystem::~ScriptSystem() = default;

ScriptSystem& ScriptSystem::of(lua_State* state) {
    return *static_cast<ScriptSystem*>(ScriptRuntime::of(state).host());
}

ScriptRuntime& ScriptSystem::runtime() {
    return *m_impl->runtime;
}

const ScriptContext* ScriptSystem::context() const {
    return m_impl->context;
}

std::span<const ScriptProblem> ScriptSystem::problems() const {
    return m_impl->problems;
}

void ScriptSystem::pushInstance(lua_State* state, Entity entity) const {
    const auto found = m_impl->instances.find(entity);
    if (found == m_impl->instances.end() || found->second.self == LUA_NOREF) {
        lua_pushnil(state);
    } else {
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second.self);
    }
}

double ScriptSystem::seconds() const {
    return m_impl->seconds;
}

uint64_t ScriptSystem::updates() const {
    return m_impl->updates;
}

double ScriptSystem::deltaSeconds() const {
    return m_impl->deltaSeconds;
}

uint64_t ScriptSystem::addTimer(lua_State* state, double seconds, bool repeats) {
    Impl& impl = *m_impl;
    if (impl.calling.isNull()) {
        luaL_error(state, "a timer belongs to the script of an entity: make it in a function the engine calls");
    }
    const auto waiting = static_cast<size_t>(std::ranges::count_if(
        impl.timers, [&](const Timer& timer) { return timer.id != 0 && timer.owner == impl.calling; }));
    if (waiting >= kMaxTimersPerInstance) {
        luaL_error(state,
                   "the script has %d timers and tasks waiting, which is as many as it may: does it make "
                   "one in every frame?",
                   static_cast<int>(kMaxTimersPerInstance));
    }
    // On the list before it holds anything, so that nothing is held that is not on it.
    impl.timers.push_back({.owner = impl.calling, .due = impl.seconds + seconds, .interval = repeats ? seconds : 0.0});
    impl.timers.back().function = luaL_ref(state, LUA_REGISTRYINDEX);
    impl.timers.back().id = impl.nextTimer++;
    return impl.timers.back().id;
}

uint64_t ScriptSystem::startTask(lua_State* state, int arguments) {
    Impl& impl = *m_impl;
    if (impl.calling.isNull()) {
        luaL_error(state, "a task belongs to the script of an entity: start it in a function the engine calls");
    }
    const auto waiting = static_cast<size_t>(std::ranges::count_if(
        impl.timers, [&](const Timer& timer) { return timer.id != 0 && timer.owner == impl.calling; }));
    if (waiting >= kMaxTimersPerInstance) {
        luaL_error(state,
                   "the script has %d timers and tasks waiting, which is as many as it may: does it start "
                   "one in every frame?",
                   static_cast<int>(kMaxTimersPerInstance));
    }
    luaL_checkstack(state, 2, "the scripts are out of stack");
    // The coroutine, below the function and what it is called with - which move over.
    lua_State* thread = lua_newthread(state);
    lua_insert(state, -(arguments + 2));
    lua_xmove(state, thread, arguments + 1);
    impl.timers.push_back({.owner = impl.calling, .due = impl.seconds});
    impl.timers.back().thread = luaL_ref(state, LUA_REGISTRYINDEX);
    const uint64_t id = impl.nextTimer++;
    impl.timers.back().id = id;
    // It runs at once, as far as its first wait.
    impl.resumeTask(state, id, arguments);
    return id;
}

void ScriptSystem::cancelTimer(uint64_t timer) {
    // Marked, not let go of: it may be the task that is asking.
    if (Timer* found = m_impl->findTimer(timer); found != nullptr) {
        found->id = 0;
    }
}

bool ScriptSystem::isRunningTask(const lua_State* state) const {
    return state != nullptr && m_impl->task == state;
}

void ScriptSystem::requestScene(const Uuid& scene) {
    m_impl->requestedScene = scene;
}

std::optional<Uuid> ScriptSystem::takeRequestedScene() {
    return std::exchange(m_impl->requestedScene, std::nullopt);
}

void ScriptSystem::destroyLater(Entity entity) {
    if (std::ranges::find(m_impl->doomed, entity) == m_impl->doomed.end()) {
        m_impl->doomed.push_back(entity);
    }
}

void ScriptSystem::Impl::reconcile(const World& world) {
    // Who asks for an instance: every entity that is switched on, with all above it,
    // and has a Script that names a script - parents before what hangs under them.
    wanted.clear();
    world.traverse([&](Entity entity, uint32_t depth) {
        if (enabledAtDepth.size() <= depth) {
            enabledAtDepth.resize(depth + 1);
        }
        const bool enabled = (depth == 0 || enabledAtDepth[depth - 1]) && world.isEnabled(entity);
        enabledAtDepth[depth] = enabled;
        const ScriptComponent* component = enabled ? world.tryGet<ScriptComponent>(entity) : nullptr;
        if (component != nullptr && !component->script.isNil()) {
            wanted.push_back(entity);
        }
    });

    // Instances whose entity is gone are dropped; those whose entity asks for something
    // else now - no script, another one, other properties - are ended.
    for (const Entity entity : order) {
        const auto found = instances.find(entity);
        if (found == instances.end()) {
            continue;
        }
        if (!world.isValid(entity)) {
            forgetTimers(entity);
            release(found->second);
            instances.erase(found);
            continue;
        }
        const auto* component = world.tryGet<ScriptComponent>(entity);
        if (component == nullptr || *component != found->second.blueprint) {
            end(entity);
        }
    }
    std::erase_if(order, [&](Entity entity) { return !instances.contains(entity); });

    for (const Entity entity : wanted) {
        if (!instances.contains(entity)) {
            make(entity, *world.tryGet<ScriptComponent>(entity));
        }
    }
}

Instance* ScriptSystem::Impl::running(const World& world, Entity entity) {
    const auto found = instances.find(entity);
    const bool reachable = found != instances.end() && found->second.phase != Phase::Stopped && world.isValid(entity) &&
                           world.isEnabledInHierarchy(entity);
    return reachable ? &found->second : nullptr;
}

void ScriptSystem::Impl::startAll(const World& world) {
    for (const Entity entity : wanted) {
        Instance* instance = running(world, entity);
        if (instance == nullptr || instance->started) {
            continue;
        }
        instance->phase = Phase::Running;
        if (scripts.at(instance->blueprint.script).hears(Callback::Start)) {
            call(entity, *instance, Callback::Start);
        }
        // A start that an error cut short has not happened: it is tried again when
        // the script is another, and nothing is told that it ends. What it had asked
        // to have done later is not done - the start that is tried again asks again.
        instance->started = instance->phase == Phase::Running;
        if (!instance->started) {
            forgetTimers(entity);
        }
    }
}

void ScriptSystem::Impl::tellInput(const World& world, std::span<const ActionEvent> events) {
    for (const Entity entity : wanted) {
        for (const ActionEvent& event : events) {
            // Asked again for every event: the one before may have stopped the script,
            // or switched its entity off. And nothing is called before `start`.
            Instance* instance = running(world, entity);
            if (instance == nullptr || !instance->started ||
                !scripts.at(instance->blueprint.script).hears(Callback::Input)) {
                break;
            }
            const std::array<Argument, 2> arguments = {Argument(event.action), Argument(event.pressed)};
            call(entity, *instance, Callback::Input, arguments);
        }
    }
}

void ScriptSystem::Impl::updateAll(const World& world) {
    for (const Entity entity : wanted) {
        Instance* instance = running(world, entity);
        if (instance != nullptr && instance->started &&
            scripts.at(instance->blueprint.script).hears(Callback::Update)) {
            const std::array<Argument, 1> arguments = {Argument(deltaSeconds)};
            call(entity, *instance, Callback::Update, arguments);
        }
    }
}

void ScriptSystem::Impl::tellContact(const World& world, Entity entity, Entity other, const CollisionEvent& event,
                                     bool mirrored) {
    Instance* instance = running(world, entity);
    if (instance == nullptr || !instance->started) {
        return;
    }
    Callback callback = Callback::CollisionBegan;
    bool met = true;
    switch (event.type) {
        case CollisionEventType::CollisionBegan:
            break;
        case CollisionEventType::CollisionEnded:
            callback = Callback::CollisionEnded;
            met = false;
            break;
        case CollisionEventType::TriggerEntered:
            callback = Callback::TriggerEntered;
            break;
        case CollisionEventType::TriggerLeft:
            callback = Callback::TriggerLeft;
            met = false;
            break;
    }
    if (!scripts.at(instance->blueprint.script).hears(callback)) {
        return;
    }
    // What parts has no place where it touches: only what meets is told how.
    const std::array<Argument, 2> arguments = {
        Argument(other),
        Argument(Contact{
            .position = event.position, .normal = mirrored ? -event.normal : event.normal, .speed = event.speed}),
    };
    call(entity, *instance, callback, met ? Arguments(arguments) : Arguments(arguments).first(1));
}

ScriptStatistics ScriptSystem::update(const ScriptContext& context) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.context == nullptr, "the scripts are updated from inside an update of the scripts");
    if (impl.context != nullptr) {
        return impl.statistics();
    }

    const Impl::Reach reach(impl, context);
    const World& world = context.world;
    impl.calls = 0;
    impl.followAssets();
    impl.reconcile(world);

    const double deltaSeconds = std::isfinite(context.deltaSeconds) ? std::max(context.deltaSeconds, 0.0) : 0.0;
    impl.deltaSeconds = deltaSeconds;
    impl.seconds += deltaSeconds;
    ++impl.updates;

    // `start` for what has not started, then `input` for what was pressed and released,
    // then `update` for all: three walks, each in the order of the hierarchy. A script
    // may switch an entity off, or have something else destroy it, before its turn
    // comes - which is why every one of them asks again whether an instance runs.
    impl.startAll(world);
    // The names in the events are the map's own, and the map is not changed while the
    // scripts run: they outlive every call.
    impl.tellInput(world, context.controls != nullptr ? context.controls->events() : std::span<const ActionEvent>());
    impl.fireTimers(world);
    impl.updateAll(world);

    impl.destroyDoomed();
    impl.sweepTimers();
    std::erase_if(impl.order, [&](Entity entity) { return !impl.instances.contains(entity); });
    impl.listProblems();
    return impl.statistics();
}

ScriptStatistics ScriptSystem::tellContacts(const ScriptContext& context, std::span<const CollisionEvent> events) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.context == nullptr, "the scripts are told of contacts from inside an update of the scripts");
    if (impl.context != nullptr || events.empty()) {
        return impl.statistics();
    }
    const Impl::Reach reach(impl, context);
    const World& world = context.world;
    for (const CollisionEvent& event : events) {
        impl.tellContact(world, event.first, event.second, event, false);
        impl.tellContact(world, event.second, event.first, event, true);
    }
    impl.destroyDoomed();
    impl.sweepTimers();
    std::erase_if(impl.order, [&](Entity entity) { return !impl.instances.contains(entity); });
    impl.listProblems();
    return impl.statistics();
}

std::vector<ScriptCallbackInfo> ScriptSystem::callbacks() const {
    std::vector<ScriptCallbackInfo> result;
    result.reserve(kCallbackCount);
    for (size_t callback = 0; callback < kCallbackCount; ++callback) {
        result.push_back({.name = kCallbacks[callback].name,
                          .signature = kCallbacks[callback].signature,
                          .description = kCallbacks[callback].description,
                          .calls = m_impl->callbackCalls[callback]});
    }
    return result;
}

void ScriptSystem::clear(const ScriptContext& context) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.context == nullptr, "the scripts are cleared from inside an update of the scripts");
    if (impl.context != nullptr) {
        return;
    }
    impl.context = &context;
    // Children before what they hang under, as far as the order they were made in says.
    for (const Entity entity : impl.order | std::views::reverse) {
        impl.end(entity);
    }
    impl.destroyDoomed();
    // Every instance has ended, and its timers with it: what they held is let go of.
    impl.sweepTimers();
    for (auto& [id, script] : impl.scripts) {
        impl.release(script);
    }
    impl.instances.clear();
    impl.order.clear();
    impl.scripts.clear();
    impl.scriptOrder.clear();
    impl.problems.clear();
    impl.knowsRevision = false;
    impl.seconds = 0.0;
    impl.updates = 0;
    impl.deltaSeconds = 0.0;
    // What a script of the scene that ends asked for is not asked of the next.
    impl.requestedScene.reset();
    impl.context = nullptr;
}

} // namespace cordite
