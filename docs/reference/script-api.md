# Scripting API

Everything gameplay scripts are written against: the functions the engine calls in a script, and every function scripts can call, library by library. Written by the engine (`scriptReference`, editor/ReferenceDocs.h) from the catalogue scripts are given, and held to it by a test: do not change it by hand. How scripts are written is in the [guide](../guide/scripting.md).

## Functions the engine calls

| Function | When |
|---|---|
| `function Script:start()` | Called once, before the first update: for parents before what hangs under them. What it sets up - `self.count = 0` - is the script's to keep in `self`. |
| `function Script:input(action, pressed)` | Called for every action of the input map that was pressed or released in the frame, with its name and true for pressed: after the starts of the frame, before its timers and updates. |
| `function Script:update(deltaSeconds)` | Called every frame, with the seconds that have passed since the frame before. |
| `function Script:collisionBegan(other, contact)` | Called when the body of the entity has begun to touch that of another, right after the step of the simulation in which it did. `contact` holds `position`, where the two touch in the world; `normal`, which way the other lies from there; and `speed`, how fast they were closing in, in metres per second. |
| `function Script:collisionEnded(other)` | Called when the two bodies have parted, or one of them is gone: `other` may no longer be valid. |
| `function Script:triggerEntered(other, contact)` | Called when a body has moved into a trigger: for the entity that has the trigger, and for the one that moved into it. |
| `function Script:triggerLeft(other)` | Called for both when the body has left the trigger again, or one of the two is gone. |
| `function Script:destroy()` | Called when the script ends while its entity is still there: its Script was taken off or changed, a script destroyed the entity, or another scene is loaded. |

## cordite.log

| Function | What it does |
|---|---|
| `debug(...)` | Writes its arguments to the log, side by side, as a message for whoever is looking for a mistake: it is left out unless the log is asked for such messages. |
| `info(...)` | Writes its arguments to the log, side by side. print() does the same. |
| `warn(...)` | Writes its arguments to the log, side by side, as a warning: something that should not be so. |
| `error(...)` | Writes its arguments to the log, side by side, as an error. The script goes on; Lua's own error() is what stops it. |

## cordite

| Function | What it does |
|---|---|
| `cordite.vec3(x, y, z) -> Vec3` | A vector: a table with x, y and z, which add, subtract, scale and compare like numbers. What is left out is 0. |
| `cordite.quat(x, y, z, w) -> Quat` | A rotation from its four numbers, brought to length one. Without numbers, the rotation that turns nothing. |
| `cordite.euler(x, y, z) -> Quat` | The rotation of three angles in degrees - about X, about Y, about Z - as the inspector of the editor shows them: Y is applied first, then X, then Z. Takes a vector of the three as well. |
| `cordite.axisAngle(axis, degrees) -> Quat` | The rotation by an angle in degrees about an axis, counter-clockwise for whoever looks against the axis. |
| `cordite.lookRotation(direction, up) -> Quat` | The rotation that makes forward (-Z) point along a direction, with up (+Y) as near to `up` as that allows; `up` is the world's when left out. |

## cordite.Vec3

| Function | What it does |
|---|---|
| `a + b -> Vec3` | The sum of two vectors. |
| `a - b -> Vec3` | The difference of two vectors. |
| `a * b -> Vec3` | A vector scaled by a number, or two vectors multiplied component by component. |
| `a / b -> Vec3` | A vector divided by a number, or two vectors divided component by component. |
| `-a -> Vec3` | The vector that points the other way. |
| `a == b -> boolean` | Whether two vectors have the same three numbers, exactly. |
| `tostring(a) -> string` | What print() shows for a vector: (x, y, z). |
| `a:length() -> number` | How long the vector is. |
| `a:normalized() -> Vec3` | The vector of length one that points the same way; a vector of no length stays one. |
| `a:dot(b) -> number` | The dot product: how far one vector reaches along the other, times the length of that one. |
| `a:cross(b) -> Vec3` | The cross product: a vector at right angles to both, by the rule of the right hand. |
| `a:distance(b) -> number` | How far it is from one point to the other. |
| `a:lerp(b, amount) -> Vec3` | The point on the way from a to b: a at 0, b at 1, and beyond both outside of that. |

