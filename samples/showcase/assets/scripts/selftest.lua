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
end

-- What takes a frame to show is looked at in the frames after the first: a script that
-- was added, an entity that was destroyed. Entities are updated from the top of the
-- hierarchy down, so this script runs before the one on the entity below it.
function SelfTest:update(deltaSeconds)
    self.age = self.age + 1
    if self.age == 2 then
        local clock = self.scratch:script()
        self:expect("another script", clock ~= nil and clock.entity == self.scratch)
        self:expect("the properties of another script", clock.properties.label == "Scratch")
        self:expect("the next frame", cordite.time.frame() == self.frame + 1)
        self:expect("the time of a frame", deltaSeconds == cordite.time.delta())
    elseif self.age == 3 then
        -- By now the clock has started and run once: what it keeps can be read.
        self:expect("what another script knows", self.scratch:script().ticks == 0)
        self.scratch:destroy()
        self:expect("an entity that is to be destroyed", self.scratch:isValid())
    elseif self.age == 4 then
        self:expect("an entity that is gone", not self.scratch:isValid() and not self.first:isValid())
        self:expect("an entity that is gone, as text", tostring(self.scratch) == "Entity(gone)")
        cordite.log.info(string.format("self-test: %d checks passed", self.checked))
    end
end

return SelfTest
