-- Makes a body hop when an action is pressed, and counts how often it has come down
-- again - in the name of its entity, where an editor that plays the scene shows it.
--
-- A hop is a knock: `addImpulse` changes how fast a body moves at once, by the impulse
-- divided by the mass of the body. What the body meets and parts from, the simulation
-- tells the script after every step: `collisionBegan` with the other entity and how they
-- met, `collisionEnded` with the other entity. Both are called for either of the two.
--
-- Both are heard, each in one of the two ways a script makes a sound. The hop is the
-- AudioSource of the entity, which is played by setting its `playing`: what an entity
-- sounds like is a component, saved with the scene and set in an editor. The landing
-- belongs to no entity - it is where the two met - and is played with
-- `cordite.audio.play`, once, from that place.

local Hop = {
    properties = {
        action = "fire", -- the action of the input map that makes it hop
        impulse = 3.0, -- newton seconds, upwards
        label = "Ball", -- what the name begins with
        landing = "sounds/hop.wav", -- what it lands with: a sound, by where its file is
    },
}

function Hop:start()
    self.touching = 0 -- how many bodies it touches
    self.landings = 0
end

function Hop:input(action, pressed)
    -- Only off something: a body in the air has nothing to push itself off with.
    if action == self.properties.action and pressed and self.touching > 0 then
        self.entity:addImpulse(cordite.vec3(0.0, self.properties.impulse, 0.0))
        -- From its beginning, once: at its end the source says that it is over.
        if self.entity:has("AudioSource") then
            self.entity:set("AudioSource", "playing", true)
        end
    end
end

-- `contact` says where the two touch, which way the other lies from here and how fast
-- they were closing in on each other, in metres a second.
function Hop:collisionBegan(other, contact)
    self.touching = self.touching + 1
    -- Coming down on something, hard enough to count: not resting on it, nor
    -- brushing past its side.
    if contact.normal.y < -0.5 and contact.speed > 0.5 then
        self.landings = self.landings + 1
        self.entity:setName(string.format("%s (%d)", self.properties.label, self.landings))
        -- A thud where they met, lower than the hop, and louder the harder it came
        -- down. Nothing comes back when as many sounds as can be are playing already;
        -- it is not kept here, because nobody is going to stop it.
        cordite.audio.play(self.properties.landing, {
            position = contact.position,
            volume = math.min(contact.speed / 6.0, 1.0),
            pitch = 0.6,
        })
    end
end

function Hop:collisionEnded(other)
    self.touching = self.touching - 1
end

return Hop
