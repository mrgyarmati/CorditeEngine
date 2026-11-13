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
- Drag it onto an entity of the Hierarchy to place it under that entity, or onto the
  Viewport to place it where you let go.
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

## Linked to their prefab

Every entity of a placed prefab gets a **PrefabLink** component that says which entity of the
prefab it is a copy of. When you change the prefab - make it again from an entity with
**overwrite**, or edit its file - every scene you open, and the open scene as soon as the
editor notices, takes the change: values, components, and entities the prefab gained or lost.

What you change in a placed prefab is kept as its own. The editor notes it in the
PrefabLink's **overrides** - a property (`Light.intensity`), a component you added (`+Light`)
or removed (`-Light`), a new `name`, `enabled` - and the prefab does not change it any more.
Where the top entity stands, and its name, are always its own, and so is an entity you add to
a placed prefab. To give an entity back what the prefab has, run `prefab.revert` - for one
override, or for all of them; on the top entity it reverts the whole group. All of this is
undoable.

To make what a placed prefab has of its own the prefab's, run `prefab.apply` - for one
override, or for all of them; on the top entity it applies the whole group, with the entities
you added to it. The prefab's file is written, and every placed copy of it in the scene takes
the change. Undo takes back what changed in the scene; the file stays as it was written.

Two things are not there yet: a prefab placed inside another cannot be applied, and an entity
of the prefab that you delete from a placed copy comes back the next time the copy is brought
up to date.

A scene still saves every entity in full. It opens even when a prefab is missing - the editor
says so in the console - and a game plays it as it was saved: save the scene after a prefab
changed. What a script places while the game runs is not followed.
