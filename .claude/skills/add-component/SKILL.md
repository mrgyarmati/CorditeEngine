---
name: add-component
description: Add a component type or a property to Cordite's entity-component system. Use when gameplay, rendering, physics or audio needs new per-entity data, when a field must become visible in the editor inspector or in scene files, or when the samples test reports that the showcase scene is incomplete.
---

# Add a component or a property

A component is a plain struct. Describing it once with reflection is what makes it
loadable, saveable, editable in the inspector, undoable and scriptable. Nothing else has
to be written for any of those.

## 1. Declare the data

Put the struct in the module that owns the data (`engine/cordite/world/Components.h`
for general scene data; a system-specific component goes in that system's module, which
must then depend on `world`).

```cpp
enum class LightKind : uint8_t {
    Point,
    Spot,
};

/// A light source. It shines from the entity's position, along -Z for a spot light.
struct LightComponent {
    LightKind kind = LightKind::Point;
    /// Linear RGB.
    Vec3 color{1.0f};
    float intensity = 1.0f;
    /// Distance at which the light has faded to nothing, in metres.
    float range = 10.0f;
};
```

Rules:

- Data only: no methods that do work, no pointers to systems, no GPU handles. Default
  member initializers give the values a new component has.
- Members must be copyable and default-constructible.
- State that exists only while the game runs (a physics body handle, a cached matrix)
  either lives in a separate, unregistered component that the system manages, or is a
  property marked `transient`.
- Refer to other entities and to assets by `Uuid`, with the matching `semantic`.

## 2. Describe it

In the module's `Components.cpp` (or equivalent), next to the other descriptions:

```cpp
TypeInfo describeLight() {
    return TypeBuilder<LightComponent>("Light")
        .enumProperty("kind", &LightComponent::kind, {{LightKind::Point, "point"}, {LightKind::Spot, "spot"}},
                      {.description = "The shape of the light"})
        .property("color", &LightComponent::color,
                  {.description = "Linear color of the light", .semantic = PropertySemantic::Color, .minimum = 0.0})
        .property("intensity", &LightComponent::intensity,
                  {.description = "Brightness", .minimum = 0.0, .unit = "cd"})
        .property("range", &LightComponent::range,
                  {.description = "Distance at which the light has faded out", .minimum = 0.01, .unit = "m"})
        .build();
}
```

- The type name (`"Light"`) and the property names are what scene files, scripts and
  agents use. They are part of the file format: changing one breaks saved scenes.
  Names are `camelCase`, enum option names lowercase.
- Every property gets a `description`: it is the tooltip in the inspector and the
  documentation an agent reads.
- Numbers get a `minimum` and/or `maximum` wherever a value outside would be a bug.
  The range is enforced on every write, from files and from tools alike.
- `semantic` picks the editor widget: `Color`, `EntityReference`, `AssetReference`
  (with `referenceType`), `Multiline`.
- `readOnly`: shown, saved and loaded, but tools cannot change it.
  `transient`: runtime state, never saved.
- Property types: `bool`, integers, `float`, `double`, `std::string`, `Uuid`, `Vec2/3/4`,
  `Quat` (include `cordite/math/MathReflection.h`), enums (`enumProperty`), nested
  reflected structs (`objectProperty`), and `std::vector` of the plain ones. A new kind
  of value needs a `PropertyCodec` specialization (see `core/Reflection.h`).

## 3. Register it

In the same file's registration function:

```cpp
CORDITE_TRY(registry.add<LightComponent>(
    describeLight(), {.description = "A light source at the entity's position."}));
```

A module that registers components for the first time adds its
`register<Module>Components` function to `registerEngineComponents`
(`engine/cordite/engine/EngineComponents.cpp`). The order of registration is the order
components appear in scene files and in the inspector.

Pass `.optional = false` only for components every entity has from creation.

## 4. Show it in the showcase scene

Add an entity that uses the component, with every enum option appearing at least once,
to `samples/showcase/showcase.scene.json`. The `samples` test fails until this is done.
Give the entity a fixed, readable id in the style already used there.

## 5. Test it

- Extend `tests/unit/<module>/ComponentRegistryTests.cpp` (or add a suite for the
  module): the component is registered under its name, in its place in the order; its
  defaults are what the struct says; representative invalid values are rejected.
- The generic tests already cover save/load, undo and the inspector for every
  registered component; do not repeat them per component.
- If a system consumes the component, test the system.

## 6. Check the result

```powershell
./scripts/dev.ps1 build
./scripts/dev.ps1 test -Filter "world|samples|editor"
./scripts/dev.ps1 run -Target cordite-editor -- --scene samples/showcase/showcase.scene.json
```

In the editor, select the showcase entity and look at the inspector: every property is
there, with its tooltip, and its widget fits its type. Or, without a window:

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --exec script.json
```

with a script that calls `component.types` and reads the description of the new type.

The editor reference image (`editor-showcase-1280x720`) changes when the showcase scene
does. Look at the new image and approve it (skill: `render-tests`).

## Adding a property to an existing component

1. Add the member with a default.
2. Add one `.property(...)` line to the description.
3. Existing scene files keep loading: a property a file does not mention gets its
   default. No migration is needed for an addition.

Renaming or removing a property, or changing what its values mean, breaks existing
files. That needs a new `kSceneFormatVersion` and code that upgrades old documents;
do not do it casually.