## cordite.Quat

| Function | What it does |
|---|---|
| `q * r -> Quat, q * v -> Vec3` | Two rotations in one - r first, then q - or a vector turned by the rotation. |
| `q == r -> boolean` | Whether two rotations have the same four numbers, exactly. |
| `tostring(q) -> string` | What print() shows for a rotation: (x, y, z, w). |
| `q:inverse() -> Quat` | The rotation that undoes this one. |
| `q:slerp(r, amount) -> Quat` | The rotation on the shortest way from q to r: q at 0, r at 1, at an even speed between. |
| `q:euler() -> Vec3` | The three angles in degrees that cordite.euler() makes this rotation of. |
| `q:forward() -> Vec3` | Where forward (-Z) points once turned by the rotation. |
| `q:right() -> Vec3` | Where right (+X) points once turned by the rotation. |
| `q:up() -> Vec3` | Where up (+Y) points once turned by the rotation. |

## cordite.Entity

| Function | What it does |
|---|---|
| `entity:isValid() -> boolean` | Whether the entity is still there. Everything else that is asked of an entity which is gone is an error. |
| `entity:id() -> string` | The id of the entity: the one thing about it that stays the same when the scene is saved and loaded again. |
| `entity:name() -> string` | The name of the entity, which need not be its alone. |
| `entity:setName(name)` | Gives the entity another name. |
| `entity:isEnabled() -> boolean` | Whether the entity itself is switched on. One that is may still be off because something it hangs under is. |
| `entity:setEnabled(enabled)` | Switches the entity on or off, and with it everything below it: what is off is not drawn, not simulated, and its scripts are not called. |
| `entity:destroy()` | Destroys the entity and everything below it once the scripts of this frame have run. Until then it is as valid as ever; the scripts in there have their `destroy` called first. |
| `entity:script() -> table or nil` | The table that the entity's script keeps for it - what the script calls `self` - through which one script calls the functions of another and reads what it knows. nil when the entity runs no script. |
| `tostring(entity) -> string` | What print() shows for an entity: its name, or that it is gone. |
| `entity:parent() -> Entity or nil` | What the entity hangs under; nil for a root. |
| `entity:setParent(parent, keepWorldTransform)` | Hangs the entity under another, or under nothing for nil. It stays where it is in the world unless `keepWorldTransform` is false, in which case it keeps its own position, rotation and scale and moves with what it now hangs under. An entity cannot hang under itself, nor under what hangs under it. |
| `entity:children() -> {Entity}` | What hangs under the entity, in order. |
| `entity:position() -> Vec3` | Where the entity is under what it hangs under, in metres. A copy: changing it moves nothing until it is given to setPosition(). |
| `entity:setPosition(position)` | Puts the entity at a place under what it hangs under. |
| `entity:rotation() -> Quat` | How the entity is turned against what it hangs under. |
| `entity:setRotation(rotation)` | Turns the entity against what it hangs under. |
| `entity:scale() -> Vec3` | How large the entity is along its three axes, against what it hangs under. |
| `entity:setScale(scale)` | Makes the entity that large along its three axes. |
| `entity:worldPosition() -> Vec3` | Where the entity is in the world, with everything it hangs under as it is at this moment. |
| `entity:setWorldPosition(position)` | Puts the entity at a place in the world, whatever it hangs under. |
| `entity:worldRotation() -> Quat` | How the entity is turned in the world, with everything it hangs under as it is at this moment. |
| `entity:forward() -> Vec3` | The direction the entity faces in the world: its -Z, of length one. |
| `entity:right() -> Vec3` | The entity's right in the world: its +X, of length one. |
| `entity:up() -> Vec3` | The entity's up in the world: its +Y, of length one. |
| `entity:translate(offset)` | Moves the entity by an offset, in the directions of what it hangs under. |
| `entity:rotate(rotation)` | Turns the entity further, about its own axes: its rotation times the one given. |
| `entity:lookAt(point, up)` | Turns the entity so that it faces a point of the world, with its up as near to `up` - the world's, when left out - as that allows. |
| `entity:has(component) -> boolean` | Whether the entity has a component of that name: "Light", "RigidBody". |
| `entity:get(component, property) -> table, value or nil` | What a component of the entity holds: all its properties as a table, or the one that is named. A copy - changing it changes nothing until it is given to set(). Vectors are Vec3, rotations Quat, and an entity that a property refers to is that entity. nil when the entity has no such component. |
| `entity:set(component, values) or entity:set(component, property, value)` | Changes properties of a component of the entity: those in a table of names, or the one that is named. What is written is checked as the editor checks it - kind, range, whether it may be changed at all - and a change that is refused changes nothing. A table that get() gave can be given back with changes: what has the value it had is no change, also where it may not be changed. |
| `entity:add(component, values)` | Gives the entity a component of that name, with the properties in `values` and the rest as a new component has them. An error when it has one already. |
| `entity:remove(component)` | Takes the component of that name off the entity, if it has one. What every entity has - its Transform - stays. |
| `entity:addForce(force)` | Pushes the body of the entity at its centre with a force, in newtons, for the next step of the simulation; a script that pushes on calls this every frame. Only a dynamic body is moved by it. An error for an entity without a body. |
| `entity:addImpulse(impulse)` | Knocks the body of the entity at its centre, in newton seconds: its speed changes at once, by the impulse divided by its mass. Only a dynamic body is moved by it. |
| `entity:addTorque(torque)` | Twists the body of the entity about its centre, in newton metres about each axis, for the next step of the simulation. Only a dynamic body is turned by it. |

