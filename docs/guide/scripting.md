# Scripting

Gameplay in Cordite is written in [Lua 5.4](https://www.lua.org/manual/5.4/). A script is a
`.lua` file in the assets of your project; a **Script** component puts it on an entity, and
the engine runs it for that entity while the game runs - in the player, and in the editor
while the scene is played. While you edit a scene, no script runs.

The scripts of the showcase project (`samples/showcase/assets/scripts/`) are written to be
read: `clock.lua` shows what a script is made of, `patrol.lua` one that moves a character,
`hop.lua` one that reacts to collisions and plays sounds, and `selftest.lua` calls every
function there is. Every function, with its arguments and what it returns, is listed in the
[scripting reference](../reference/script-api.md).

## A first script

Create `scripts/spinner.lua` in the assets of your project:

```lua
-- Turns its entity around the vertical axis.
local Spinner = {
    -- What a Script component may set differently for each entity, with the defaults.
    properties = {
        speed = 90.0, -- degrees a second
    },
}

function Spinner:start()
    self.turned = 0.0
end

function Spinner:update(deltaSeconds)
    local degrees = self.properties.speed * deltaSeconds
    self.entity:rotate(cordite.axisAngle({0, 1, 0}, degrees))
    self.turned = self.turned + degrees
end

return Spinner
```

Then, in the editor, select an entity, add a **Script** component in the Inspector and pick
`scripts/spinner.lua` for its `script` (or drag the file from the Assets panel onto the
property). Press **Play** (F5): the entity turns. Press **Stop** (Shift+F5) and the scene is
back as it was.

## What a script is

A script file returns a table. The engine makes one table of its own for every entity that
runs the script - `self` - with two things in it:

- `self.entity` - the entity it runs for;
- `self.properties` - the properties of the script, with the values the entity's Script
  component gives them, or the defaults.

Anything else a script keeps between calls, it keeps in `self` (`self.turned` above). The
functions are found through `self`, so a script can call its own: `self:turn()`.

The engine calls these functions when a script has them; all of them are optional:

| Function | When |
|----------|------|
| `start()` | Once, before the first update - parents before their children. |
| `input(action, pressed)` | For every action of the input map that was pressed or released in the frame. |
| `update(deltaSeconds)` | Every frame, with the seconds since the last one. |
| `sequenceEvent(name)` | A sequence that the entity's SequencePlayer plays passed one of its events. See [Cameras](cameras.md#cinematic-sequences). |
| `collisionBegan(other, contact)` | The entity's body began to touch another. `contact` has `position`, `normal` (towards the other) and `speed`. |
| `collisionEnded(other)` | The two bodies parted. |
| `triggerEntered(other, contact)` | A body entered a trigger - called for both. |
| `triggerLeft(other)` | The body left the trigger again. |
| `destroy()` | The script ends while its entity is still there: the component was removed or changed, a script destroyed the entity, or another scene is loaded. |

In every frame, scripts run before physics: what a script decides in `update` is what the
simulation does in that frame. `sequenceEvent` is called right after the sequences have
played, still before the simulation; collision functions are called after the simulation
step.

### Properties

The `properties` table declares what can be set per entity, with defaults:

```lua
properties = {
    speed = 1.0,          -- a number
    label = "Clock",      -- text
    to = { 0, 0, -2 },    -- a vector: three numbers
    landing = "sounds/hop.wav", -- an asset, by where its file is
}
```

A Script component sets `properties` as JSON (in the Inspector, as text). A value must have
the kind of its default: a number for a number, text for text. A property the script does
not declare, or one of the wrong kind, keeps the script from starting - and says so.

## Entities and components

`self.entity` is an entity value. Others are found by name or id, or by what they have:

```lua
local door = cordite.world.find("Door")           -- nil when there is none
local lights = cordite.world.withComponent("Light")
local crate = cordite.world.create("Crate")        -- a new entity
crate:setParent(self.entity)
```

An entity value stays valid as a value after its entity is gone: ask `entity:isValid()`
before using one you kept.

Every component is reached through its properties, by the names the Inspector shows:

```lua
if self.entity:has("Light") then
    local intensity = self.entity:get("Light", "intensity")
    self.entity:set("Light", "intensity", intensity * 0.5)
    self.entity:set("Light", { color = { 1, 0.4, 0.2 }, castsShadows = true })
end
-- Assets are referred to by id; cordite.assets.find gives the id of a file.
self.entity:add("AudioSource", { sound = cordite.assets.find("sounds/hum.wav"), loop = true })
self.entity:remove("Script")
```

Writes are checked exactly as edits in the editor are: a value of the wrong kind, a number
outside its range or a property that does not exist is an error, and nothing changes. The
[component reference](../reference/components.md) lists every component and property.

Moving things has functions of its own: `position`, `setPosition`, `rotation`,
`setRotation`, `scale`, `worldPosition`, `setWorldPosition`, `translate`, `rotate`,
`lookAt`, `forward`, `right`, `up`.

`entity:destroy()` removes an entity, with everything below it, once the scripts of the frame
have run.

## Vectors and rotations

A vector is a table with `x`, `y` and `z`; a rotation one with `x`, `y`, `z` and `w`. Those
the engine returns can do arithmetic:

```lua
local a = cordite.vec3(1, 0, 0)
local b = self.entity:forward() * 2.0 + a
local distance = a:distance(b)
local turned = cordite.axisAngle({ 0, 1, 0 }, 45) -- degrees
local facing = cordite.lookRotation(b - a)
```

Wherever a vector is expected, a plain table will do: `{ 0, 1, 0 }` or `{ x = 0, y = 1, z = 0 }`.
Vectors are values: changing one you were given changes nothing until you hand it back
(`entity:setPosition(p)`).

The engine uses metres, seconds and degrees; +Y is up and -Z is forward.

## Controls

A script asks about controls by the names of the project's input map, never by keys:

```lua
function Player:input(action, pressed)
    if action == "jump" and pressed then
        self.entity:set("CharacterController", "jump", 4.0)
    end
end

function Player:update(deltaSeconds)
    local move = cordite.vec3(cordite.input.axis("moveX"), 0, -cordite.input.axis("moveY"))
    if cordite.input.isDown("sprint") then
        move = move * 2.0
    end
end
```

`isDown`, `wasPressed` and `wasReleased` answer for actions, `axis` for axes. A name the map
does not have is an error that lists the names it has. See [Input](input.md).

## Time, timers and tasks

```lua
cordite.time.after(2.0, function() cordite.log.info("two seconds later") end)
local timer = cordite.time.every(0.5, self.blink)   -- called with self, twice a second
cordite.time.cancel(timer)

-- A task runs like a function that can wait.
cordite.time.run(function()
    self.entity:set("Light", "intensity", 0)
    cordite.time.wait(1.0)
    self.entity:set("Light", "intensity", 5)
end)
```

Timers and tasks belong to the entity whose script made them: they are gone when its script
is. `cordite.time.seconds()`, `delta()` and `frame()` tell the time of the game.

## Physics

```lua
local hit = cordite.physics.raycast(origin, { 0, -1, 0 }, 10.0)
if hit then
    cordite.log.info("ground at " .. hit.distance .. " m: " .. hit.entity:name())
end
for _, entity in ipairs(cordite.physics.overlapSphere(self.entity:worldPosition(), 3.0)) do
    entity:addImpulse({ 0, 5, 0 })
end
```

Speeds are properties of `RigidBody`; forces, impulses and torques are functions
(`addForce`, `addImpulse`, `addTorque`). Scripts see the simulation as the last frame left
it: an entity that was given a collider in this frame has its body from the next one.

## Sound

What an entity sounds like is its `AudioSource` component: set `playing` to true to play it.
A sound that belongs to no entity is played with `cordite.audio.play`:

```lua
cordite.audio.play("sounds/explosion.wav", { position = where, volume = 0.8 })
cordite.audio.setVolume("music", 0.5)
```

## Scenes

`cordite.scene.load("scenes/level2.scene.json")` changes the scene between two frames, after
the new one was checked: a scene that cannot be loaded leaves the current one running and
says why.

## Logging and errors

`cordite.log.debug`, `info`, `warn` and `error` write to the log - the editor's Console.

A script that is wrong never stops the game. An error stops the script of that one entity,
and is reported once, with the entity, the file and the line; the rest of the game goes on.
Some mistakes Lua would let pass are errors here too:

- A name that was never given a value is an error, not `nil` - declare names with `local`.
- One call into the scripts may run at most ten million instructions; an endless loop ends
  with an error instead of a frozen game.
- Scripts have no access to files, the operating system or the network.

## Changing scripts while the game runs

The editor notices when a script file is saved - by any text editor - and reads it again. A
scene that is being played goes on with the new text: every entity keeps what its script
holds in `self`, and gets the new functions. A script with an error in its new text keeps
running the old one, and the error is shown in the Console.
