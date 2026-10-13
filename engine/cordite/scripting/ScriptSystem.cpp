#include "cordite/scripting/ScriptSystem.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptValue.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {
namespace {

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
    bool hasStart = false;
    bool hasUpdate = false;
    bool hasDestroy = false;
    /// Why it cannot be loaded - or why its newest text cannot, while an older runs.
    std::string problem;

    [[nodiscard]] bool isLoaded() const { return table != LUA_NOREF; }
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

/// What a function of a script is called with besides its instance.
using Argument = std::optional<double>;

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
    /// Functions of scripts called by the update that is under way.
    uint32_t calls = 0;
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

    [[nodiscard]] LoadedScript& scriptOf(const Uuid& id);
    Loaded load(LoadedScript& script) const;
    void followAssets();
    void release(LoadedScript& script) const;

    void make(Entity entity, const ScriptComponent& component);
    void stop(Entity entity, Instance& instance, std::string_view problem) const;
    void call(Entity entity, Instance& instance, const char* function, Argument argument);
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
        loaded.hasStart = hasFunction(state, "start");
        loaded.hasUpdate = hasFunction(state, "update");
        loaded.hasDestroy = hasFunction(state, "destroy");

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
    script.hasStart = loaded.hasStart;
    script.hasUpdate = loaded.hasUpdate;
    script.hasDestroy = loaded.hasDestroy;
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

void ScriptSystem::Impl::call(Entity entity, Instance& instance, const char* function, Argument argument) {
    ++calls;
    const Status called = runtime->protect([&](lua_State* state) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, instance.self);
        // Found through the instance, so that one which has a function of its own for
        // this is called with that.
        if (lua_getfield(state, -1, function) != LUA_TFUNCTION) {
            return;
        }
        lua_insert(state, -2);
        if (argument) {
            lua_pushnumber(state, static_cast<lua_Number>(*argument));
        }
        lua_call(state, argument ? 2 : 1, 0);
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
    if (instance.phase == Phase::Running && instance.started && script != scripts.end() && script->second.hasDestroy &&
        context->world.isValid(entity)) {
        call(entity, instance, "destroy", std::nullopt);
    }
    release(instance);
    instances.erase(found);
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

void ScriptSystem::destroyLater(Entity entity) {
    if (std::ranges::find(m_impl->doomed, entity) == m_impl->doomed.end()) {
        m_impl->doomed.push_back(entity);
    }
}

ScriptStatistics ScriptSystem::update(const ScriptContext& context) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.context == nullptr, "the scripts are updated from inside an update of the scripts");
    if (impl.context != nullptr) {
        return impl.statistics();
    }

    /// Says for as long as the update lasts which world scripts reach.
    struct Reach {
        Reach(Impl& impl, const ScriptContext& context) : m_impl(impl) { impl.context = &context; }

        ~Reach() { m_impl.context = nullptr; }

        CORDITE_NON_MOVABLE(Reach);

    private:
        Impl& m_impl;
    };

    const Reach reach(impl, context);
    World& world = context.world;
    impl.calls = 0;
    impl.followAssets();

    // Who asks for an instance: every entity that is switched on, with all above it,
    // and has a Script that names a script - parents before what hangs under them.
    impl.wanted.clear();
    world.traverse([&](Entity entity, uint32_t depth) {
        if (impl.enabledAtDepth.size() <= depth) {
            impl.enabledAtDepth.resize(depth + 1);
        }
        const bool enabled = (depth == 0 || impl.enabledAtDepth[depth - 1]) && world.isEnabled(entity);
        impl.enabledAtDepth[depth] = enabled;
        const ScriptComponent* component = enabled ? world.tryGet<ScriptComponent>(entity) : nullptr;
        if (component != nullptr && !component->script.isNil()) {
            impl.wanted.push_back(entity);
        }
    });

    // Instances whose entity is gone are dropped; those whose entity asks for something
    // else now - no script, another one, other properties - are ended.
    for (const Entity entity : impl.order) {
        const auto found = impl.instances.find(entity);
        if (found == impl.instances.end()) {
            continue;
        }
        if (!world.isValid(entity)) {
            impl.release(found->second);
            impl.instances.erase(found);
            continue;
        }
        const ScriptComponent* component = world.tryGet<ScriptComponent>(entity);
        if (component == nullptr || *component != found->second.blueprint) {
            impl.end(entity);
        }
    }
    std::erase_if(impl.order, [&](Entity entity) { return !impl.instances.contains(entity); });

    // New instances, then `start` for what has not started, then `update` for all:
    // each in the order of the hierarchy. A script may switch an entity off, or have
    // something else destroy it, before its turn comes.
    for (const Entity entity : impl.wanted) {
        if (!impl.instances.contains(entity)) {
            impl.make(entity, *world.tryGet<ScriptComponent>(entity));
        }
    }
    const double deltaSeconds = std::isfinite(context.deltaSeconds) ? std::max(context.deltaSeconds, 0.0) : 0.0;
    impl.deltaSeconds = deltaSeconds;
    impl.seconds += deltaSeconds;
    ++impl.updates;
    const auto runs = [&](Entity entity) -> Instance* {
        const auto found = impl.instances.find(entity);
        const bool reachable = found != impl.instances.end() && found->second.phase != Phase::Stopped &&
                               world.isValid(entity) && world.isEnabledInHierarchy(entity);
        return reachable ? &found->second : nullptr;
    };
    for (const Entity entity : impl.wanted) {
        Instance* instance = runs(entity);
        if (instance == nullptr || instance->started) {
            continue;
        }
        instance->phase = Phase::Running;
        if (impl.scripts.at(instance->blueprint.script).hasStart) {
            impl.call(entity, *instance, "start", std::nullopt);
        }
        // A start that an error cut short has not happened: it is tried again when
        // the script is another, and nothing is told that it ends.
        instance->started = instance->phase == Phase::Running;
    }
    for (const Entity entity : impl.wanted) {
        Instance* instance = runs(entity);
        if (instance != nullptr && instance->started && impl.scripts.at(instance->blueprint.script).hasUpdate) {
            impl.call(entity, *instance, "update", deltaSeconds);
        }
    }

    impl.destroyDoomed();
    std::erase_if(impl.order, [&](Entity entity) { return !impl.instances.contains(entity); });
    impl.listProblems();
    return impl.statistics();
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
    impl.context = nullptr;
}

} // namespace cordite
