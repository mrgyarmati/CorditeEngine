-- Makes a light flicker: it burns steadily for a while, dips a few times in quick
-- succession, and burns on.
--
-- What takes its time is written as a task: a function the engine runs a piece at a
-- time. `cordite.time.wait` stops it where it is, and it goes on from there when the
-- time has passed - in a later frame, before the update of its script. In between, the
-- function simply is where it was: in the middle of a loop, with everything it knew.
-- A task belongs to the entity whose script started it, and ends with the script.

local Flicker = {
    properties = {
        calm = 3.0, -- seconds it burns steadily at least; up to twice as long
        dips = 3, -- how often it dips in a row
        low = 0.3, -- how much of its brightness is left in a dip
    },
}

function Flicker:start()
    self.steady = self.entity:get("Light", "intensity")
    -- What follows the function is what it is called with.
    cordite.time.run(self.burn, self)
end

-- The task. It never comes to an end by itself.
function Flicker:burn()
    while true do
        -- math.random() begins with the same numbers every time the game is run: what
        -- a script does is the same on every run.
        cordite.time.wait(self.properties.calm * (1.0 + math.random()))
        for _ = 1, self.properties.dips do
            self.entity:set("Light", "intensity", self.steady * self.properties.low)
            cordite.time.wait(0.05)
            self.entity:set("Light", "intensity", self.steady)
            cordite.time.wait(0.1)
        end
    end
end

return Flicker
