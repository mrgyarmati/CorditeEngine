#pragma once

/// @file
/// The fixture shared by the tests of the script system: a directory of assets to put
/// scripts into, a world, and the scripts that run in it.
///
/// @code
/// ScriptStage stage;
/// const Uuid door = stage.write("scripts/door.lua", "local Door = {} ... return Door");
/// const Entity entity = stage.add("Door", door);
/// stage.update();
/// CHECK(stage.said() == std::vector<std::string>{"start Door"});     // what the scripts printed
///
/// // A piece of Lua, run once by an entity of its own:
/// CHECK(stage.line("print(entity:name())") == "Probe");
/// CHECK(stage.refusal("entity:setName()") == "bad argument #1 to 'setName' (string expected, got no value)");
///
/// // Controls, and something that presses them:
/// stage.setControls(R"({"format": "cordite.input", "version": 1,
///                       "actions": [{"name": "jump", "bindings": [{"key": "Space"}]}], "axes": []})");
/// stage.input.injectKey(Key::Space, true);
/// CHECK(stage.line("print(cordite.input.wasPressed('jump'))") == "true");
///
/// // A simulation, for scripts that ask about bodies:
/// stage.simulate();
/// stage.world.add<ColliderComponent>(stage.world.createEntity("Wall"));
/// stage.update();                                       // the step that makes the body
/// CHECK(stage.line("print(cordite.physics.raycast({0, 0, 5}, {0, 0, -1}).entity)") == "Entity(Wall)");
/// @endcode

#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/InputMap.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite::test {

/// The frame the tests of the script system let pass.
inline constexpr double kScriptFrame = 0.5;

/// A directory of assets, a world, and the scripts that run in it.
struct ScriptStage {
    explicit ScriptStage(const ScriptSystemDesc& desc = {})
        : assets(unwrap(AssetManager::create({.rootDirectory = directory.path()}))),
          scripts(unwrap(ScriptSystem::create(desc))) {
        const Status ofTheWorld = registerWorldComponents(components);
        REQUIRE_MESSAGE(ofTheWorld.has_value(), errorOf(ofTheWorld));
        const Status ofScripts = registerScriptComponents(components);
        REQUIRE_MESSAGE(ofScripts.has_value(), errorOf(ofScripts));
    }

    /// Writes a script - or writes it anew - and has the assets look again. Returns the
    /// id of the script.
    Uuid write(std::string_view path, std::string_view source) {
        const Status written = writeTextFile(directory.path() / pathFromUtf8(path), source);
        REQUIRE_MESSAGE(written.has_value(), errorOf(written));
        const Status refreshed = assets->refresh();
        REQUIRE_MESSAGE(refreshed.has_value(), errorOf(refreshed));
        const AssetInfo* asset = assets->database().findByReference(path);
        REQUIRE(asset != nullptr);
        return asset->id;
    }

    /// An entity that has a Script.
    Entity add(std::string_view name, const Uuid& script, Json properties = Json::object(), Entity parent = {}) {
        const Entity entity = world.createEntity(name);
        if (!parent.isNull()) {
            const Status attached = world.setParent(entity, parent);
            REQUIRE_MESSAGE(attached.has_value(), errorOf(attached));
        }
        world.add<ScriptComponent>(entity) = {.script = script, .properties = std::move(properties)};
        return entity;
    }

    /// Gives the stage the controls that an input file with this text names.
    void setControls(std::string_view json) { controls.setMap(unwrap(parseInputMap(unwrap(parseJson(json))))); }

    /// Gives the stage a simulation, and its scripts the components of one by name.
    /// From then on a frame of the stage ends as one of the engine goes on: with the
    /// simulation catching up - which is also what makes the bodies of new entities.
    void simulate() {
        physics = unwrap(PhysicsSystem::create());
        const Status ofPhysics = registerPhysicsComponents(components);
        REQUIRE_MESSAGE(ofPhysics.has_value(), errorOf(ofPhysics));
    }

    [[nodiscard]] ScriptContext context(double deltaSeconds = kScriptFrame) {
        return {.world = world,
                .components = components,
                .assets = *assets,
                .deltaSeconds = deltaSeconds,
                .controls = played ? &controls : nullptr,
                .physics = physics.get()};
    }

