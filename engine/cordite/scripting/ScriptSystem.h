#pragma once

/// @file
/// Runs the scripts of a world: gives every entity that has a Script a running instance
/// of it, and calls what the script has for each moment.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<ScriptSystem> scripts, ScriptSystem::create());
///
/// // A game, every frame, before the simulation moves on:
/// scripts->update({.world = world, .components = components, .assets = assets, .deltaSeconds = frameSeconds});
///
/// for (const ScriptProblem& problem : scripts->problems()) {
///     CORDITE_LOG_WARN("Game", "{}", problem.message);
/// }
/// @endcode
///
/// A script is a `.lua` file of the project that returns a table: the functions the
/// engine calls, and the properties an entity may set.
///
/// @code{.lua}
/// local Door = {
///     properties = { speed = 2.0, opens = "north" },    -- what a Script component may give other values
/// }
///
/// function Door:start()                    -- once, before the first update
///     self.travelled = 0
/// end
///
/// function Door:update(deltaSeconds)       -- every frame
///     self.travelled = self.travelled + self.properties.speed * deltaSeconds
/// end
///
/// function Door:destroy()                  -- when the script is taken off, or a script destroys the entity
/// end
///
/// return Door
/// @endcode
///
/// Every entity with a Script has a table of its own - `self` in the functions above -
/// that holds `entity`, the entity it runs for, `properties`, and whatever the script
/// keeps there. The functions are the script's, found through the table.
///
/// What happens when:
///
/// - **start** is called once for an instance, before its first update, parents before
///   what hangs under them. **update** follows in the same order, every update.
/// - An entity that is switched off, itself or by something above it, is left alone:
///   nothing of its script is called, and one that has not started waits.
/// - **destroy** is called while the entity is still there: when its Script is removed,
///   names another script or is given other properties - which makes a new instance -
///   when a script destroys the entity (ScriptSystem::destroyLater()), and when the
///   system is cleared. An entity that something else destroys takes its instance along
///   without a word.
/// - **properties** are the script's own, as it declares them, with the values the
///   component gives. A component may only give a value to what the script declares,
///   and of the same kind - a number for a number, text for text; anything else keeps
///   the instance from being made, and is a problem.
/// - **An error** in a function stops the instance: nothing of it is called again. It
///   is said once, in the log and in problems(), with the entity and the place.
/// - **A script that cannot be loaded** - no such file, text that is not Lua, something
///   returned that is no table - is a problem, and the entities that name it have no
///   instance until it can.
/// - **When the assets are refreshed**, every script whose text has changed is loaded
///   again. Instances keep their tables, and so what they know; from then on they are
///   called through the new functions, and those that an error had stopped are given
///   another go. A new text that cannot be loaded leaves the old one running.
///
/// What scripts can do is what ScriptRuntime (ScriptRuntime.h) lets them, and the
/// functions registered with it; each instance's calls are calls into the scripts with
/// the limits said there.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace cordite {

struct ScriptSystemDesc {
    /// The limits of the scripts. See ScriptRuntimeDesc.
    ScriptRuntimeDesc runtime;
};

/// What the scripts of a world have to work with during an update.
struct ScriptContext {
    World& world;
    /// The components scripts know by name.
    const ComponentRegistry& components;
    /// Where scripts are loaded from.
    AssetManager& assets;
    /// The time that passes with this update, in seconds. Less than no time, and a time
    /// that is no number, is no time. A double, as every number of a script is.
    double deltaSeconds = 0.0;
};

/// What an update of the scripts did.
struct ScriptStatistics {
    /// Scripts that are loaded.
    uint32_t scripts = 0;
    /// Entities that have an instance of their script, and of those the ones that an
    /// error has stopped or that could not be made.
    uint32_t instances = 0;
    uint32_t stopped = 0;
    /// Functions of scripts that the update called.
    uint32_t calls = 0;
    /// The memory the scripts hold, in bytes.
    uint64_t memoryBytes = 0;

    bool operator==(const ScriptStatistics&) const = default;
};

/// Something that keeps a script, or the script of one entity, from running.
struct ScriptProblem {
    /// The script it is about.
    Uuid script;
    /// The entity whose instance does not run; null when the script itself is at fault
    /// and this is said once for all that name it.
    Entity entity;
    /// What is wrong, with the name of the script and of the entity.
    std::string message;

    bool operator==(const ScriptProblem&) const = default;
};

class ScriptSystem {
public:
    /// Fails as ScriptRuntime::create() does.
    [[nodiscard]] static Result<std::unique_ptr<ScriptSystem>> create(const ScriptSystemDesc& desc = {});
    ~ScriptSystem();
    CORDITE_NON_MOVABLE(ScriptSystem);

    /// Brings the instances in line with the Script components of the world, then calls
    /// `start` for those that are new and `update` for all that run, and destroys the
    /// entities that scripts asked to have destroyed.
    ScriptStatistics update(const ScriptContext& context);

    /// Ends every instance - with `destroy`, where its entity is still there - and
    /// forgets every script that was loaded: the next update starts from nothing. Call
    /// it before the world is given another scene.
    void clear(const ScriptContext& context);

    /// What keeps scripts from running, as of the last update: scripts that cannot be
    /// loaded, then instances in the order they were made.
    [[nodiscard]] std::span<const ScriptProblem> problems() const;

    /// The runtime the scripts run in: to give them more functions, and to ask what
    /// they have been given.
    [[nodiscard]] ScriptRuntime& runtime();

    /// For the functions of the engine that scripts call. The world as the update that
    /// is under way was given it; null outside an update, where no script runs.
    [[nodiscard]] const ScriptContext* context() const;

    /// Destroys `entity` and everything below it once the scripts of this update have
    /// run: `destroy` is called for every instance in there, children first, and then
    /// the entities go. Until then the entity is as valid as ever. Asking twice is
    /// asking once.
    void destroyLater(Entity entity);

    /// For the functions of the engine that scripts call: pushes the table of the
    /// instance that runs for `entity` onto the stack of `state`, or nil when none
    /// does - no Script, one that could not be made, one that is switched off and
    /// has not been made yet.
    void pushInstance(lua_State* state, Entity entity) const;

    /// The time the scripts have been running, in seconds: the sum of what the updates
    /// let pass. How many updates there were, and the time the last of them let pass.
    /// All nothing after clear().
    [[nodiscard]] double seconds() const;
    [[nodiscard]] uint64_t updates() const;
    [[nodiscard]] double deltaSeconds() const;

    /// The system that owns the runtime a function was called in.
    [[nodiscard]] static ScriptSystem& of(lua_State* state);

private:
    struct Impl;

    explicit ScriptSystem(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
