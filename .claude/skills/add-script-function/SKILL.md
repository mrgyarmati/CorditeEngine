---
name: add-script-function
description: Give gameplay scripts (Lua) a new function, or change one. Use when a script needs to reach something of the engine it cannot reach yet - a query, a setter, a new library such as audio - or when a function of cordite.Entity, cordite.world, cordite.Vec3 and the like is wrong.
---

# Add a function that scripts can call

Scripts are Lua 5.4, run by `ScriptSystem` in a `ScriptRuntime` (see the Scripting section
of [AGENTS.md](../../../AGENTS.md)). Everything they can call is a C++ function in
`engine/cordite/scripting/`, registered under a name with a signature and a description.

## Before writing one

- **Is it a property of a component?** Then it needs nothing: `entity:get("Light",
  "intensity")` and `entity:set(...)` reach every property the reflection describes,
  checked as the editor checks them. Add a function only for what is not a property: a
  question to a system, an action, a computation.
- **Which library?** `cordite.Entity` for what is asked of one entity (`entity:name()`),
  `cordite.world` for the world as a whole, `cordite.Vec3` / `cordite.Quat` for the
  values, `cordite.time`, `cordite.assets`, `cordite.log`, `cordite.input`,
  `cordite.physics`, `cordite.audio`, `cordite.scene`; a new library for a new
  subject. Names are `camelCase`, verbs for what changes something (`setPosition`), nouns
  for what is read (`position`).

## Steps

1. **Write the function** in the file of its library (`ScriptEntityApi.cpp`,
   `ScriptWorldApi.cpp`, `ScriptMathApi.cpp`, `ScriptComponentApi.cpp`,
   `ScriptInputApi.cpp`, `ScriptPhysicsApi.cpp`, `ScriptAudioApi.cpp`), in the anonymous
   namespace:

   ```cpp
   int entityDistanceTo(lua_State* state) {
       const Entity entity = checkEntity(state, 1);        // raises for anything else, or one that is gone
       const Vec3 point = checkVec3(state, 2);             // raises for what is no vector of finite numbers
       const World& world = checkContext(state).world;     // raises outside an update
       const Vec3 position(world.computeWorldMatrix(entity)[3]);
       lua_pushnumber(state, scriptNumber(glm::distance(position, point)));
       return 1;                                           // how many values were pushed
   }
   ```

   - Arguments are checked with `checkEntity`, `checkVec3`, `checkQuat`
     (`ScriptBindings.h`) and Lua's own `luaL_check...`; their messages name the function
     and the argument. Raise anything else with `luaL_error` or `luaL_argerror`.
   - Read the arguments first, then do the work: an error leaves the function at once.
   - A float goes to Lua through `scriptNumber`, `pushVec3` or `pushQuat`, so that a
     script sees 0.1 and not 0.10000000149011612.
   - What moves or sizes an entity must be finite: `checkVec3` sees to that.
   - Do not keep a pointer or a reference to a component across anything that runs a
     script or changes components.
   - What a function needs of the engine besides the world comes through
     `ScriptContext` (`ScriptSystem.h`) - the controls and the simulation are there -
     as a pointer that is null where a world has none. Say so in an error
     (`checkControls`, `checkPhysics`); do not answer as if there were.
   - A script does not destroy entities; it asks (`ScriptSystem::destroyLater`).
   - A list of entities is in the order of the hierarchy.

2. **Register it**: add a `ScriptFunction` to the array of its library, with the
   signature as a script writes the call and a sentence or two that say what it does,
   what it gives back and when it fails. That text is what people and agents read.

   ```cpp
   ScriptFunction{"distanceTo", "entity:distanceTo(point) -> number",
                  "How far the entity is from a point of the world, in metres.", entityDistanceTo},
   ```

   A new library is registered in a `register...Api` function declared in
   `ScriptBindings.h` and called by `ScriptSystem::create`.

3. **Test it** in `tests/unit/scripting/Script...ApiTests.cpp` with `ScriptStage`
   (`ScriptTestSupport.h`), which runs a piece of Lua as the update of an entity:

   ```cpp
   CHECK(stage.line("print(entity:distanceTo(cordite.vec3(3, 4, 0)))") == "5.0");
   CHECK(stage.refusal("entity:distanceTo()") == "bad argument #1 to 'distanceTo' (Vec3 expected, got no value)");
   ```

   Cover what it does, each kind of wrong argument, an entity that is gone, and what the
   world looks like afterwards from C++. `ScriptEntityApiTests` pins the list of what an
   entity can be asked; add the name there.

4. **Call it in the showcase**: `samples/showcase/assets/scripts/selftest.lua` calls
   every function scripts are given, in the running game, and compares the answer
   with what it should be:

   ```lua
   self:expect("how far it is to a point", scratch:distanceTo(vec3(3, 4, 0)) == 5)
   ```

   The `samples` test fails, with the name of the function, until a script of the
   showcase has called it. What the self-test changes it changes on the entities it
   makes itself, and it leaves the scene as it found it - the same test checks that.
   The showcase is also where people learn the API: say in a comment what is shown.

5. **Document it** where it changes what AGENTS.md or `docs/architecture.md` say, and
   run the pre-commit gate (see the pre-commit-review skill).

## A function the engine calls in a script

`start`, `update`, `input`, `collisionBegan` and the like go the other way: the engine
calls them. They are one list - `Callback` and `kCallbacks` in `ScriptSystem.cpp` -
and a new one is:

1. an entry in both, in the order a frame calls them - in `kCallbacks` with how a
   script writes it and a sentence or two on when it is called and with what, which
   is what `script.api` hands to whoever writes a script;
2. the place that calls it, with `Impl::call(entity, instance, Callback::..., arguments)`
   for an instance that `Impl::running()` returns and that has started;
3. its lines in the comment at the top of `ScriptSystem.h`, which is where a script
   writer reads what is called when, and with what;
4. tests in `tests/unit/scripting/` - what it is called with, in which order, and that
   a script that is off, stopped or not started hears nothing;
5. a script of the showcase that has it, and something in the scene that makes the
   engine call it: the `samples` test names every function on the list that the
   showcase never heard.

## What goes wrong

- **`bad argument #1 to '?'`**: a function was called through another C function. Check
  the arguments in the function a script calls directly.
- **A test goes on after a failed `REQUIRE`**: inside `ScriptRuntime::protect()` and in
  functions scripts call, use `CHECK`. Lua takes whatever is thrown through it for a
  script error.
- **A crash only in Debug, deep in Lua**: something used Lua's API outside
  `ScriptRuntime::protect()`, or pushed more than the stack had room for
  (`luaL_checkstack`).
- **Numbers print with a long tail**: a float was pushed with `lua_pushnumber` instead
  of through `scriptNumber`.
