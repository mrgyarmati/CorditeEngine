-- Lights a light while something stands in the trigger of its entity, and for a moment
-- after it has left.
--
-- A collider that is a trigger stops nothing; the simulation only says what moves into
-- it and out of it: `triggerEntered` and `triggerLeft`, with the other entity. Both are
-- called for either of the two - here for the entity that has the trigger, and just the
-- same for a script on what walks in.

local Post = {
    properties = {
        lit = 3.0, -- how bright the Light of the entity is while something is there
        dark = 0.0, -- and while nothing is
        linger = 0.25, -- seconds it stays lit once the last has left
    },
}

function Post:start()
    -- Several things may be in there at once: it goes dark when the last has left.
    self.inside = 0
    self.entity:set("Light", "intensity", self.properties.dark)
end

function Post:triggerEntered(other)
    self.inside = self.inside + 1
    -- Back before the light went out: it is not to go out now. Cancelling nothing -
    -- no timer was set, or it has been called - is nothing.
    cordite.time.cancel(self.going)
    self.going = nil
    self.entity:set("Light", "intensity", self.properties.lit)
end

function Post:triggerLeft(other)
    self.inside = self.inside - 1
    if self.inside == 0 then
        -- Once, a little later, unless it is cancelled before.
        self.going = cordite.time.after(self.properties.linger, self.darken)
    end
end

function Post:darken()
    self.going = nil
    self.entity:set("Light", "intensity", self.properties.dark)
end

return Post
