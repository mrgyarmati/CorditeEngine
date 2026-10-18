-- Plays the flyby - the sequence of this entity's SequencePlayer - when an action is
-- pressed, and hears what happens in it.
--
-- A sequence plays itself. Its SequencePlayer moves its time on, and its tracks fly the
-- camera below this entity round the stage, close its lens in, make the light on the wall
-- flare and cut the view of the game to that camera and back (sequences/flyby.sequence.json).
-- A script starts it by setting `playing` - from the beginning, with `time` - and is told
-- of its events by `sequenceEvent`, with the names the sequence gives them.

local Cinematic = {
    properties = {
        action = "cinematic", -- the action of the input map that starts the flyby
        shake = 0.6, -- the trauma the camera of the flyby is given at the impact
    },
}

function Cinematic:input(action, pressed)
    -- Once at a time: pressed again while it plays, it plays on.
    if action == self.properties.action and pressed and not self.entity:get("SequencePlayer", "playing") then
        self.entity:set("SequencePlayer", { time = 0.0, playing = true })
    end
end

function Cinematic:sequenceEvent(name)
    if name == "impact" then
        -- What the sequence moves is the camera; what shakes it is this script.
        for _, child in ipairs(self.entity:children()) do
            if child:has("CameraShake") then
                child:set("CameraShake", "trauma", self.properties.shake)
            end
        end
    elseif name == "over" then
        cordite.log.info("the flyby is over")
    end
end

return Cinematic
