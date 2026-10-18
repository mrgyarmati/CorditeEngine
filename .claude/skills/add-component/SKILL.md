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

The engine's light (`engine/cordite/renderer/RenderComponents.h`), shortened:

```cpp
enum class LightType : uint8_t {
    Directional,
    Point,
    Spot,
};

std::string_view nameOf(LightType type);

/// A light. It shines from where the entity is; a directional or a spot light shines
/// along the entity's -Z axis.
struct LightComponent {
    LightType type = LightType::Point;
    /// Linear color.
    Vec3 color{1.0f};
    /// Brightness. 1 lights a white, matte surface that faces the light to white.
    float intensity = 1.0f;
    /// The distance at which the light has faded to nothing, in metres.
    float range = 10.0f;
    /// The lighting channels the light shines on, one bit each.
    uint32_t channels = 1;
};
```

Rules:

- Data only: no methods that do work, no pointers to systems, no GPU handles. Default
  member initializers give the values a new component has.
- Members must be copyable and default-constructible.
- State that exists only while the game runs (a physics body handle, a cached matrix)
  either lives in a separate, unregistered component that the system manages, or is a
  property marked `transient`.
- Refer to other entities and to assets by `Uuid` (or a `std::vector<Uuid>`), with the
  matching `semantic`; for an asset also say which kind with `referenceType` ("Mesh",
  "Material", "Texture", "Model", "Scene", "Skeleton", "Animation", "Script", "Sound",
  "Prefab", "RenderTarget", "Sequence" - `displayNameOf` of the asset type). The inspector
  then offers the assets of that kind, and `scene.validate` checks the reference.
  `MeshRendererComponent` in `renderer/RenderComponents.cpp` is the example.

## 2. Describe it

In the module's `Components.cpp` (or equivalent), next to the other descriptions:

```cpp
TypeInfo describeLight() {
    return TypeBuilder<LightComponent>("Light")
        .enumProperty("type", &LightComponent::type,
                      {{LightType::Directional, std::string(nameOf(LightType::Directional))},
                       {LightType::Point, std::string(nameOf(LightType::Point))},
                       {LightType::Spot, std::string(nameOf(LightType::Spot))}},
                      {.description = "Directional: from one direction, everywhere. Point: from a point ..."})
        .property("color", &LightComponent::color,
                  {.description = "Linear color of the light", .semantic = PropertySemantic::Color, .minimum = 0.0})
        .property("intensity", &LightComponent::intensity,
                  {.description = "Brightness; 1 lights a white matte surface facing the light to white",
                   .minimum = 0.0})
        .property("range", &LightComponent::range,
                  {.description = "The distance at which the light has faded to nothing", .minimum = 0.01,
                   .unit = "m"})
        .property("channels", &LightComponent::channels,
                  {.description = "The lighting channels the light shines on, one bit each",
                   .semantic = PropertySemantic::BitMask, .minimum = 0.0, .maximum = 255.0})
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
  (with `referenceType`), `Multiline`, and `BitMask` for an integer whose bits are
  switches - a row of check boxes, as many as its `maximum` has bits.
- An enum has a `nameOf` in the header, and its options are named by it, so that
  code, files and tools use the same words.
- `readOnly`: shown, saved and loaded, but tools cannot change it.
  `transient`: runtime state, never saved.
- Property types: `bool`, integers, `float`, `double`, `std::string`, `Uuid`, `Vec2/3/4`,
  `Quat` (include `cordite/math/MathReflection.h`), enums (`enumProperty`), nested
  reflected structs (`objectProperty`), and `std::vector` of the plain ones. A new kind
  of value needs a `PropertyCodec` specialization (see `core/Reflection.h`).
- A component that is "settings, and where they apply" can be built on the struct of
  the settings (`struct PostProcessComponent : PostProcessSettings`): the builder
  takes members of the base like the component's own, and code that only needs the
  settings takes the base.

## 3. Register it

In the same file's registration function:

```cpp
CORDITE_TRY(registry.add<LightComponent>(
    describeLight(), {.description = "A light at the entity's place: directional, point or spot."}));
```

A module that registers components for the first time adds its
`register<Module>Components` function to `registerEngineComponents`
(`engine/cordite/engine/EngineComponents.cpp`). The order of registration is the order
components appear in scene files and in the inspector.

Pass `.optional = false` only for components every entity has from creation.

## 4. Show it in the showcase scene

Add an entity that uses the component, with every enum option appearing at least once,
to `samples/showcase/assets/scenes/showcase.scene.json`. The `samples` test fails until
this is done.
Give the entity a fixed, readable id in the style already used there.

## 5. Test it

- Extend `tests/unit/<module>/ComponentRegistryTests.cpp` (or add a suite for the
  module, as `tests/unit/renderer/RenderComponentsTests.cpp` is): the component is
  registered under its name, in its place in the order; its defaults are what the
  struct says; what it looks like in a scene file is pinned; representative invalid
  values are rejected.
- The generic tests already cover save/load, undo and the inspector for every
  registered component; do not repeat them per component.
- If a system consumes the component, test the system.

## 6. Check the result

```powershell
./scripts/dev.ps1 build
./scripts/dev.ps1 test -Filter "world|samples|editor"
./scripts/dev.ps1 run -Target cordite-editor -- --project samples/showcase
```

In the editor, select the showcase entity and look at the inspector: every property is
there, with its tooltip, and its widget fits its type. Or, without a window:

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --exec script.json
```

with a script that calls `component.types` and reads the description of the new type.

The editor reference images (`editor-showcase-1280x720` and the others that show the
showcase) change when the showcase scene does. Look at the new images and approve them
(skill: `render-tests`).

`docs/reference/components.md` lists every component with every property, and is written
by the engine: run `cordite_render_tests -ts=ReferenceDocs` with
`CORDITE_UPDATE_REFERENCES=1`, read the new section, and commit it with the component.

## Adding a property to an existing component

1. Add the member with a default.
2. Add one `.property(...)` line to the description.
3. Existing scene files keep loading: a property a file does not mention gets its
   default. No migration is needed for an addition.

Renaming or removing a property, or changing what its values mean, breaks existing
files. That needs a new `kSceneFormatVersion` and code that upgrades old documents;
do not do it casually.
