/// @file
/// Keeps the showcase project complete.
///
/// The showcase is the project that exercises everything the engine has. These tests
/// fail when a component, a named option of a component or of a material, an asset or a
/// function that scripts are given exists but is not used in samples/showcase: adding a
/// feature includes showing it.

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Image.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/Project.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/engine/SceneChange.h"
#include "cordite/math/Color.h"
#include "cordite/math/Equirectangular.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/InputMap.h"
#include "cordite/platform/KeyCodes.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/SceneSerializer.h"

#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
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
    /// What is pressed and moved - by the tests that play - and the controls of the
    /// project made of it, which is what its scripts ask about.
    Input input;
    InputActions controls;
    /// The bodies of the scene, for the scripts to ask about. No time passes for them
    /// here: what falls and what is pushed is for the tests of the engine.
    std::unique_ptr<PhysicsSystem> physics = test::unwrap(PhysicsSystem::create());

    Showcase() {
        REQUIRE(registerEngineComponents(components).has_value());
        const Status loaded = loadScene(project.startScenePath(), world, components);
        REQUIRE_MESSAGE(loaded.has_value(), test::errorOf(loaded));
        controls.setMap(test::unwrap(loadInputMap(project.inputMapPath())));
    }

    /// What the engine does where a frame begins: the scene a script asked for in the
    /// frame before takes the place of the one that is there.
    void followSceneRequest(ScriptSystem& scripts) {
        if (const std::optional<Uuid> scene = scripts.takeRequestedScene()) {
            const Status changed = changeScene({.world = world,
                                                .components = components,
                                                .assets = *assets,
                                                .scripts = scripts,
                                                .physics = physics.get(),
                                                .controls = &controls},
                                               *scene);
            REQUIRE_MESSAGE(changed.has_value(), test::errorOf(changed));
        }
    }

    /// A frame of the game, as far as its scripts go: the controls are worked out, every
    /// body is where its entity is, and the scripts run.
    ScriptStatistics play(ScriptSystem& scripts, double deltaSeconds) {
        followSceneRequest(scripts);
        controls.update(input);
        physics->synchronize(world, *assets);
        const ScriptStatistics statistics = scripts.update({.world = world,
                                                            .components = components,
                                                            .assets = *assets,
                                                            .deltaSeconds = deltaSeconds,
                                                            .controls = &controls,
                                                            .physics = physics.get()});
        input.beginFrame();
        return statistics;
    }

    /// A frame of the game with its simulation, a sixtieth of a second long: the scripts
    /// run, the bodies move on, and the scripts are told what met and what parted.
    ScriptStatistics simulate(ScriptSystem& scripts) {
        const double frameSeconds = 1.0 / 60.0;
        followSceneRequest(scripts);
        controls.update(input);
        const ScriptContext context = {.world = world,
                                       .components = components,
                                       .assets = *assets,
                                       .deltaSeconds = frameSeconds,
                                       .controls = &controls,
                                       .physics = physics.get()};
        scripts.update(context);
        physics->update(world, *assets, static_cast<float>(frameSeconds));
        const ScriptStatistics statistics = scripts.tellContacts(context, physics->events());
        input.beginFrame();
        return statistics;
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
                case AssetType::Script: {
                    const auto script = showcase.assets->loadScript(asset.id);
                    CHECK_MESSAGE(script.has_value(), test::errorOf(script));
                    break;
                }
                case AssetType::Sound: {
                    const auto sound = showcase.assets->loadSound(asset.id);
                    CHECK_MESSAGE(sound.has_value(), test::errorOf(sound));
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
        for (const AssetType type : kAssetTypes) {
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

    TEST_CASE("the showcase is heard: its lamp hums, its music plays, and its camera listens") {
        Showcase showcase;
        const test::LogCapture log(LogLevel::Warn);
        const std::unique_ptr<AudioSystem> audio = test::unwrap(AudioSystem::create());
        showcase.world.updateWorldTransforms();
        // A second of it; returns the loudest that was heard at the end of it.
        AudioStatistics statistics;
        const auto listen = [&] {
            float loudest = 0.0f;
            for (int frame = 0; frame < 60; ++frame) {
                statistics = audio->update(showcase.world, *showcase.assets, 1.0 / 60.0);
                if (frame >= 30) {
                    for (const float sample : audio->mixed()) {
                        loudest = std::max(loudest, std::abs(sample));
                    }
                }
            }
            return loudest;
        };

        // Two sources sound from the start, the lamp and the music; the camera hears them.
        const float both = listen();
        CHECK(statistics.sources == 2);
        CHECK(statistics.voices == 2);
        CHECK(statistics.waiting == 0);
        CHECK(statistics.sounds == 2);
        CHECK(statistics.listeners == 1);
        CHECK(findAudioListener(showcase.world) == findPrimaryCamera(showcase.world));
        // Heard, and not so loud that the mix has to cut anything off.
        CHECK(both > 0.05f);
        CHECK(both < 0.8f);

        // Each of the two by itself: the music with its bus alone, and the lamp - which
        // has a place, and is heard from where the camera stands - with its own.
        REQUIRE(audio->setVolume(AudioBus::Ambience, 0.0f).has_value());
        const float music = listen();
        CHECK(music > 0.02f);
        REQUIRE(audio->setVolume(AudioBus::Ambience, 1.0f).has_value());
        REQUIRE(audio->setVolume(AudioBus::Music, 0.0f).has_value());
        const float lamp = listen();
        CHECK(lamp > 0.02f);
        REQUIRE(audio->setVolume(AudioBus::Ambience, 0.0f).has_value());
        CHECK(listen() == 0.0f);
        REQUIRE(audio->setVolume(AudioBus::Ambience, 1.0f).has_value());
        REQUIRE(audio->setVolume(AudioBus::Music, 1.0f).has_value());

        // Three more wait to be played. Told to, each plays its sound once and says when
        // it is over: none of them is as long as a second.
        std::vector<Entity> waiting;
        for (const char* name : {"Clock", "Sentry post", "Ball"}) {
            CAPTURE(name);
            const Entity entity = showcase.world.findByName(name);
            auto* source = showcase.world.tryGet<AudioSourceComponent>(entity);
            REQUIRE(source != nullptr);
            CHECK_FALSE(source->playing);
            CHECK_FALSE(source->loop);
            source->playing = true;
            waiting.push_back(entity);
        }
        statistics = audio->update(showcase.world, *showcase.assets, 1.0 / 60.0);
        CHECK(statistics.sources == 5);
        CHECK(statistics.sounds == 5);
        listen();
        CHECK(statistics.sources == 2);
        for (const Entity entity : waiting) {
            CHECK_FALSE(showcase.world.tryGet<AudioSourceComponent>(entity)->playing);
        }
        // Nothing about any of it was worth a warning.
        CHECK(log.messages().empty());
    }

    TEST_CASE("the scripts of the showcase run, and none of them has a problem") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());
        const Entity clock = showcase.world.findByName("Clock");
        REQUIRE_FALSE(clock.isNull());

        // A second and a quarter of the game, a quarter at a time.
        ScriptStatistics statistics;
        size_t scripted = 0;
        for (int frame = 0; frame < 5; ++frame) {
            statistics = showcase.play(*scripts, 0.25);
        }
        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
        showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
            scripted += showcase.world.has<ScriptComponent>(entity) ? 1 : 0;
        });
        CHECK(scripted > 0);
        CHECK(statistics.instances == scripted);
        CHECK(statistics.stopped == 0);
        CHECK(statistics.calls > 0);
        // The clock ticks twice a second, as its Script says, and shows it in its name.
        CHECK(showcase.world.name(clock) == "Clock (2)");
        // The sentry has been told where to walk: to the end of its beat, at the speed
        // its Script gives it. Nothing is simulated here, so it has not gone anywhere.
        const Entity sentry = showcase.world.findByName("Sentry");
        REQUIRE(showcase.world.has<CharacterControllerComponent>(sentry));
        CHECK(showcase.world.tryGet<CharacterControllerComponent>(sentry)->movement == Vec3(-0.8f, 0.0f, 0.0f));

        // Ended, a script has the last word.
        const test::LogCapture log(LogLevel::Info);
        scripts->clear({.world = showcase.world, .components = showcase.components, .assets = *showcase.assets});
        CHECK(log.contains("Clock (2) stopped"));
    }

    TEST_CASE("the scripts of the showcase hear of everything the engine tells scripts") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());
        const Entity post = showcase.world.findByName("Sentry post");
        const Entity ball = showcase.world.findByName("Ball");
        REQUIRE(showcase.world.has<LightComponent>(post));
        REQUIRE_FALSE(ball.isNull());
        const auto brightness = [&] { return showcase.world.tryGet<LightComponent>(post)->intensity; };
        const auto run = [&](int frames) {
            for (int frame = 0; frame < frames; ++frame) {
                showcase.simulate(*scripts);
            }
        };

        // Half a second in, the left button of the pointer is "fire": the ball is knocked
        // off the floor by its script, which the simulation then tells that the two have
        // parted - and, when it comes down, that they have met again, which it counts.
        run(30);
        CHECK(showcase.world.name(ball) == "Ball");
        CHECK(brightness() == 0.0f);
        showcase.input.injectMouseButton(MouseButton::Left, true);
        run(15);
        CHECK(showcase.world.transform(ball).position.y > 0.45f);
        showcase.input.injectMouseButton(MouseButton::Left, false);

        // Two and a half seconds in, the sentry stands at the end of its beat, in the
        // trigger of its post: the light there is lit. Two seconds and a half later it
        // is on its way back, and the light is dark again.
        run(105);
        CHECK(brightness() == 3.0f);
        CHECK(showcase.world.name(ball).starts_with("Ball ("));
        run(150);
        CHECK(brightness() == 0.0f);

        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
        // Every function the engine calls in a script has been called in one of them.
        for (const ScriptCallbackInfo& callback : scripts->callbacks()) {
            CAPTURE(callback.name);
            CHECK_MESSAGE(callback.calls > 0, "give a script of the showcase a function `"
                                                  << callback.name << "`, and something in the scene that it hears of");
        }
    }

    TEST_CASE("the lamp of the showcase flickers: a task that waits, dips the light and waits again") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());
        const Entity lamp = showcase.world.findByName("Lamp");
        REQUIRE(showcase.world.has<LightComponent>(lamp));
        const float steady = showcase.world.tryGet<LightComponent>(lamp)->intensity;
        REQUIRE(steady > 1.0f);

        // Seven seconds of the game, a sixtieth at a time. It burns steadily for three to
        // six of them, then dips three times in a row - a twentieth of a second each, to
        // three tenths of its brightness - and burns steadily again.
        int dimFrames = 0;
        int dips = 0;
        int firstDip = -1;
        bool dimmed = false;
        for (int frame = 0; frame < 420; ++frame) {
            showcase.simulate(*scripts);
            const float intensity = showcase.world.tryGet<LightComponent>(lamp)->intensity;
            const bool dim = intensity < steady;
            if (dim) {
                CHECK(intensity == doctest::Approx(steady * 0.3f));
                ++dimFrames;
                firstDip = firstDip < 0 ? frame : firstDip;
            } else {
                CHECK(intensity == steady);
            }
            dips += dim && !dimmed ? 1 : 0;
            dimmed = dim;
        }
        CHECK(firstDip >= 180);
        CHECK(firstDip <= 361);
        CHECK(dips == 3);
        // Three frames a dip, give or take the one a wait ends in.
        CHECK(dimFrames >= 6);
        CHECK(dimFrames <= 12);
        CHECK_FALSE(dimmed);
        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
    }

    TEST_CASE("whoever plays the showcase travels to its annex, and back") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());
        const test::LogCapture log(LogLevel::Info);
        showcase.play(*scripts, 0.25);
        REQUIRE(showcase.world.sceneName() == "Showcase");

        // T is "travel". The script that hears it asks for the annex - and the scene
        // goes on to the end of its frame, the scripts of all its entities with it.
        showcase.input.injectKey(Key::T, true);
        ScriptStatistics statistics = showcase.play(*scripts, 0.25);
        showcase.input.injectKey(Key::T, false);
        CHECK(log.contains("leaving for scenes/annex.scene.json"));
        CHECK(showcase.world.sceneName() == "Showcase");
        // The seven of the scene, and the clock the self-test has set up for a few frames.
        CHECK(statistics.instances == 8);
        CHECK_FALSE(log.contains("Clock stopped"));

        // The next frame begins in the annex. The scripts of the showcase have ended -
        // the clock had its last word - and of its entities none is left; what runs is
        // the one script of the annex, which is the way back.
        statistics = showcase.play(*scripts, 0.25);
        CHECK(showcase.world.sceneName() == "Annex");
        CHECK(log.contains("Clock stopped"));
        CHECK(showcase.world.findByName("Sentry").isNull());
        CHECK_FALSE(showcase.world.findByName("Way back").isNull());
        CHECK(statistics.instances == 1);
        CHECK(statistics.timers == 0);
        CHECK(scripts->seconds() == 0.25);
        // What the annex needs of the project is there: its scene has no problems.
        CHECK(findSceneProblems(showcase.world, showcase.components, showcase.assets->database()).empty());

        // And back: the showcase starts anew, with the clock at nothing.
        showcase.input.injectKey(Key::T, true);
        showcase.play(*scripts, 0.25);
        showcase.input.injectKey(Key::T, false);
        CHECK(log.contains("leaving for scenes/showcase.scene.json"));
        statistics = showcase.play(*scripts, 0.25);
        CHECK(showcase.world.sceneName() == "Showcase");
        CHECK(showcase.world.findByName("Way back").isNull());
        CHECK_FALSE(showcase.world.findByName("Clock").isNull());
        CHECK(statistics.instances == 7);
        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
        CHECK(log.count(LogLevel::Warn) == 0);
        CHECK(log.count(LogLevel::Error) == 0);
    }

    TEST_CASE("whoever plays the showcase takes the sentry over with the controls of the project") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());
        const Entity sentry = showcase.world.findByName("Sentry");
        REQUIRE(showcase.world.has<CharacterControllerComponent>(sentry));
        const auto controller = [&]() -> const CharacterControllerComponent& {
            return *showcase.world.tryGet<CharacterControllerComponent>(sentry);
        };

        // Left alone it walks its beat, at the speed its Script gives it.
        showcase.play(*scripts, 0.25);
        CHECK(controller().movement == Vec3(-0.8f, 0.0f, 0.0f));
        CHECK(controller().jump == 0.0f);

        // D is "moveX" pushed all the way: to the right, which is +X.
        showcase.input.injectKey(Key::D, true);
        showcase.play(*scripts, 0.25);
        CHECK(controller().movement == Vec3(0.8f, 0.0f, 0.0f));
        // W as well is forward, which is -Z - and no faster for being both.
        showcase.input.injectKey(Key::W, true);
        showcase.play(*scripts, 0.25);
        CHECK(controller().movement.x == doctest::Approx(0.8 * std::sqrt(0.5)));
        CHECK(controller().movement.z == doctest::Approx(-0.8 * std::sqrt(0.5)));
        CHECK(controller().movement.y == 0.0f);

        // Space is "jump": told once, when it goes down, with the speed the script declares.
        showcase.input.injectKey(Key::Space, true);
        showcase.play(*scripts, 0.25);
        CHECK(controller().jump == 4.0f);
        // The simulation takes that and sets it back; here a test does.
        showcase.world.tryGet<CharacterControllerComponent>(sentry)->jump = 0.0f;
        showcase.play(*scripts, 0.25);
        CHECK(controller().jump == 0.0f);

        // Let go of, it takes up its beat again.
        showcase.input.releaseAll();
        showcase.play(*scripts, 0.25);
        CHECK(controller().movement == Vec3(-0.8f, 0.0f, 0.0f));
        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
    }

    TEST_CASE("the scripts of the showcase call everything scripts are given, and leave its components as they were") {
        Showcase showcase;
        const std::unique_ptr<ScriptSystem> scripts = test::unwrap(ScriptSystem::create());

        // What every component of every entity holds, as a file would have it.
        const auto components = [&] {
            std::map<std::string, std::string> held;
            showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
                for (const ComponentDescriptor* descriptor : showcase.components.components()) {
                    const void* instance = descriptor->getConst(showcase.world.registry(), entity.handle);
                    if (instance != nullptr) {
                        held[showcase.world.id(entity).toString() + " " + descriptor->type.name()] =
                            dumpJson(descriptor->type.toJson(instance), -1);
                    }
                }
            });
            return held;
        };
        const std::map<std::string, std::string> before = components();
        REQUIRE(before.size() > 100);

        const test::LogCapture log(LogLevel::Info);
        for (int frame = 0; frame < 5; ++frame) {
            showcase.play(*scripts, 0.25);
        }
        for (const ScriptProblem& problem : scripts->problems()) {
            FAIL_CHECK(problem.message);
        }
        // The self-test has come to its end, and says how much it compared.
        CHECK(log.contains("self-test: "));
        CHECK(log.contains(" checks passed"));
        CHECK(log.count(LogLevel::Warn) == 0);
        CHECK(log.count(LogLevel::Error) == 0);

        // It handed every component of the scene to a script and took it back, and made
        // and destroyed entities of its own. Nothing of that is left: the scene holds what
        // it held, to the digit.
        const std::map<std::string, std::string> after = components();
        CHECK(after.size() == before.size());
        for (const auto& [which, held] : before) {
            CAPTURE(which);
            const auto now = after.find(which);
            if (now == after.end()) {
                FAIL_CHECK("the component is gone");
            } else {
                CHECK(now->second == held);
            }
        }

        // Every function was called - but for the two that say something is wrong, which
        // in a scene where nothing is wrong are not.
        size_t functions = 0;
        for (const ScriptFunctionInfo& function : scripts->runtime().functions()) {
            CAPTURE(function.name);
            ++functions;
            if (function.name == "cordite.log.warn" || function.name == "cordite.log.error") {
                CHECK(function.calls == 0);
            } else {
                CHECK_MESSAGE(function.calls > 0, "call " << function.name
                                                          << " in samples/showcase/assets/scripts/selftest.lua, and "
                                                             "compare what it gives with what it should");
            }
        }
        CHECK(functions > 70);
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

    TEST_CASE("the controls of the showcase are read, written as they are, and use every kind of binding") {
        const Project project = test::unwrap(loadProject(showcaseDirectory()));
        REQUIRE_FALSE(project.inputMapPath().empty());
        const InputMap map = test::unwrap(loadInputMap(project.inputMapPath()));
        CHECK_FALSE(map.actions.empty());
        CHECK_FALSE(map.axes.empty());
        // Everything is written out in the file: reading it and writing it again changes
        // nothing but its comments.
        CHECK(serializeInputMap(map) == test::unwrap(readJsonFile(project.inputMapPath())));

        std::set<size_t> pressedBy;
        for (const InputAction& action : map.actions) {
            for (const ActionBinding& binding : action.bindings) {
                pressedBy.insert(binding.index());
            }
        }
        std::set<size_t> movedBy;
        for (const InputAxis& axis : map.axes) {
            for (const AxisBinding& binding : axis.bindings) {
                movedBy.insert(binding.source.index());
            }
        }
        CHECK_MESSAGE(pressedBy.size() == std::variant_size_v<ActionBinding>,
                      "bind an action of the showcase to every kind of control: a key, a mouse button, a "
                      "gamepad button and a gamepad axis");
        CHECK_MESSAGE(movedBy.size() == std::variant_size_v<decltype(AxisBinding::source)>,
                      "bind an axis of the showcase to every kind of control: keys, gamepad buttons, a stick, "
                      "the pointer and the wheel");
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
