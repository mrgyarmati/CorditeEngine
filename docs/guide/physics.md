# Physics

Cordite simulates rigid bodies, joints and walking characters with
[Jolt Physics](https://github.com/jrouwe/JoltPhysics). You say with components what is
solid and how it moves; the engine makes the bodies, steps the simulation in fixed steps of
1/60 of a second, and moves the entities. The simulation runs while the game runs - in the
editor while a scene is played - and gives the same result every time.

## What is solid: Collider

A **Collider** component gives its entity a shape:

| `shape` | Size | Use |
|---------|------|-----|
| `box` | `size` (metres along X, Y and Z) | Crates, walls, floors |
| `sphere` | `radius` | Balls, rough approximations |
| `capsule` | `radius`, `height` (from end to end, along Y) | People, pillars, limbs |
| `convexHull` | the `mesh` it is made of | Anything that moves and is not a simple shape |
| `mesh` | the `mesh` it is made of | The level itself: terrain, buildings - things that do not move |

The shape is scaled with the entity, and `center` moves it within the entity. `friction` (0
slides, 1 grips) and `restitution` (0 does not bounce, 1 bounces back fully) say what the
surface is like.

A **mesh** shape is a surface without an inside: a body made of one cannot be moved by the
simulation (it may be static or kinematic). What the simulation moves is a convex hull.

A collider without a rigid body stands still - which is what most of a level is.

## What moves: RigidBody

A **RigidBody** component makes its entity a body that is simulated:

- `motion`: `dynamic` - pushed by forces, gravity and collisions; `kinematic` - moved by you
  (by its transform, or a script) and pushing what is in its way, like a lift or a door;
  `static` - never moves.
- `mass` in kilograms, `linearDamping` and `angularDamping` (air resistance),
  `gravityScale` (0 floats), `continuous` (for fast, small things that must not pass
  through walls), `allowSleep`.
- `linearVelocity` and `angularVelocity`: how fast it moves now; set them to throw something.

A rigid body is made of its own collider and of the colliders of the entities below it that
have no rigid body of their own - a chair can be one body of several boxes.

To put a dynamic body somewhere else, give its entity another position: the body follows.

## Triggers

A collider with `trigger` on is not solid: things pass through it, and scripts are told
when they do (`triggerEntered`, `triggerLeft`). Use triggers for doors that open when the
player comes near, checkpoints, and the end of a level. They are drawn in a color of their
own in the editor (View > Colliders).

## Joints

A **Joint** component holds its entity's body to another one (`connectedTo`), or to the
world when that is empty:

| `type` | What it allows |
|--------|----------------|
| `fixed` | Nothing: the two move as one |
| `ball` | Turning about the anchor in every direction - a chain, a ragdoll shoulder |
| `hinge` | Turning about `axis` - a door, a wheel |
| `slider` | Moving along `axis` - a drawer, a piston |
| `distance` | Keeping a distance - a rope, a spring |

`anchor` is where the joint is, in the space of the entity; `limited` with `minimum` and
`maximum` limits how far it turns (degrees) or slides (metres); `frequency` and `damping`
make it a spring. Joined bodies do not collide with each other unless `collideConnected`.

## Characters: CharacterController

A **CharacterController** makes its entity a character that walks: a capsule (`radius`,
`height`) that is moved through the world by asking what is in its way, not pushed around by
forces. It stands on slopes up to `maxSlope` degrees, climbs steps up to `stepHeight`, and
pushes dynamic bodies with `pushForce`.

You walk it by setting `movement` (metres a second, in the world) and make it jump by setting
`jump` (metres a second upwards) - usually from a script:

```lua
self.entity:set("CharacterController", "movement", direction * 4.0)
self.entity:set("CharacterController", "jump", 5.0)
```

It tells you what happened: `velocity`, `ground` (`onGround`, `onSteepGround`, `inAir`),
`groundNormal` and `groundEntity`. A character in the air falls; `airControl` says how much
say `movement` still has there. The entity is where the character's feet are.

## Asking the simulation

Scripts ask with `cordite.physics.raycast`, `sphereCast`, `overlapSphere` and `overlapBox`,
and are told about contacts with `collisionBegan` and `collisionEnded` (see
[Scripting](scripting.md)). In the editor - also while editing, when nothing falls -
`physics.raycast` and `physics.overlap` answer the same questions for agents and tools.

## Problems

What cannot be simulated is named once in the Console - a box without volume, a mesh shape
on a dynamic body, a joint to an entity without a body - and `scene.validate` finds such
problems without running anything. The rest of the scene runs.
