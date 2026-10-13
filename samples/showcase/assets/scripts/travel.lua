-- Leaves the scene for another when an action is pressed.
--
-- `cordite.scene.load` asks for a scene by where its file is among the assets. Nothing
-- happens at once: the scene that runs goes on to the end of the frame. Then its scripts
-- end - each has its `destroy` called, while its entity is still there - and the next
-- frame begins in the new scene, whose scripts start from nothing. What a script knew
-- is gone with the scene it knew it in.

local Travel = {
    properties = {
        to = "", -- the scene to go to, by its path among the assets
        action = "travel", -- the action of the input map that sets off
    },
}

function Travel:input(action, pressed)
    if action == self.properties.action and pressed then
        cordite.log.info("leaving for " .. self.properties.to)
        cordite.scene.load(self.properties.to)
    end
end

return Travel
