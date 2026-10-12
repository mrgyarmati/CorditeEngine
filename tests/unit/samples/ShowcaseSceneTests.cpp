/// @file
/// Keeps the showcase project complete.
///
/// The showcase is the project that exercises everything the engine has. These tests
/// fail when a component, a named option of a component or of a material, or an asset
/// exists but is not used in samples/showcase: adding a feature includes showing it.

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Image.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/Project.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/math/Color.h"
#include "cordite/math/Equirectangular.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/SceneSerializer.h"

#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace cordite;

namespace {

std::filesystem::path showcaseDirectory() {
    return test::sourceDirectory() / "samples" / "showcase";
}

/// The showcase project: its assets indexed, its start scene loaded into a world with
/// the engine's components.
struct Showcase {
    Project project = test::unwrap(loadProject(showcaseDirectory()));
    // Read-only: a test must not write into the repository.
    std::unique_ptr<AssetManager> assets =
        test::unwrap(AssetManager::create({.rootDirectory = project.assetDirectory()}));
    ComponentRegistry components;
    World world;

    Showcase() {
        REQUIRE(registerEngineComponents(components).has_value());
        const Status loaded = loadScene(project.startScenePath(), world, components);
        REQUIRE_MESSAGE(loaded.has_value(), test::errorOf(loaded));
    }

    /// Every asset the scene needs: what its components name, and what those need in turn.
    [[nodiscard]] std::set<Uuid> usedAssets() const {
        std::set<Uuid> used;
        std::vector<Uuid> pending;
        for (const AssetReferenceUse& use : collectAssetReferences(world, components)) {
            pending.push_back(use.asset);
        }
        while (!pending.empty()) {
            const Uuid id = pending.back();
            pending.pop_back();
            if (!used.insert(id).second) {
                continue;
            }
            if (const AssetInfo* asset = assets->database().find(id)) {
                pending.insert(pending.end(), asset->dependencies.begin(), asset->dependencies.end());
            }
        }
        return used;
    }
};

} // namespace

