#pragma once

/// @file
/// Putting another scene into a world that is being played: what a game does between two
/// levels.
///
/// @code
/// // Between two frames, when a script has asked for it:
/// if (const std::optional<Uuid> scene = scripts.takeRequestedScene()) {
///     const SceneChange change = {.world = world, .components = components, .assets = assets, .scripts = scripts,
///                                 .physics = &physics, .animation = &animation};
///     if (const Status changed = changeScene(change, *scene); !changed) {
///         CORDITE_LOG_ERROR("Game", "{}", changed.error().message);   // the scene that was there goes on
///     }
/// }
/// @endcode
///
/// A world that is played is more than its entities. Scripts run for them, and are owed a
/// word when they end; bodies stand for them in the simulation; skeletons are posed for
/// them. All of that belongs to the scene that goes, and none of it may be left behind
/// for the scene that comes - whose entities may well have the very handles the old ones
/// had.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"

namespace cordite {

class AnimationSystem;
class AssetManager;
class ComponentRegistry;
class InputActions;
class PhysicsSystem;
class ScriptSystem;
class World;

/// The world a scene is put into, and what is kept for it.
struct SceneChange {
    World& world;
    const ComponentRegistry& components;
    /// Where the scene is found, by its id.
    AssetManager& assets;
    ScriptSystem& scripts;
    /// The simulation and the poses of the world, where it has them.
    PhysicsSystem* physics = nullptr;
    AnimationSystem* animation = nullptr;
    /// The controls the scripts are given, which the ones that end may still ask about.
    const InputActions* controls = nullptr;
};

/// Replaces what is in the world by the scene asset `scene`.
///
/// The file is read and checked first, against a world of its own. Only when all of it is
/// right do the scripts of the scene that is there end - each is told so (`destroy`)
/// while its entity still exists - are its bodies and poses forgotten, and is the new
/// scene put into the world, with its world transforms worked out. Its scripts start
/// with the next update, and its bodies are made by the next step.
///
/// Fails with NotFound for an id that is no asset and InvalidArgument for an asset that
/// is no scene, and as readJsonFile() and deserializeScene() do for a file that cannot be
/// read or is wrong, naming the file. After a failure the world is as it was, scripts,
/// bodies and all. Not to be called while the scripts run: between two frames.
[[nodiscard]] Status changeScene(const SceneChange& change, const Uuid& scene);

} // namespace cordite
