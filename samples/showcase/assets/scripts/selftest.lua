-- Checks, in the running game, that what scripts are given does what it says. Everything
-- a script can call is called here, in the first frames of the game, and what comes back
-- is compared with what should. The first thing that is not as expected is an error,
-- which stops this script and is a problem of the scene.
--
-- It is the showcase's test of the script API - the `samples` test fails when scripts are
-- given a function that no script of the showcase calls - and the place to look up how
-- something is written. For how a script of a game is put together, read clock.lua and
-- patrol.lua first.
--
-- What it makes, it makes under its own entity and takes away again: when it has run,
-- the scene is as it was.

local SelfTest = {}

-- A shorter name for what is written often.
local vec3 = cordite.vec3

-- Whether two vectors, or two rotations, are the same but for what the single floats of
-- the engine lose. Scripts compute with more digits than the engine keeps: what was once
-- in a component is compared like this, not with ==.
local function near(a, b, tolerance)
    for _, name in ipairs({ "x", "y", "z", "w" }) do
        if math.abs((a[name] or 0.0) - (b[name] or 0.0)) > (tolerance or 1.0e-5) then
            return false
        end
    end
    return true
end

-- Counts a comparison, and stops the script with what it was about when it does not
-- hold. The 2 makes the error name the line that asked, not this one.
function SelfTest:expect(what, holds)
    self.checked = self.checked + 1
    if not holds then
        error("self-test: " .. what .. " is not as expected", 2)
    end
end

-- Vectors are tables with x, y and z. cordite.vec3 makes one that computes; where the
-- engine expects a vector, a table written by hand will do.
function SelfTest:vectors()
    local a, b = vec3(1, 2, 3), vec3(4, 5, 6)
    self:expect("a vector", a.x == 1 and a.y == 2 and a.z == 3 and vec3() == vec3(0, 0, 0))
    self:expect("a sum", a + b == vec3(5, 7, 9))
    self:expect("a difference", b - a == vec3(3, 3, 3))
    self:expect("a product with a number", a * 2 == vec3(2, 4, 6) and 2 * a == a * 2)
    self:expect("a product with a vector", a * b == vec3(4, 10, 18))
    self:expect("a quotient", b / 2 == vec3(2, 2.5, 3))
    self:expect("the other way", -a == vec3(-1, -2, -3))
    self:expect("a table as a vector", a + { 1, 1, 1 } == vec3(2, 3, 4))
    self:expect("a table of names as a vector", a - { x = 1, y = 2, z = 3 } == vec3())
    self:expect("a vector as text", tostring(a) == "(1.0, 2.0, 3.0)")
    self:expect("a length", vec3(3, 4, 0):length() == 5)
    self:expect("a direction", vec3(0, 0, 9):normalized() == vec3(0, 0, 1))
    self:expect("a dot product", a:dot(b) == 32)
    self:expect("a cross product", vec3(1, 0, 0):cross(vec3(0, 1, 0)) == vec3(0, 0, 1))
    self:expect("a distance", a:distance(b) == (b - a):length())
    self:expect("a point on the way", a:lerp(b, 0.5) == vec3(2.5, 3.5, 4.5))
end

-- Rotations are tables with x, y, z and w, made from angles in degrees, from an axis and
-- an angle, or from a direction to face. Forward is -Z, right +X, up +Y.
function SelfTest:rotations()
    local still = cordite.quat()
    local left = cordite.euler(0, 90, 0) -- a quarter turn about +Y: to the left
    self:expect("the rotation that turns nothing", still == cordite.quat(0, 0, 0, 1))
    self:expect("a rotation as text", tostring(still) == "(0.0, 0.0, 0.0, 1.0)")
    self:expect("where a turn faces", near(left:forward(), vec3(-1, 0, 0)))
    self:expect("the right of a turn", near(left:right(), vec3(0, 0, -1)))
    self:expect("the up of a turn", near(left:up(), vec3(0, 1, 0)))
    self:expect("a vector that is turned", near(left * vec3(0, 0, -1), vec3(-1, 0, 0)))
    self:expect("two turns in one", near((left * left):forward(), vec3(0, 0, 1)))
    self:expect("a turn and the turn back", near((left * left:inverse()):forward(), vec3(0, 0, -1)))
    self:expect("half a turn", near(still:slerp(left, 0.5):forward(), cordite.euler(0, 45, 0):forward()))
    self:expect("the angles of a rotation", near(cordite.euler(10, 20, 30):euler(), vec3(10, 20, 30), 1.0e-3))
    self:expect("an axis and an angle", near(cordite.axisAngle({ 0, 1, 0 }, 90):forward(), left:forward()))
    self:expect("a direction to face", near(cordite.lookRotation({ 5, 0, 0 }):forward(), vec3(1, 0, 0)))
