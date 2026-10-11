#include "cordite/editor/CommandRegistry.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

using Type = CommandParameterType;

/// A command that returns the parameters it was given.
CommandDesc echo(std::string name, std::vector<CommandParameter> parameters = {}) {
    return {.name = std::move(name),
            .description = "Returns its parameters.",
            .parameters = std::move(parameters),
            .handler = [](const Json& received) -> Result<Json> { return received; }};
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

} // namespace

TEST_SUITE("CommandRegistry") {
    TEST_CASE("a registered command can be found and executed") {
        CommandRegistry commands;
        REQUIRE(commands.add(echo("test.echo", {{"text", Type::String, "What to return."}})).has_value());

        const CommandDesc* command = commands.find("test.echo");
        REQUIRE(command != nullptr);
        CHECK(command->description == "Returns its parameters.");
        CHECK(commands.find("test.missing") == nullptr);

        const Json result = unwrap(commands.execute("test.echo", json(R"({"text": "hello"})")));
        CHECK(result == json(R"({"text": "hello"})"));
    }

    TEST_CASE("commands are listed sorted by name") {
        CommandRegistry commands;
        for (const char* name : {"scene.save", "entity.create", "scene.new", "editor.undo"}) {
            REQUIRE(commands.add(echo(name)).has_value());
        }

        std::vector<std::string> names;
        for (const CommandDesc& command : commands.commands()) {
            names.push_back(command.name);
        }
        CHECK(names == std::vector<std::string>{"editor.undo", "entity.create", "scene.new", "scene.save"});
    }

    TEST_CASE("names must be dotted lowercase words") {
        CommandRegistry commands;
        for (const char* name : {"entity.create", "entity.setParent", "a.b.c", "viewport.frame2"}) {
            CAPTURE(name);
            CHECK(commands.add(echo(name)).has_value());
        }
        for (const char* name : {"", "create", "Entity.create", "entity.", ".create", "entity..create", "entity.Create",
                                 "entity create", "entity.2d", "entity.cre-ate"}) {
            CAPTURE(name);
            const Status status = commands.add(echo(name));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
        }
    }

    TEST_CASE("a name can be registered only once") {
        CommandRegistry commands;
        REQUIRE(commands.add(echo("test.echo")).has_value());
        const Status again = commands.add(echo("test.echo"));
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::AlreadyExists);
        CHECK(commands.commands().size() == 1);
    }

    TEST_CASE("a command needs a handler and distinct, named parameters") {
        CommandRegistry commands;
        CHECK_FALSE(
            commands.add({.name = "test.none", .description = "", .parameters = {}, .handler = nullptr}).has_value());
        CHECK_FALSE(
            commands.add(echo("test.twice", {{"value", Type::Number, ""}, {"value", Type::String, ""}})).has_value());
        CHECK_FALSE(commands.add(echo("test.unnamed", {{"", Type::Number, ""}})).has_value());
        CHECK(commands.commands().empty());
    }

    TEST_CASE("an unknown command is reported as not found") {
        const CommandRegistry commands;
        const Result<Json> result = commands.execute("test.missing");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::NotFound);
        CHECK(result.error().message.find("test.missing") != std::string::npos);
    }

    TEST_CASE("parameters are validated before the handler runs") {
        CommandRegistry commands;
        int calls = 0;
        REQUIRE(commands
                    .add({.name = "test.count",
                          .description = "Counts calls.",
                          .parameters = {{"name", Type::String, "Required."},
                                         {"amount", Type::Integer, "Optional.", false}},
                          .handler = [&calls](const Json& /*received*/) -> Result<Json> {
                              ++calls;
                              return Json(nullptr);
                          }})
                    .has_value());

        struct Case {
            const char* parameters;
            const char* fragment;
        };

        for (const Case& entry : {
                 Case{R"({})", "missing parameter 'name'"},
                 Case{R"({"amount": 3})", "missing parameter 'name'"},
                 Case{R"({"name": 5})", "parameter 'name' must be of type string, found number"},
                 Case{R"({"name": null})", "parameter 'name' must be of type string, found null"},
                 Case{R"({"name": "x", "amount": "many"})", "parameter 'amount' must be of type integer"},
                 Case{R"({"name": "x", "amount": 1.5})", "parameter 'amount' must be of type integer"},
                 Case{R"({"name": "x", "nmae": "y"})", "unknown parameter 'nmae'"},
                 Case{R"([1, 2])", "parameters must be a JSON object, found array"},
                 Case{R"("name")", "parameters must be a JSON object"},
             }) {
            CAPTURE(entry.parameters);
            const Result<Json> result = commands.execute("test.count", json(entry.parameters));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::InvalidArgument);
            CHECK(result.error().message.find(entry.fragment) != std::string::npos);
            // Errors say which command they came from.
            CHECK(result.error().message.find("test.count: ") == 0);
        }
        CHECK(calls == 0);

        for (const char* parameters : {R"({"name": "x"})", R"({"name": "x", "amount": 3})",
                                       R"({"name": "x", "amount": 3.0})", R"({"name": "x", "amount": null})"}) {
            CAPTURE(parameters);
            CHECK(commands.execute("test.count", json(parameters)).has_value());
        }
        CHECK(calls == 4);
    }

    TEST_CASE("every parameter type accepts its JSON type and nothing else") {
        CommandRegistry commands;
        REQUIRE(commands
                    .add(echo("test.types", {{"string", Type::String, "", false},
                                             {"number", Type::Number, "", false},
                                             {"integer", Type::Integer, "", false},
                                             {"boolean", Type::Boolean, "", false},
                                             {"object", Type::Object, "", false},
                                             {"array", Type::Array, "", false},
                                             {"any", Type::Any, "", false}}))
                    .has_value());

        CHECK(commands
                  .execute("test.types", json(R"({"string": "s", "number": 1.5, "integer": -2, "boolean": true,
                                                 "object": {}, "array": [], "any": [1, "two", null]})"))
                  .has_value());
        CHECK(commands.execute("test.types", json(R"({"number": 7, "any": "text"})")).has_value());

        for (const char* parameters : {R"({"string": 1})", R"({"number": "1"})", R"({"integer": 0.5})",
                                       R"({"boolean": 0})", R"({"object": []})", R"({"array": {}})"}) {
            CAPTURE(parameters);
            CHECK_FALSE(commands.execute("test.types", json(parameters)).has_value());
        }
    }

    TEST_CASE("a command without parameters can be called with none, null or an empty object") {
        CommandRegistry commands;
        REQUIRE(commands.add(echo("test.none")).has_value());

        CHECK(unwrap(commands.execute("test.none")) == Json::object());
        CHECK(unwrap(commands.execute("test.none", nullptr)) == Json::object());
        CHECK(unwrap(commands.execute("test.none", Json::object())) == Json::object());
        CHECK_FALSE(commands.execute("test.none", json(R"({"extra": 1})")).has_value());
    }

    TEST_CASE("a failing handler reports its error with the command name") {
        CommandRegistry commands;
        REQUIRE(commands
                    .add({.name = "test.fail",
                          .description = "Always fails.",
                          .parameters = {},
                          .handler = [](const Json& /*received*/) -> Result<Json> {
                              return makeError(ErrorCode::NotFound, "nothing here");
                          }})
                    .has_value());

        const Result<Json> result = commands.execute("test.fail");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::NotFound);
        CHECK(result.error().message == "test.fail: nothing here");
    }

    TEST_CASE("the catalogue describes every command and parameter") {
        CommandRegistry commands;
        REQUIRE(commands
                    .add(echo("entity.rename", {{"entity", Type::String, "Which entity."},
                                                {"name", Type::String, "The new name."},
                                                {"index", Type::Integer, "Where to put it.", false}}))
                    .has_value());
        REQUIRE(commands.add(echo("editor.undo")).has_value());

        const Json catalogue = commands.describe();
        REQUIRE(catalogue.size() == 2);
        CHECK(catalogue[0]["name"] == "editor.undo");
        CHECK(catalogue[0]["parameters"] == Json::array());

        const Json& rename = catalogue[1];
        CHECK(rename["name"] == "entity.rename");
        CHECK(rename["description"] == "Returns its parameters.");
        REQUIRE(rename["parameters"].size() == 3);
        CHECK(rename["parameters"][0] ==
              json(R"({"name": "entity", "type": "string", "required": true, "description": "Which entity."})"));
        CHECK(rename["parameters"][2]["type"] == "integer");
        CHECK(rename["parameters"][2]["required"] == false);
    }

    TEST_CASE("parameter type names are stable") {
        CHECK(nameOf(Type::String) == "string");
        CHECK(nameOf(Type::Number) == "number");
        CHECK(nameOf(Type::Integer) == "integer");
        CHECK(nameOf(Type::Boolean) == "boolean");
        CHECK(nameOf(Type::Object) == "object");
        CHECK(nameOf(Type::Array) == "array");
        CHECK(nameOf(Type::Any) == "any");
    }
}