TEST_SUITE("Showcase") {
    TEST_CASE("the showcase is a project whose start scene loads with the engine's components") {
        const Showcase showcase;
        CHECK(showcase.project.settings.name == "Showcase");
        CHECK(showcase.project.settings.startScene == "scenes/showcase.scene.json");
        CHECK(showcase.world.sceneName() == "Showcase");
        CHECK(showcase.world.entityCount() >= 20);
        CHECK(showcase.world.roots().size() >= 2);
    }

    TEST_CASE("the asset directory of the showcase has no problems") {
        const Showcase showcase;
        for (const std::string& problem : showcase.assets->database().problems()) {
            FAIL_CHECK(problem);
        }
        // Every asset file has its description committed with it; without one, the id
        // would only hold until the file is moved.
        for (const AssetInfo& asset : showcase.assets->database().assets()) {
            if (asset.isBuiltin() || !asset.subPath.empty()) {
                continue;
            }
            CAPTURE(asset.path);
            std::filesystem::path meta = showcase.assets->database().sourceFile(asset);
            meta += ".meta";
            CHECK(std::filesystem::exists(meta));
        }
    }

    TEST_CASE("every reference from the scene leads to an asset of the right kind") {
        const Showcase showcase;
        for (const SceneProblem& problem :
             findSceneProblems(showcase.world, showcase.components, showcase.assets->database())) {
            FAIL_CHECK(showcase.world.name(problem.entity) << ": " << problem.message);
        }
    }

    TEST_CASE("every asset of the showcase can be loaded") {
        const Showcase showcase;
        for (const AssetInfo& asset : showcase.assets->database().assets()) {
            CAPTURE(asset.reference());
            switch (asset.type) {
                case AssetType::Mesh: {
                    const auto mesh = showcase.assets->loadMesh(asset.id);
                    REQUIRE_MESSAGE(mesh.has_value(), test::errorOf(mesh));
                    CHECK(validateMesh(**mesh).has_value());
                    break;
                }
                case AssetType::Material: {
                    const auto material = showcase.assets->loadMaterial(asset.id);
                    CHECK_MESSAGE(material.has_value(), test::errorOf(material));
                    break;
                }
                case AssetType::Texture: {
                    const auto texture = showcase.assets->loadTexture(asset.id);
                    CHECK_MESSAGE(texture.has_value(), test::errorOf(texture));
                    break;
                }
                case AssetType::Model: {
                    const auto model = showcase.assets->loadModel(asset.id);
                    CHECK_MESSAGE(model.has_value(), test::errorOf(model));
                    // A model in the showcase is one the importer brings in whole.
                    CHECK(asset.warnings.empty());
                    break;
                }
                case AssetType::Skeleton: {
                    const auto skeleton = showcase.assets->loadSkeleton(asset.id);
                    CHECK_MESSAGE(skeleton.has_value(), test::errorOf(skeleton));
                    break;
                }
                case AssetType::Animation: {
                    const auto animation = showcase.assets->loadAnimation(asset.id);
                    CHECK_MESSAGE(animation.has_value(), test::errorOf(animation));
                    break;
                }
                case AssetType::Scene:
                    break;
            }
        }
    }

    TEST_CASE("every asset of the showcase is used by its scene") {
        const Showcase showcase;
        const std::set<Uuid> used = showcase.usedAssets();
        for (const AssetInfo& asset : showcase.assets->database().assets()) {
            // A model is a way of placing its meshes; the scene shows the meshes placed.
            // Built-in assets are not the project's to use up.
            if (asset.isBuiltin() || asset.type == AssetType::Scene || asset.type == AssetType::Model) {
                continue;
            }
            CAPTURE(asset.reference());
            CHECK_MESSAGE(used.contains(asset.id), "nothing in the showcase scene uses this asset");
        }
    }

    TEST_CASE("the showcase uses every kind of asset") {
        const Showcase showcase;
        std::set<AssetType> projectTypes;
        bool hasEmbeddedTexture = false;
        bool hasBinaryModel = false;
        for (const AssetInfo& asset : showcase.assets->database().assets()) {
            if (asset.isBuiltin()) {
                continue;
            }
            projectTypes.insert(asset.type);
            hasEmbeddedTexture = hasEmbeddedTexture || (asset.type == AssetType::Texture && !asset.subPath.empty());
            hasBinaryModel = hasBinaryModel || (asset.type == AssetType::Model && asset.path.ends_with(".glb"));
        }
        for (const AssetType type :
             {AssetType::Texture, AssetType::Mesh, AssetType::Material, AssetType::Model, AssetType::Scene}) {
            CAPTURE(nameOf(type));
            CHECK(projectTypes.contains(type));
        }
        CHECK(hasEmbeddedTexture);
        CHECK(hasBinaryModel);

        // Every built-in mesh is on show as well.
        const std::set<Uuid> used = showcase.usedAssets();
        for (const BuiltinMesh mesh : kBuiltinMeshes) {
            CAPTURE(nameOf(mesh));
            CHECK(used.contains(builtinMeshId(mesh)));
        }
    }

    TEST_CASE("every component type of the engine is used in the showcase") {
        const Showcase showcase;
        for (const ComponentDescriptor* descriptor : showcase.components.components()) {
            CAPTURE(descriptor->type.name());
            bool used = false;
            showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
                used = used || descriptor->has(showcase.world.registry(), entity.handle);
            });
            CHECK_MESSAGE(used,
                          "add an entity with a " << descriptor->type.name() << " component to the showcase scene");
        }
    }

    TEST_CASE("every named option of every component is used in the showcase") {
        const Showcase showcase;
        for (const ComponentDescriptor* descriptor : showcase.components.components()) {
            for (const PropertyInfo& property : descriptor->type.properties()) {
                // What is not part of a scene - what a running simulation says of a thing - no
                // scene can use.
                if (property.type() != PropertyType::Enum || property.attributes().transient) {
                    continue;
                }

                std::set<std::string> seen;
                showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
                    if (const void* instance = descriptor->getConst(showcase.world.registry(), entity.handle)) {
                        seen.insert(property.get(instance).get<std::string>());
                    }
                });
                for (const EnumOption& option : property.enumOptions()) {
                    CAPTURE(descriptor->type.name());
                    CAPTURE(property.name());
                    CHECK_MESSAGE(seen.contains(option.name), "no entity in the showcase uses '" << option.name << "'");
                }
            }
        }
    }

    TEST_CASE("every named option of a material is used by a material of the showcase") {
        const Showcase showcase;
        std::vector<std::shared_ptr<const MaterialData>> materials;
        for (const Uuid& id : showcase.usedAssets()) {
            const AssetInfo* asset = showcase.assets->database().find(id);
            if (asset != nullptr && asset->type == AssetType::Material) {
                materials.push_back(test::unwrap(showcase.assets->loadMaterial(id)));
            }
        }

        for (const PropertyInfo& property : materialTypeInfo().properties()) {
            if (property.type() != PropertyType::Enum) {
                continue;
            }
            std::set<std::string> seen;
            for (const std::shared_ptr<const MaterialData>& material : materials) {
                seen.insert(property.get(material.get()).get<std::string>());
            }
            for (const EnumOption& option : property.enumOptions()) {
                CAPTURE(property.name());
                CHECK_MESSAGE(seen.contains(option.name), "no material in the showcase uses '" << option.name << "'");
            }
        }

        // What a material can do beyond its options: each has one that does it.
        const auto any = [&](const auto& predicate) { return std::ranges::any_of(materials, predicate); };
        CHECK(any([](const auto& material) { return material->doubleSided; }));
        CHECK(any([](const auto& material) { return !material->baseColorTexture.isNil(); }));
        CHECK(any([](const auto& material) { return !material->normalTexture.isNil(); }));
        CHECK(any([](const auto& material) { return !material->metallicRoughnessTexture.isNil(); }));
        CHECK(any([](const auto& material) { return !material->occlusionTexture.isNil(); }));
        CHECK(any([](const auto& material) { return !material->emissiveTexture.isNil(); }));
        CHECK(any([](const auto& material) { return material->uvScale != Vec2(1.0f); }));
        CHECK(any([](const auto& material) { return material->emissiveIntensity > 1.0f; }));
    }

    TEST_CASE("the showcase has a material that inherits from another") {
        const Showcase showcase;
        const AssetDatabase& database = showcase.assets->database();
        const AssetInfo* parent = database.findByReference("materials/painted_metal.material.json");
        const AssetInfo* instance = database.findByReference("materials/painted_metal_red.material.json");
        REQUIRE(parent != nullptr);
        REQUIRE(instance != nullptr);
        CHECK(instance->dependencies == std::vector<Uuid>{parent->id});

        const auto base = test::unwrap(showcase.assets->loadMaterial(parent->id));
        const auto red = test::unwrap(showcase.assets->loadMaterial(instance->id));
        // Its own color; everything else from the parent.
        CHECK(red->baseColor != base->baseColor);
        CHECK(red->roughness == base->roughness);
        CHECK(red->metallic == base->metallic);
    }

    TEST_CASE("the showcase covers hierarchy, disabled entities, submeshes and a camera to render from") {
        Showcase showcase;
        World& world = showcase.world;

        bool hasGrandchild = false;
        bool hasDisabledEntity = false;
        bool hasEntityDisabledByAncestor = false;
        bool hasSeveralMaterials = false;
        bool hasNoMaterial = false;
        bool hasMirroredMesh = false;
        world.updateWorldTransforms();
        world.traverse([&](Entity entity, uint32_t depth) {
            hasGrandchild = hasGrandchild || depth >= 2;
            hasDisabledEntity = hasDisabledEntity || !world.isEnabled(entity);
            hasEntityDisabledByAncestor =
                hasEntityDisabledByAncestor || (world.isEnabled(entity) && !world.isEnabledInHierarchy(entity));
            if (const auto* renderer = world.tryGet<MeshRendererComponent>(entity)) {
                hasSeveralMaterials = hasSeveralMaterials || renderer->materials.size() > 1;
                hasNoMaterial = hasNoMaterial || renderer->materials.empty();
                hasMirroredMesh = hasMirroredMesh || glm::determinant(Mat3(world.worldMatrix(entity))) < 0.0f;
            }
        });
        CHECK(hasGrandchild);
        CHECK(hasDisabledEntity);
        CHECK(hasEntityDisabledByAncestor);
        CHECK(hasSeveralMaterials);
        CHECK(hasNoMaterial);
        CHECK(hasMirroredMesh);

        // A game started on this scene has a camera, and it looks at the stage.
        const Entity camera = findPrimaryCamera(world);
        REQUIRE_FALSE(camera.isNull());
        const CameraView view =
            makeCameraView(world.worldMatrix(camera), *world.tryGet<CameraComponent>(camera), 16.0f / 9.0f);
        const std::optional<Vec2> stage = worldToScreen(view, Vec3(0.0f, 0.5f, 0.0f), Vec2(1600.0f, 900.0f));
        REQUIRE(stage.has_value());
        CHECK(stage->x == doctest::Approx(800.0f).epsilon(0.2));
        CHECK(stage->y == doctest::Approx(450.0f).epsilon(0.2));
    }

    TEST_CASE("the sun in the showcase's image of the sky stands where its sun shines from") {
        Showcase showcase;
        World& world = showcase.world;
        world.updateWorldTransforms();

        // The directional light that casts the shadows, and the environment.
        Vec3 towardsSun(0.0f);
        const auto lights = world.registry().view<const LightComponent>();
        for (const auto [handle, light] : lights.each()) {
            if (light.type == LightType::Directional && light.castsShadows) {
                // A light shines along the -Z axis of its entity.
                towardsSun = glm::normalize(Vec3(world.worldMatrix(Entity{handle}) * Vec4(0.0f, 0.0f, 1.0f, 0.0f)));
            }
        }
        REQUIRE(glm::length(towardsSun) == doctest::Approx(1.0f));
        const EnvironmentComponent* environment = nullptr;
        const auto environments = world.registry().view<const EnvironmentComponent>();
        for (const auto [handle, component] : environments.each()) {
            environment = &component;
        }
        REQUIRE(environment != nullptr);
        CHECK(environment->visible);

        // The brightest pixel of the image is its sun: an image of light, in which
        // the sun is many times brighter than white.
        const std::shared_ptr<const Image> sky = test::unwrap(showcase.assets->loadTexture(environment->texture));
        REQUIRE(sky->format() == ImageFormat::Rgba32F);
        CHECK(sky->width() == 2 * sky->height());
        float brightest = 0.0f;
        Vec2 where(0.0f);
        for (uint32_t y = 0; y < sky->height(); ++y) {
            for (uint32_t x = 0; x < sky->width(); ++x) {
                const float brightness = luminance(Vec3(sky->pixel(x, y)));
                if (brightness > brightest) {
                    brightest = brightness;
                    where = Vec2((static_cast<float>(x) + 0.5f) / static_cast<float>(sky->width()),
                                 (static_cast<float>(y) + 0.5f) / static_cast<float>(sky->height()));
                }
            }
        }
        CHECK(brightest > 8.0f);

        // Turned as the scene turns the image, it is where the light comes from, to
        // within a pixel or two of the image.
        const Vec3 inTheWorld = turnedAboutVertical(equirectDirection(where), toRadians(environment->rotationDegrees));
        CHECK(glm::degrees(std::acos(std::min(glm::dot(inTheWorld, towardsSun), 1.0f))) < 2.0f);
    }

    TEST_CASE("saving the showcase keeps everything that is in the file") {
        const Showcase showcase;
        const Json saved = serializeScene(showcase.world, showcase.components);

        // The file is written by hand, with comments; what counts is that loading and
        // saving it loses nothing.
        World reloaded;
        REQUIRE(deserializeScene(reloaded, saved, showcase.components).has_value());
        CHECK(serializeScene(reloaded, showcase.components) == saved);

        const Json original = test::unwrap(readJsonFile(showcase.project.startScenePath()));
        CHECK(saved["entities"].size() == original["entities"].size());
        CHECK(saved["name"] == original["name"]);
    }
}