end

-- What a script is handed for a component is a copy, with the vectors and the entities
-- in it as scripts know them, and handing it back writes what is other than it was. Here
-- every component of every entity of the scene goes to the script and back: none may be
-- refused, and - the test of the showcase sees to that - none may be another afterwards.
function SelfTest:everyComponent()
    local handed = 0
    for _, kind in ipairs(cordite.world.componentTypes()) do
        for _, entity in ipairs(cordite.world.withComponent(kind)) do
            entity:set(kind, entity:get(kind))
            handed = handed + 1
        end
    end
    self:expect("a scene with components in it", handed > 0)
end

-- Finding entities, making them, and what an entity says of itself.
function SelfTest:entities()
    local stage = cordite.world.find("Stage")
    self:expect("an entity by its name", stage ~= nil and stage:name() == "Stage")
    self:expect("a name that nothing has", cordite.world.find("No such thing") == nil)
    self:expect("an entity by its id", #stage:id() == 36 and cordite.world.findById(stage:id()) == stage)
    local roots = cordite.world.roots()
    self:expect("the roots of the scene", #roots > 0 and roots[1]:parent() == nil)

    local scripted = false
    for _, entity in ipairs(cordite.world.withComponent("Script")) do
        scripted = scripted or entity == self.entity
    end
    self:expect("the entities that have a component", scripted)

    local scratch = cordite.world.create("Scratch", self.entity)
    self:expect("a new entity", scratch:isValid() and scratch:name() == "Scratch")
    self:expect("an entity as text", tostring(scratch) == "Entity(Scratch)")
    scratch:setName("Scratch pad")
    scratch:setEnabled(false)
    self:expect("an entity that is switched off", scratch:name() == "Scratch pad" and not scratch:isEnabled())
    scratch:setEnabled(true)
    self.scratch = scratch
end

-- What hangs under what.
function SelfTest:hierarchy()
    local scratch = self.scratch
    local first = cordite.world.create("First", scratch)
    local second = cordite.world.create("Second", scratch)
    self:expect("a parent", first:parent() == scratch and scratch:parent() == self.entity)
    self:expect("how many children", #scratch:children() == 2)
    self:expect("children in their order", scratch:children()[1] == first and scratch:children()[2] == second)
    second:setParent(first)
    self:expect("another parent", second:parent() == first and #scratch:children() == 1)
    second:setParent(nil)
    self:expect("no parent at all", second:parent() == nil)
    second:setParent(scratch)
    self.first = first
end

-- Where an entity is: under what it hangs under, and in the world.
function SelfTest:transforms()
    local scratch, first = self.scratch, self.first
    scratch:setPosition(vec3(1, 2, 3))
    scratch:setRotation(cordite.euler(0, 90, 0))
    scratch:setScale({ 2, 2, 2 })
    self:expect("a position", scratch:position() == vec3(1, 2, 3))
    self:expect("a scale", scratch:scale() == vec3(2, 2, 2))
    self:expect("a rotation", near(scratch:rotation():forward(), vec3(-1, 0, 0)))

    -- A metre in front of a parent that is turned and twice as large is two metres from
    -- it in the world, the way the parent faces.
    first:setPosition({ 0, 0, -1 })
    local ahead = scratch:worldPosition() + scratch:forward() * 2
    self:expect("a place in the world", near(first:worldPosition(), ahead))
    self:expect("a turn in the world", near(first:worldRotation():forward(), scratch:forward()))
    self:expect("the directions of an entity", near(scratch:right():cross(scratch:up()), -scratch:forward()))
    first:setWorldPosition(vec3(0, 5, 0))
    self:expect("an entity put somewhere in the world", near(first:worldPosition(), vec3(0, 5, 0)))

    scratch:translate(vec3(0, 1, 0))
    self:expect("a step", scratch:position() == vec3(1, 3, 3))
    scratch:rotate(cordite.euler(0, 90, 0))
    self:expect("a turn further", near(scratch:rotation():forward(), vec3(0, 0, 1)))
    scratch:lookAt(scratch:worldPosition() + vec3(0, 0, -5))
    self:expect("an entity that faces a point", near(scratch:forward(), vec3(0, 0, -1)))
end

-- Components are read and written by the names of their properties, and what is written
-- is checked as the editor checks it.
function SelfTest:components()
    local scratch = self.scratch
    self:expect("an entity without a light", not scratch:has("Light") and scratch:get("Light") == nil)
    self:expect("what every entity has", scratch:has("Transform"))
    scratch:add("Light", { type = "point", color = { 1, 0.5, 0.25 }, intensity = 2 })
    self:expect("a component that was added", scratch:has("Light"))
    self:expect("one property of a component", scratch:get("Light", "type") == "point")

    scratch:set("Light", "intensity", 3) -- one property
    scratch:set("Light", { range = 4, color = vec3(0, 1, 0) }) -- several
    local light = scratch:get("Light") -- all of them, as a table of the script's own
    light.intensity = light.intensity * 2
    scratch:set("Light", light) -- and back again
    self:expect("a property that was changed", scratch:get("Light", "intensity") == 6)
    self:expect("properties that were left alone", scratch:get("Light", "range") == 4)
    self:expect("a color as a vector", scratch:get("Light", "color") == vec3(0, 1, 0))

    -- What is wrong is refused, and nothing of it is written. pcall gives false and
    -- what was wrong instead of stopping the script.
    local written, refusal = pcall(scratch.set, scratch, "Light", { intensity = -1, range = 9 })
    self:expect("a value that is refused", not written and string.find(refusal, "intensity", 1, true) ~= nil)
    self:expect("a change of which nothing was made", scratch:get("Light", "range") == 4)

    scratch:remove("Light")
    self:expect("a component that was taken off", not scratch:has("Light"))

    -- A property that refers to an entity is that entity.
    local bracket = cordite.world.find("Bracket")
    self:expect("a reference to an entity", bracket:get("Joint", "connectedTo") == cordite.world.find("Beam"))
end

-- The time, the assets, and other scripts.
function SelfTest:timeAndAssets()
    self:expect("a frame", cordite.time.frame() >= 1 and cordite.time.delta() >= 0)
    self:expect("the time", cordite.time.seconds() >= cordite.time.delta())
    self.frame = cordite.time.frame()

    local clock = cordite.assets.find("scripts/clock.lua")
    self:expect("an asset by where it is", clock ~= nil and #clock == 36)
    self:expect("a file that is not there", cordite.assets.find("scripts/none.lua") == nil)
    self:expect("an asset inside a file", cordite.assets.find("builtin#mesh/cube") ~= nil)

    -- A Script is a component like the others. The script runs from the next frame on;
    -- until then nothing answers for the entity.
    self.scratch:add("Script", { script = clock, properties = { label = "Scratch" } })
    self:expect("the table of this script", self.entity:script() == self)
    self:expect("a script that was only just added", self.scratch:script() == nil)

    -- Another scene is asked for by where its file is among the assets, and what is no
    -- scene is refused. To see one loaded, press "travel": scripts/travel.lua, on the
    -- entity "Way out", asks for the annex - and this scene, this script with it, ends.
    self:expect("a scene that is none", not pcall(cordite.scene.load, "scripts/clock.lua"))
end

-- The controls of the game, by the names the input map of the project gives them.
-- Whoever plays may be holding any of them as the game starts, so what is looked at here
-- is the kind of answer; that the answers are the right ones is for the tests of the
-- engine, which press keys.
function SelfTest:controls()
    self:expect("whether an action is held", type(cordite.input.isDown("jump")) == "boolean")
    self:expect("whether an action was pressed", type(cordite.input.wasPressed("fire")) == "boolean")
    self:expect("whether an action was released", type(cordite.input.wasReleased("fire")) == "boolean")
    local push = cordite.input.axis("moveX")
    self:expect("how far an axis is pushed", push >= -1 and push <= 1)
    -- A name the input map does not have is a mistake, not a control nobody touches.
    self:expect("a control that there is not", not pcall(cordite.input.isDown, "no such control"))
end

-- The simulation: what a ray hits, what reaches into a ball or a box, what pulls at
-- everything, and a push. The bodies of a scene are made when the simulation first
-- moves on, at the end of a frame - so in the frame a game starts with there are none
-- to find, and this is looked at one frame later.
function SelfTest:simulation()
    local floor = cordite.world.find("Floor")
    -- Straight down onto a corner of the floor, where nothing stands.
    local above = vec3(-8, 5, 8)
    local hit = cordite.physics.raycast(above, vec3(0, -1, 0), 20)
    self:expect("what a ray hits", hit ~= nil and hit.entity == floor)
    self:expect("where a ray hits", near(hit.position, vec3(-8, 0, 8), 1.0e-3))
    self:expect("which way a surface faces", near(hit.normal, vec3(0, 1, 0), 1.0e-3))
    self:expect("how far a ray goes", math.abs(hit.distance - 5) < 1.0e-3)
    self:expect("a ray that hits nothing", cordite.physics.raycast(above, vec3(0, 1, 0), 20) == nil)

    -- A ball of half a metre touches the floor half a metre sooner.
    local touched = cordite.physics.sphereCast(above, vec3(0, -1, 0), 0.5, 20)
    self:expect("what a ball touches", touched ~= nil and touched.entity == floor)
    self:expect("how far a ball gets", math.abs(touched.distance - 4.5) < 1.0e-3)

    local reached = cordite.physics.overlapSphere(vec3(-8, 0, 8), 0.5)
    self:expect("what reaches into a ball", #reached == 1 and reached[1] == floor)
    reached = cordite.physics.overlapBox(vec3(-8, 2, 8), vec3(1, 1, 1), cordite.euler(0, 45, 0))
    self:expect("a box that nothing reaches into", #reached == 0)

    -- Gravity is handed back as it was found: the scene is to stay as it is.
    local gravity = cordite.physics.gravity()
    self:expect("what pulls at everything", gravity.y < 0)
    cordite.physics.setGravity(gravity)

    -- A push of nothing moves nothing. What it shows is that the ball can be pushed:
    -- an entity without a body cannot, and says so.
    local ball = cordite.world.find("Ball")
    ball:addForce(vec3())
    ball:addImpulse(vec3())
    ball:addTorque(vec3())
    self:expect("an entity without a body", not pcall(self.entity.addImpulse, self.entity, vec3(0, 1, 0)))
end

-- Sound. What an entity sounds like is its AudioSource, a component like the others;
-- what is here is the rest: a sound that belongs to no entity, and how loud the buses
-- are. Everything is played at no volume: a self-test is nothing to listen to.
function SelfTest:sound()
    -- By where its file is among the assets. What comes back is a number to stop it by.
    local round = cordite.audio.play("sounds/click.wav", { bus = "interface", volume = 0, loop = true })
    self:expect("a number for a sound", math.type(round) == "integer")
    self:expect("a sound that is playing", cordite.audio.isPlaying(round))
    cordite.audio.stop(round)
    self:expect("a sound that was stopped", not cordite.audio.isPlaying(round))
    -- By its id, and from a place: quieter with distance, the way its rolloff says.
    local placed = cordite.audio.play(cordite.assets.find("sounds/hop.wav"), {
        volume = 0,
        pitch = 2,
        position = vec3(0, 1, 0),
        rolloff = "linear",
        minDistance = 1,
        maxDistance = 10,
    })
    self:expect("another sound, another number", placed ~= round and cordite.audio.isPlaying(placed))
    cordite.audio.stop(placed)

    -- Every bus has a volume, and "master" is that of everything. It is handed back as
    -- it was found: the game is to stay as loud as its player made it.
    local music = cordite.audio.volume("music")
    cordite.audio.setVolume("music", 0.25)
    self:expect("how loud a bus is", cordite.audio.volume("music") == 0.25)
    cordite.audio.setVolume("music", music)
    self:expect("how loud everything is", cordite.audio.volume("master") >= 0)

    -- What is wrong is refused: what is no sound, an option there is none of, a bus
    -- nobody has heard of, a volume that is none.
    self:expect("a sound that there is not", not pcall(cordite.audio.play, "sounds/no such sound.wav"))
    self:expect("an asset that is no sound", not pcall(cordite.audio.play, "scripts/clock.lua"))
    self:expect("an option that there is not", not pcall(cordite.audio.play, "sounds/click.wav", { loud = 1 }))
    self:expect("a bus that there is not", not pcall(cordite.audio.volume, "noise"))
    self:expect("a volume that is none", not pcall(cordite.audio.setVolume, "music", -1))
end

-- What is to happen later: a function that is called once or again and again, and a
-- task - a function that waits in the middle of what it does. What they have done is
-- looked at in the frames that follow, in `update`.
function SelfTest:timers()
    self.rang = 0
    self.ticked = 0
    -- Called with the table of the script, so that a function of the script can be
    -- given as it is. No time at all is this frame still: timers are called after the
    -- starts of a frame, before its updates.
    cordite.time.after(0, self.ring)
    -- A thousandth of a second is less than any frame: once a frame, from the next on.
    self.ticking = cordite.time.every(0.001, function() self.ticked = self.ticked + 1 end)
    -- A task runs at once, as far as its first wait, with what follows the function.
    self.task = cordite.time.run(function(word)
        self.heard = word
        cordite.time.wait() -- until the next frame
        self.wentOn = true
        cordite.time.wait(3600) -- an hour: it is cancelled long before
        self.overslept = true
    end, "a word")
    self:expect("a task that has begun", self.heard == "a word" and self.wentOn == nil)
    self:expect("numbers to cancel by", math.type(self.ticking) == "integer" and self.task ~= self.ticking)
    -- Only a task can wait: there is nobody to go on with anything else.
    self:expect("a wait outside a task", not pcall(cordite.time.wait, 1))
end

function SelfTest:ring()
    self.rang = self.rang + 1
end

function SelfTest:start()
    self.checked = 0
    self.age = 0
    cordite.log.debug("self-test: begun in frame " .. cordite.time.frame())
    self:vectors()
    self:rotations()
    self:everyComponent()
    self:entities()
    self:hierarchy()
    self:transforms()
    self:components()
    self:timeAndAssets()
    self:controls()
    self:sound()
    self:timers()
end

-- What takes a frame to show is looked at in the frames after the first: a script that
-- was added, an entity that was destroyed. Entities are updated from the top of the
-- hierarchy down, so this script runs before the one on the entity below it.
function SelfTest:update(deltaSeconds)
    self.age = self.age + 1
    if self.age == 1 then
        self:expect("a timer of no time", self.rang == 1)
        self:expect("a timer that repeats, before its time", self.ticked == 0)
    elseif self.age == 2 then
        self:expect("a timer that repeats", self.ticked == 1 and self.rang == 1)
        self:expect("a task that has gone on", self.wentOn == true)
        local clock = self.scratch:script()
        self:expect("another script", clock ~= nil and clock.entity == self.scratch)
        self:expect("the properties of another script", clock.properties.label == "Scratch")
        self:expect("the next frame", cordite.time.frame() == self.frame + 1)
        self:expect("the time of a frame", deltaSeconds == cordite.time.delta())
        self:simulation()
    elseif self.age == 3 then
        self:expect("a timer that repeats, again", self.ticked == 2)
        cordite.time.cancel(self.ticking)
        cordite.time.cancel(self.task)
        -- By now the clock has started and run once: what it keeps can be read.
        self:expect("what another script knows", self.scratch:script().ticks == 0)
        self.scratch:destroy()
        self:expect("an entity that is to be destroyed", self.scratch:isValid())
    elseif self.age == 4 then
        self:expect("a timer that was cancelled", self.ticked == 2)
        self:expect("a task that was cancelled", self.overslept == nil)
        self:expect("an entity that is gone", not self.scratch:isValid() and not self.first:isValid())
        self:expect("an entity that is gone, as text", tostring(self.scratch) == "Entity(gone)")
        cordite.log.info(string.format("self-test: %d checks passed", self.checked))
    end
end

return SelfTest
