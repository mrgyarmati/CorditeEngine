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
/// function Door:input(action, pressed)     -- for every control that was pressed or released in the frame
///     if action == "use" and pressed then self.travelled = 0 end
/// end
///
/// function Door:collisionBegan(other, contact)    -- the body of the entity has met that of another
///     if contact.speed > 2.0 then cordite.log.info(other:name() .. " slammed into the door") end
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
/// - **input** is called between the two, in the same order, once for every action of
///   the input map that was pressed or released in the frame: with its name, and true
///   for pressed. What is tapped within a frame is pressed, then released. Only a
///   world that is given controls (ScriptContext::controls) has any.
/// - **Timers and tasks** - what a script makes with `cordite.time.after`, `every` and
///   `run` - are called after `input` and before `update`, in the order they were made,
///   each once in an update at most. They belong to the instance whose script was
///   being called when they were made: they are called while it runs, wait while it
///   does not, and are gone when it ends. A timer's function is called with the table
///   of that instance. A task is a function that runs a piece at a time: at once as
///   far as its first `cordite.time.wait`, and on from there when the time has passed.
/// - **collisionBegan**(other, contact), **collisionEnded**(other),
///   **triggerEntered**(other, contact) and **triggerLeft**(other) are called when
///   tellContacts() is given what the simulation saw: for either of the two entities
///   whose bodies met or parted, with the other entity - which may be gone by then,
///   and says so (`other:isValid()`). `contact` is a table: `position`, where the two
///   touch in the world; `normal`, which way the other lies from there; and `speed`,
///   how fast they were closing in on each other, in metres per second. Of a trigger
///   both hear too: the entity that has it, and what moved into it.
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
#include <string_view>
#include <vector>

namespace cordite {

class InputActions;
class PhysicsSystem;
struct CollisionEvent;

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
    /// The controls of the game as they are in this frame (platform/InputMap.h), already
    /// worked out for it. None for a world that nothing is played in: no `input` is
    /// called then, and a script that asks about a control is told that there are none.
    const InputActions* controls = nullptr;
    /// The simulation of the world's bodies (physics/PhysicsSystem.h), as the last frame
    /// left it: what scripts ask about rays and reach, and push. None for a world that
    /// is not simulated, and then a script that asks is told so.
    PhysicsSystem* physics = nullptr;
};

/// What an update of the scripts did.
struct ScriptStatistics {
    /// Scripts that are loaded.
    uint32_t scripts = 0;
    /// Entities that have an instance of their script, and of those the ones that an
    /// error has stopped or that could not be made.
    uint32_t instances = 0;
    uint32_t stopped = 0;
    /// Functions of scripts called since the last update() began: by it, and by what
    /// tellContacts() has told since. A timer that is called and a task that goes on
    /// count as well.
    uint32_t calls = 0;
    /// Timers and tasks that scripts have waiting.
    uint32_t timers = 0;
    /// The memory the scripts hold, in bytes.
    uint64_t memoryBytes = 0;

    bool operator==(const ScriptStatistics&) const = default;
};

/// A function the engine calls in scripts, and how often it has.
struct ScriptCallbackInfo {
    /// What a script calls it: "start", "update", "collisionBegan".
    std::string_view name;
    /// Calls since the system was made, of all instances together.
    uint64_t calls = 0;

    bool operator==(const ScriptCallbackInfo&) const = default;
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
    /// `start` for those that are new, `input` for what was pressed and released, the
    /// timers and tasks that are due and `update` for all that run, and destroys the
    /// entities that scripts asked to have destroyed.
    ScriptStatistics update(const ScriptContext& context);

    /// Tells the scripts what the bodies of their entities did in the step of the
    /// simulation that followed update(): `events` is PhysicsSystem::events(). For every
    /// event the script of the first entity is told, then that of the second - if it
    /// runs, has started and has a function for it. Destroys the entities that scripts
    /// asked to have destroyed, as update() does. What it returns goes on from what
    /// update() returned: `calls` counts both.
    ScriptStatistics tellContacts(const ScriptContext& context, std::span<const CollisionEvent> events);

    /// Every function the engine calls in scripts, in the order a frame calls them, with
    /// how often each has been called.
    [[nodiscard]] std::vector<ScriptCallbackInfo> callbacks() const;

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

    /// For the functions of the engine that scripts call. Has the function on top of
    /// the stack of `state` called in `seconds`, by the clock of the scripts - and if it
    /// `repeats`, every `seconds` from then on - with the table of the instance whose
    /// script is being called now, which is who it belongs to: it is called only while
    /// that instance runs, and goes when it does. Pops the function, and returns the
    /// number that stands for the timer. Raises a Lua error when no script of an entity
    /// is being called, and when that script has as many waiting as it may.
    uint64_t addTimer(lua_State* state, double seconds, bool repeats);

    /// The same for a task: the function below the `arguments` values on top of the
    /// stack is run as a coroutine, at once and as far as its first wait - a yield of so
    /// many seconds, or of nothing for the next update - and goes on from there when the
    /// time has passed. Pops the function and the values. An error in the task is raised
    /// here, with where the task was; later ones stop the instance, as any error does.
    uint64_t startTask(lua_State* state, int arguments);

    /// Lets a timer or a task go. A number that stands for none - one that was called,
    /// that has ended, that never was - is nothing to cancel.
    void cancelTimer(uint64_t timer);

    /// Whether `state` is the coroutine of the task that is being run: the one place a
    /// script can wait in.
    [[nodiscard]] bool isRunningTask(const lua_State* state) const;

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