## cordite.world

| Function | What it does |
|---|---|
| `cordite.world.find(name) -> Entity or nil` | The first entity of that name, in the order of the hierarchy; nil when there is none. |
| `cordite.world.findById(id) -> Entity or nil` | The entity with that id, as entity:id() gives it; nil when there is none. |
| `cordite.world.create(name, parent) -> Entity` | Makes an entity, at once. With a parent it hangs under that, where the parent is; without, it is a root. |
| `cordite.world.instantiate(prefab, options) -> Entity` | Places a prefab, at once: copies of its entities, with ids of their own, and their scripts start with the next frame. Returns the root of them, which a PrefabInstance component marks. The prefab is named by where its file is among the assets, "prefabs/crate.prefab.json", or by its id. The options are a table, each of them optional: `parent` - the entity the root hangs under, a root of the scene without -, `name` for the root, and `position` and `rotation`, which put and turn the root relative to its parent instead of where and how the prefab has it. An error for what is no prefab, for a prefab that cannot be read, and for an option there is none of. |
| `cordite.world.roots() -> {Entity}` | The entities that hang under nothing, in their order. |
| `cordite.world.withComponent(component) -> {Entity}` | Every entity that has a component of that name - "Light", "Script" - in the order of the hierarchy. |
| `cordite.world.componentTypes() -> {string}` | The name of every kind of component there is, in the order the engine knows them: what has(), get() and withComponent() are asked with. |

## cordite.time

| Function | What it does |
|---|---|
| `cordite.time.seconds() -> number` | How long the scene has been running, in seconds: the time of every frame since it was loaded, this one included. |
| `cordite.time.delta() -> number` | The time that passes with this frame, in seconds: what `update` is called with. |
| `cordite.time.frame() -> integer` | Which frame of the scene this is, counted from 1 when it was loaded. |
| `cordite.time.after(seconds, callback) -> timer` | Has a function called once, so many seconds from now, with the table of the script - so that a function of the script can be given as it is: after(2, self.close). The timer belongs to the entity whose script made it: it is called while that script runs, before its update, and is gone when the script is. Returns a number to cancel() it by. |
| `cordite.time.every(seconds, callback) -> timer` | Has a function called again and again, so many seconds apart, as after() calls it once. It keeps time: a call that comes late does not make the next one late, and what a long frame has missed is not made up for. |
| `cordite.time.cancel(timer)` | Lets a timer or a task go, by the number that after(), every() or run() gave. One that has been called, has ended or never was is nothing to cancel, and neither is nil. |
| `cordite.time.run(task, ...) -> timer` | Runs a function as a task: something that takes its time. It is called at once, with what follows it here, and runs as far as its first wait(); from there it goes on when the time has passed, before the update of the script it belongs to. An error in it is an error of that script. |
| `cordite.time.wait(seconds)` | In a task: stops here and goes on so many seconds later - in the next frame, when no time is given. An error anywhere else. |

