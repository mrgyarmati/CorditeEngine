-- Walks a character back and forth: from where it stands when the game starts to a
-- place `to` away from there, and back again, with a rest at either end.
--
-- The script moves nothing itself. Its entity has a CharacterController, which the
-- simulation walks the way its `movement` says: a property that no scene holds, and that
-- a script sets like any other property of any other component. Scripts run before the
-- simulation in every frame, so what is decided here is walked in the same frame.
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

function Patrol:update(deltaSeconds)
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
