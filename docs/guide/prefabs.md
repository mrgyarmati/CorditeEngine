# Prefabs

A prefab is a group of entities saved as an asset - a lamp post with its light, a crate with
its collider and its sound, an enemy with its script - so that it can be placed again and
again, by you or by a script.

## Making one

Build the group in a scene: one entity with everything else below it. Select the top entity
and choose **Entity > Make prefab**, or run `prefab.create`. The group is written to a
`.prefab.json` file among the assets.

## Placing one

- Double-click it in the Assets panel, or choose **Assets > Place prefab**.
- Drag it onto an entity of the Hierarchy to place it under that entity.
- From a script:

```lua
local barrel = cordite.world.instantiate("prefabs/barrel.prefab.json", {
    parent = self.entity,
    position = { 2, 0, -1 },
})
```

Each time, the entities are copied, with ids of their own; whatever in the group referred to
another entity of the group - a joint, a script property - refers to its copy. The top entity
of the copy gets a **PrefabInstance** component that says which prefab it came from.

## Copies, not links

A placed prefab is an ordinary group of entities from then on: change it as you like, and it
is saved with its scene. A prefab that is changed later does **not** change the copies that
were placed before - place them again to get the new version. (Linked prefabs, whose changes
reach every copy, are planned.)
