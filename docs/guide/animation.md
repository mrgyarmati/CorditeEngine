# Animation

Characters and machines that bend are **skinned meshes**: meshes whose vertices follow the
joints of a **skeleton**, posed by **animation clips**. All three come from glTF files - export
your animated model from Blender, Maya or any other tool as `.glb` or `.gltf`, with its
skeleton (armature) and its animations, and put it into the assets.

## Placing an animated model

Place the model in the scene - double-click it in the Assets panel, drag it onto an entity of
the hierarchy, or `asset.instantiate`. What the editor makes of it:

- one entity for the model;
- under it, one entity with an **Animator** for each skeleton of the file ("Skeleton");
- under that, the skinned meshes, each a MeshRenderer.

Joints are not entities: a skeleton is an asset, and its pose is worked out by the animator.
Select the animator's entity to see the skeleton drawn in the viewport.

## The Animator

| Property | Meaning |
|----------|---------|
| `skeleton` | The skeleton that is posed. |
| `clip` | The animation that plays. Clips are found inside the model file in the Assets panel. |
| `time` | Where in the clip the pose is taken from, in seconds. Setting it shows that moment. |
| `speed` | 1 as made, 2 twice as fast, negative backwards. |
| `loop` | Start again at the end, or stay in the last pose. |
| `playing` | Whether time moves on. |
| `blendClip`, `blendTime`, `blendWeight` | A second clip laid over the first - a run over a walk - by `blendWeight`, from 0 (none of it) to 1 (nothing but it). |

A clip finds its joints by name: any clip fits any skeleton whose joints have the names it
animates, so animations can be shared between characters with the same rig.

While you edit, the animators do not play: they show the pose at their `time`, which you can
set to look at a moment of a clip. Played, they move on. Scripts change clips, speeds and
blends like any other property:

```lua
-- On the model's entity, which walks as a character; the Animator is below it.
local Guard = {}

function Guard:start()
    for _, child in ipairs(self.entity:children()) do
        if child:has("Animator") then
            self.animator = child
        end
    end
end

function Guard:update(deltaSeconds)
    -- Walk when slow, run - the blend clip - when fast.
    local speed = self.entity:get("CharacterController", "velocity"):length()
    self.animator:set("Animator", "blendWeight", math.min(speed / 4.0, 1.0))
end

return Guard
```

## What animation does not do yet

More than two clips at once (layers, blend trees, state machines), motion taken from a clip
(root motion), objects that follow a joint (a sword in a hand stays where the hand rests), and
inverse kinematics. The README lists what is planned.
