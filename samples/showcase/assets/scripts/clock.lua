-- The clock of the showcase: counts the time the scene has been running, and writes it
-- into the name of its entity - where the hierarchy of an editor that plays the scene
-- shows it going.
--
-- A script is a file that returns a table: the functions the engine calls for every
-- entity whose Script component names the file, and the properties such a component
-- may give other values. The engine makes a table for each of those entities - `self`
-- below - that holds `entity`, the entity it runs for, `properties`, and whatever the
-- script keeps in it. Scripts run while a game runs; an editor shows the scene and
-- runs none.
--
-- Names that are not `local` belong to this file alone, and a name that was never
-- given a value is an error, not nil: a slip of the pen is found where it is made.

local Clock = {
    -- What the script can be told for one entity. A Script component gives values to
    -- the names it wants to be other than here, of the same kind: a number for a
    -- number, text for text.
    properties = {
        interval = 1.0, -- seconds between two changes of the name
        label = "Clock", -- what the name begins with
    },
}

-- Once, before the first update.
function Clock:start()
    self.elapsed = 0.0
    self.ticks = 0
end

-- Every frame, with the time that has passed since the last, in seconds.
function Clock:update(deltaSeconds)
    self.elapsed = self.elapsed + deltaSeconds
    local ticks = self.elapsed // self.properties.interval
    if ticks ~= self.ticks then
        self.ticks = ticks
        self.entity:setName(string.format("%s (%d)", self.properties.label, ticks))
    end
end

-- When the script is taken off its entity, or a script destroys the entity.
function Clock:destroy()
    cordite.log.info(self.entity:name() .. " stopped")
end

return Clock
