#include "cordite/editor/ScriptCommands.h"

#include "cordite/core/Json.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"

#include "unit/editor/EditorTestSupport.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::json;
using cordite::test::unwrap;

namespace {

/// A script system as the engine has one, and the commands about it.
struct ScriptFixture {
    std::unique_ptr<ScriptSystem> scripts = unwrap(ScriptSystem::create());
    CommandRegistry commands;

    ScriptFixture() { REQUIRE(registerScriptCommands(commands, *scripts).has_value()); }

    Json run(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));
        return *result;
    }
};

/// The entry of a list that has the name, or null.
Json named(const Json& list, std::string_view name) {
    for (const Json& entry : list) {
        if (entry["name"].get<std::string>() == name) {
            return entry;
        }
    }
    return {};
}

} // namespace

TEST_SUITE("ScriptCommands") {
    TEST_CASE("script.api says everything a script can call, each with how and what for") {
        ScriptFixture fixture;
        const Json api = fixture.run("script.api");

        // It is the catalogue of the runtime, whole and in its order.
        const std::vector<ScriptFunctionInfo> registered = fixture.scripts->runtime().functions();
        REQUIRE(api["functions"].size() == registered.size());
        REQUIRE(registered.size() > 70);
        for (size_t index = 0; index < registered.size(); ++index) {
            const Json& function = api["functions"][index];
            CAPTURE(registered[index].name);
            CHECK(function["name"] == registered[index].name);
            CHECK(function["signature"] == registered[index].signature);
            CHECK(function["description"] == registered[index].description);
            // Nothing else: how often something was called is no part of what it is.
            CHECK(function.size() == 3);
        }
        CHECK(named(api["functions"], "cordite.world.find") == json(R"({"name": "cordite.world.find",
                       "signature": "cordite.world.find(name) -> Entity or nil",
                       "description": "The first entity of that name, in the order of the hierarchy; nil when there is none."})"));
    }

    TEST_CASE("script.api says what the engine calls in a script, in the order of a frame") {
        ScriptFixture fixture;
        const Json callbacks = fixture.run("script.api")["callbacks"];

        std::string names;
        for (const Json& callback : callbacks) {
            const std::string name = callback["name"].get<std::string>();
            CAPTURE(name);
            names += (names.empty() ? "" : " ") + name;
            // Written as a script writes it, and said in a sentence that ends.
            CHECK(callback["signature"].get<std::string>().starts_with("function Script:" + name + "("));
            CHECK(callback["description"].get<std::string>().size() > 40);
            CHECK(callback["description"].get<std::string>().back() == '.');
            CHECK(callback.size() == 3);
        }
        CHECK(names == "start input update sequenceEvent collisionBegan collisionEnded triggerEntered triggerLeft "
                       "destroy");
        CHECK(named(callbacks, "update")["signature"] == "function Script:update(deltaSeconds)");
        CHECK(named(callbacks, "collisionBegan")["signature"] == "function Script:collisionBegan(other, contact)");
    }

    TEST_CASE("script.api can be asked for the functions of one library") {
        ScriptFixture fixture;
        const Json physics = fixture.run("script.api", R"({"prefix": "cordite.physics."})");
        std::string names;
        for (const Json& function : physics["functions"]) {
            names += (names.empty() ? "" : " ") + function["name"].get<std::string>().substr(16);
        }
        CHECK(names == "raycast sphereCast overlapSphere overlapBox gravity setGravity");
        // The callbacks are no functions of a library, and come along whatever is asked.
        CHECK(physics["callbacks"].size() == 9);

        CHECK(fixture.run("script.api", R"({"prefix": "cordite.Entity.add"})")["functions"].size() == 4);
        CHECK(fixture.run("script.api", R"({"prefix": "no such library"})")["functions"].empty());
        CHECK(fixture.run("script.api", R"({"prefix": ""})")["functions"].size() ==
              fixture.scripts->runtime().functions().size());

        const Result<Json> wrong = fixture.commands.execute("script.api", json(R"({"prefix": 5})"));
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == ErrorCode::InvalidArgument);
    }
}