## cordite.scene

| Function | What it does |
|---|---|
| `cordite.scene.load(scene)` | Asks for another scene - by where its file is among the assets, "scenes/cellar.scene.json", or by its id. The scene that runs goes on to the end of the frame; then its scripts end, each with its `destroy`, and the next frame begins in the new one. A scene that cannot be loaded is said in the log, and the one that was there stays. An error for what is no scene. |

## cordite.assets

| Function | What it does |
|---|---|
| `cordite.assets.find(reference) -> string or nil` | The id of an asset by where it is - "models/crate.gltf#mesh/0", "builtin#mesh/cube" - which is what a component is given to name the asset; nil when there is none. |

## cordite.input

| Function | What it does |
|---|---|
| `cordite.input.isDown(action) -> boolean` | Whether an action of the input map is held: any key, button or stick that is bound to it. |
| `cordite.input.wasPressed(action) -> boolean` | Whether the action went down in this frame - also when it has come up again already: what is tapped between two frames was pressed, and released. |
| `cordite.input.wasReleased(action) -> boolean` | Whether the action came up in this frame. |
| `cordite.input.axis(axis) -> number` | How far an axis of the input map is pushed, and which way: what its binding that is moved furthest gives, times the scale of that binding. Two keys give -1, 0 or 1. |

## cordite.physics

| Function | What it does |
|---|---|
| `cordite.physics.raycast(origin, direction, reach) -> hit or nil` | The first body a ray hits within `reach` metres - 1000 when left out - as a table: `entity`, whose body it is, `position`, the `normal` of the surface there, which points out of the body, and `distance`, in metres. nil when it hits nothing. Triggers are not hit. Bodies are where the last frame left them. |
| `cordite.physics.sphereCast(origin, direction, radius, reach) -> hit or nil` | The first body a ball of `radius` touches when its middle is moved along a ray, as raycast() gives it: `distance` is how far the middle got, `position` where ball and body touch, and `normal` points from the body to the ball. |
| `cordite.physics.overlapSphere(center, radius) -> {Entity}` | The entities whose bodies reach into a ball, each once, in an order that is the same every time. Triggers are not among them. |
| `cordite.physics.overlapBox(center, size, rotation) -> {Entity}` | The entities whose bodies reach into a box of `size` metres along its three axes, turned by `rotation` about its middle when one is given. |
| `cordite.physics.gravity() -> Vec3` | What pulls at every body, in metres per second squared: (0, -9.81, 0) unless it was changed. |
| `cordite.physics.setGravity(gravity)` | Changes what pulls at every body, from the next step of the simulation on. |

## cordite.audio

| Function | What it does |
|---|---|
| `cordite.audio.play(sound, options) -> voice or nil` | Plays a sound once, without an entity: `sound` is where its file is among the assets - "sounds/shot.wav" - or its id. `options` is a table of any of `bus` ("effects" when left out), `volume`, `pitch`, `loop`, `position` - a vector: heard from there, quieter with distance; without it the same in both ears - and for that `rolloff` ("inverse" or "linear"), `minDistance` and `maxDistance`, as an AudioSource has them. Returns a number to stop it by, or nil when as many sounds as can be are playing. What an entity sounds like is its AudioSource: set its `playing`. |
| `cordite.audio.stop(voice)` | Stops what play() started. Nothing happens for a sound that is over, and for nil. |
| `cordite.audio.isPlaying(voice) -> boolean` | Whether what play() started is still to be heard: not stopped, and not at its end. |
| `cordite.audio.volume(bus) -> number` | How loud everything on a bus is - "effects", "music", "ambience", "voice" or "interface" - or, for "master", everything there is: 1 as it is, 0 not at all. |
| `cordite.audio.setVolume(bus, volume)` | Makes everything on a bus, or with "master" everything, that loud, from 0 to 4: what the sliders of a settings menu do. It stays so when another scene is loaded. |