    /// A frame, as the engine has them: the controls are worked out from what was
    /// pressed and moved since the frame before, then the scripts run, and then the
    /// simulation moves on, where there is one, and the scripts are told what met and
    /// what parted in it.
    ScriptStatistics update(double deltaSeconds = kScriptFrame) {
        controls.update(input);
        ScriptStatistics statistics = scripts->update(context(deltaSeconds));
        if (physics != nullptr) {
            physics->update(world, *assets, static_cast<float>(deltaSeconds));
            statistics = scripts->tellContacts(context(deltaSeconds), physics->events());
        }
        input.beginFrame();
        return statistics;
    }

    /// What the scripts have printed since this was last asked, in order.
    std::vector<std::string> said() { return take(LogLevel::Info); }

    /// What was logged as an error since this was last asked.
    std::vector<std::string> errors() { return take(LogLevel::Error); }

    /// Runs `body` once, as what the script of an entity called "Probe" does in an
    /// update - the entity is `entity` in there, the table of the script `self` - and
    /// returns what it printed. The probe is gone afterwards; what it made stays. It
    /// must run without an error. Two updates pass: the one the probe runs in, and one
    /// in which the scripts find it gone - whatever else runs in the stage runs twice.
    std::vector<std::string> lines(std::string_view body) {
        const Entity probe = addProbe(body);
        update();
        std::vector<std::string> printed = said();
        std::string wrong;
        for (const std::string& problem : problems()) {
            wrong += problem + "\n";
        }
        REQUIRE_MESSAGE(wrong.empty(), wrong);
        removeProbe(probe);
        return printed;
    }

    /// The same for one line, which is all that `body` is expected to print.
    std::string line(std::string_view body) {
        const std::vector<std::string> printed = lines(body);
        REQUIRE(printed.size() == 1);
        return printed.front();
    }

    /// What a probe that runs `body` is stopped with: the error, without the entity and
    /// the file before it and without the calls that led there after it.
    std::string refusal(std::string_view body) {
        const Entity probe = addProbe(body);
        update();
        static_cast<void>(said());
        static_cast<void>(errors());
        const std::vector<std::string> wrong = problems();
        REQUIRE(wrong.size() == 1);
        removeProbe(probe);
        // "'Probe' (scripts/probe1.lua): scripts/probe1.lua:3: what is wrong\nstack traceback: ..."
        std::string message = wrong.front().substr(0, wrong.front().find('\n'));
        const std::string file = "scripts/probe" + std::to_string(m_probes) + ".lua";
        if (const std::string who = "'Probe' (" + file + "): "; message.starts_with(who)) {
            message.erase(0, who.size());
        }
        if (message.starts_with(file + ":")) {
            message.erase(0, message.find(": ") + 2);
        }
        return message;
    }

    /// The messages of the problems there are, in order.
    [[nodiscard]] std::vector<std::string> problems() const {
        std::vector<std::string> messages;
        for (const ScriptProblem& problem : scripts->problems()) {
            messages.push_back(problem.message);
        }
        return messages;
    }

    TempDirectory directory;
    std::unique_ptr<AssetManager> assets;
    ComponentRegistry components;
    World world;
    /// What is pressed and moved - a test does that with the inject functions - and the
    /// controls made of it: those of no map, until setControls() gives one.
    Input input;
    InputActions controls;
    /// Whether the scripts are given the controls at all.
    bool played = true;
    /// None until simulate() makes one.
    std::unique_ptr<PhysicsSystem> physics;
    std::unique_ptr<ScriptSystem> scripts;

private:
    /// An entity whose script does `body` in its update, on the third line of its file.
    Entity addProbe(std::string_view body) {
        const std::string path = "scripts/probe" + std::to_string(++m_probes) + ".lua";
        const std::string source =
            "return {update = function(self)\n    local entity = self.entity\n" + std::string(body) + "\nend}";
        return add("Probe", write(path, source));
    }

    void removeProbe(Entity probe) {
        world.destroyEntity(probe);
        update();
        static_cast<void>(said());
    }

    std::vector<std::string> take(LogLevel level) {
        const std::vector<LogMessage> messages = m_log.messages();
        size_t& taken = level == LogLevel::Info ? m_saidTaken : m_errorsTaken;
        std::vector<std::string> texts;
        size_t seen = 0;
        for (const LogMessage& message : messages) {
            if (message.level != level || message.category != "Script") {
                continue;
            }
            if (seen++ >= taken) {
                texts.push_back(message.text);
            }
        }
        taken = seen;
        return texts;
    }

    LogCapture m_log{LogLevel::Info};
    int m_probes = 0;
    size_t m_saidTaken = 0;
    size_t m_errorsTaken = 0;
};

} // namespace cordite::test
