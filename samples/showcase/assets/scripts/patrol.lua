-- Walks a character back and forth: from where it stands when the game starts to a
-- place `to` away from there, and back again, with a rest at either end - until whoever
-- plays takes it over, which the controls of the project let them do at any time.
--
-- The script moves nothing itself. Its entity has a CharacterController, which the
-- simulation walks the way its `movement` says: a property that no scene holds, and that
-- a script sets like any other property of any other component. Scripts run before the
-- simulation in every frame, so what is decided here is walked in the same frame.
--
-- The controls are asked by the names the input map of the project gives them
-- (input/controls.input.json): how far "moveX" and "moveY" are pushed, whether "jump"
-- was pressed. Which keys and sticks those are, a script never learns.
--
-- Vectors are tables with x, y and z that add and scale like numbers. Wherever one is
-- expected, three numbers in a row will do - which is how `to` is written here and in
-- the scene, since the properties of a script are plain data.

local Patrol = {
    properties = {
        to = { 0.0, 0.0, -2.0 }, -- where it walks to, in metres from where it starts
        speed = 1.0, -- metres a second
        rest = 1.0, -- seconds it stands at either end
        patience = 4.0, -- seconds it tries to get there before it turns round regardless
        jump = 4.0, -- metres a second it leaves the ground with when it is told to jump
    },
}

function Patrol:start()
    self.from = self.entity:worldPosition()
    self.target = self.from + self.properties.to
    self.resting = 0.0
    self.underway = 0.0
end

-- Turns round: where it came from is where it goes next. A function of the script's
-- own, which the engine never calls.
function Patrol:turn()
    self.from, self.target = self.target, self.from
    self.resting = self.properties.rest
    self.underway = 0.0
    self.entity:set("CharacterController", "movement", cordite.vec3())
end

-- Called for every action of the input map that is pressed or released in a frame,
-- before the update of that frame: what happens once belongs here, what lasts into
-- `update`.
function Patrol:input(action, pressed)
    if action == "jump" and pressed then
        self.entity:set("CharacterController", "jump", self.properties.jump)
    end
end

function Patrol:update(deltaSeconds)
    -- Whoever plays has the say: +X is to the right, and forward is -Z. A stick that is
    -- pushed into a corner must not make it faster than one that is pushed straight.
    local steer = cordite.vec3(cordite.input.axis("moveX"), 0.0, -cordite.input.axis("moveY"))
    if steer:length() > 0.0 then
        if steer:length() > 1.0 then
            steer = steer:normalized()
        end
        self.entity:set("CharacterController", "movement", steer * self.properties.speed)
        -- Let go of, it takes up its beat from wherever it has been walked to.
        self.resting = 0.0
        self.underway = 0.0
        return
    end

    if self.resting > 0.0 then
        self.resting = self.resting - deltaSeconds
        return
    end

    local way = self.target - self.entity:worldPosition()
    way.y = 0.0 -- it walks: how high it stands is for the ground to say
    self.underway = self.underway + deltaSeconds
    if way:length() < 0.05 or self.underway > self.properties.patience then
        self:turn()
    else
        self.entity:set("CharacterController", "movement", way:normalized() * self.properties.speed)
    end
end

return Patrol
