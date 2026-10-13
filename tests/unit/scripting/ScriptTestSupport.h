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
/// @endcode

#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
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
inline constexpr float kScriptFrame = 0.5f;

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

    [[nodiscard]] ScriptContext context(float deltaSeconds = kScriptFrame) {
        return {.world = world, .components = components, .assets = *assets, .deltaSeconds = deltaSeconds};
    }

    ScriptStatistics update(float deltaSeconds = kScriptFrame) { return scripts->update(context(deltaSeconds)); }

    /// What the scripts have printed since this was last asked, in order.
    std::vector<std::string> said() { return take(LogLevel::Info); }

    /// What was logged as an error since this was last asked.
    std::vector<std::string> errors() { return take(LogLevel::Error); }

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
    std::unique_ptr<ScriptSystem> scripts;

private:
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
    size_t m_saidTaken = 0;
    size_t m_errorsTaken = 0;
};

} // namespace cordite::test
