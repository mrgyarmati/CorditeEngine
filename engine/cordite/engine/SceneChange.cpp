#include "cordite/engine/SceneChange.h"

#include "cordite/animation/AnimationSystem.h"
#include "cordite/animation/SequenceSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/SceneSerializer.h"
#include "cordite/world/World.h"

#include <filesystem>
#include <string>

namespace cordite {

void endScene(const SceneChange& change) {
    // The scripts end while their entities are still there to be said goodbye to.
    change.scripts.clear({.world = change.world,
                          .components = change.components,
                          .assets = change.assets,
                          .controls = change.controls,
                          .physics = change.physics,
                          .audio = change.audio});
    if (change.physics != nullptr) {
        change.physics->clear();
    }
    if (change.animation != nullptr) {
        change.animation->clear();
    }
    if (change.sequences != nullptr) {
        change.sequences->clear();
    }
    // After the scripts have ended: what one of them still starts as it goes is not
    // heard in the scene that comes.
    if (change.audio != nullptr) {
        change.audio->clear();
    }
}

Status changeScene(const SceneChange& change, const Uuid& scene) {
    const AssetDatabase& database = change.assets.database();
    const AssetInfo* asset = database.find(scene);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset {} to load as a scene", scene.toString());
    }
    if (asset->type != AssetType::Scene) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is of the kind {}, not a scene", asset->reference(),
                         nameOf(asset->type));
    }
    const std::filesystem::path path = database.sourceFile(*asset);
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(path));

    // Tried on a world of its own first: a scene that is wrong must not cost the one
    // that is there its scripts and its bodies.
    {
        World trial;
        if (const Status fits = deserializeScene(trial, document, change.components); !fits) {
            return tl::unexpected(fits.error().withContext(pathToUtf8(path)));
        }
    }

    endScene(change);
    if (const Status loaded = deserializeScene(change.world, document, change.components); !loaded) {
        // What fitted a world a moment ago fits this one: this is not expected to happen.
        return tl::unexpected(loaded.error().withContext(pathToUtf8(path)));
    }
    change.world.updateWorldTransforms();
    return {};
}

} // namespace cordite
