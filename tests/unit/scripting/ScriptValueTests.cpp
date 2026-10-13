#include "cordite/scripting/ScriptValue.h"

#include "cordite/core/Json.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptRuntime.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace cordite;
using cordite::test::unwrap;

// Inside ScriptRuntime::protect() a test uses CHECK, never REQUIRE or unwrap(): Lua takes
// whatever is thrown through it for an error of a script, and the test would go on.

namespace {

std::unique_ptr<ScriptRuntime> makeRuntime() {
    return unwrap(ScriptRuntime::create());
}

/// What Lua makes of `value`, read back.
Json roundTrip(ScriptRuntime& runtime, const Json& value) {
    Result<Json> back = Json(nullptr);
    const Status status = runtime.protect([&](lua_State* state) {
        pushScriptValue(state, value);
        back = readScriptValue(state, -1);
    });
    REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
    return unwrap(std::move(back));
}

/// What the value that `source` returns is refused with.
std::string refusal(ScriptRuntime& runtime, std::string_view source) {
    const Result<Json> value = runtime.run(source, "value");
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == ErrorCode::InvalidArgument);
    return value.error().message;
}

/// Lua that returns tables nested `levels` deep.
std::string nestedTables(int levels) {
    return "local outermost = {} local innermost = outermost "
           "for level = 2, " +
           std::to_string(levels) +
           " do innermost.inner = {} innermost = innermost.inner end "
           "return outermost";
}

} // namespace

TEST_SUITE("ScriptValue") {
    TEST_CASE("values that are not tables come back as they went") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(roundTrip(*runtime, Json(nullptr)).is_null());
        CHECK(roundTrip(*runtime, Json(true)) == Json(true));
        CHECK(roundTrip(*runtime, Json(false)) == Json(false));
        CHECK(roundTrip(*runtime, Json("")) == Json(""));
        CHECK(roundTrip(*runtime, Json("Lämpchen")) == Json("Lämpchen"));
        // A string of Lua is bytes; a zero among them ends nothing.
        const std::string withZero("a\0b", 3);
        CHECK(roundTrip(*runtime, Json(withZero)).get<std::string>() == withZero);
    }

    TEST_CASE("whole numbers are integers to Lua, and other numbers floats") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        for (const int64_t number : {int64_t{0}, int64_t{-5}, int64_t{42}, std::numeric_limits<int64_t>::max(),
                                     std::numeric_limits<int64_t>::min()}) {
            const Json back = roundTrip(*runtime, Json(number));
            CHECK(back.is_number_integer());
            CHECK(back.get<int64_t>() == number);
        }
        for (const double number : {0.5, -1.0e300, 3.0, 1.0e-300}) {
            const Json back = roundTrip(*runtime, Json(number));
            CHECK(back.is_number_float());
            CHECK(back.get<double>() == number);
        }
        // An unsigned number is an integer while Lua's signed ones hold it.
        const Json small = roundTrip(*runtime, Json(uint64_t{7}));
        CHECK(small.is_number_integer());
        CHECK(small.get<int64_t>() == 7);
        const Json large = roundTrip(*runtime, Json(std::numeric_limits<uint64_t>::max()));
        CHECK(large.is_number_float());
        CHECK(large.get<double>() == 18446744073709551616.0);

        bool wholeIsInteger = false;
        bool fractionIsInteger = true;
        const Status status = runtime->protect([&](lua_State* state) {
            pushScriptValue(state, Json(3));
            wholeIsInteger = lua_isinteger(state, -1) != 0;
            pushScriptValue(state, Json(3.0));
            fractionIsInteger = lua_isinteger(state, -1) != 0;
        });
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(wholeIsInteger);
        CHECK_FALSE(fractionIsInteger);
    }

    TEST_CASE("arrays and objects come back as they went") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        const Json scene = unwrap(parseJson(R"({
            "entities": [
                {"name": "Lamp", "position": [0, 2.5, -1], "tags": ["light", "indoor"]},
                {"name": "Crate", "position": [1, 0, 0], "tags": []}
            ],
            "gravity": -9.81,
            "title": "Cellar"
        })"));
        // An array without elements comes back as an object without members: a table
        // without keys is all Lua has for either. Everything else is as it was.
        Json expected = scene;
        expected["entities"][1]["tags"] = Json::object();
        CHECK(dumpJson(roundTrip(*runtime, scene)) == dumpJson(expected));

        CHECK(roundTrip(*runtime, Json::array({1, 2, 3})) == Json::array({1, 2, 3}));
        CHECK(roundTrip(*runtime, Json::object()) == Json::object());
        CHECK(roundTrip(*runtime, Json::array()) == Json::object());
    }

    TEST_CASE("a member that is null is not in the table, and null in an array is cordite.null") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();
        const Json value = unwrap(parseJson(R"({"mesh": null, "materials": ["a", null, "c"]})"));

        bool meshIsNil = false;
        bool gapIsNull = false;
        bool gapIsNil = true;
        lua_Unsigned materials = 0;
        const Status status = runtime->protect([&](lua_State* state) {
            pushScriptValue(state, value);
            lua_getfield(state, -1, "mesh");
            meshIsNil = lua_isnil(state, -1);
            lua_pop(state, 1);
            lua_getfield(state, -1, "materials");
            materials = lua_rawlen(state, -1);
            lua_rawgeti(state, -1, 2);
            gapIsNull = isScriptNull(state, -1);
            gapIsNil = lua_isnil(state, -1);
        });
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(meshIsNil);
        CHECK(materials == 3);
        CHECK(gapIsNull);
        CHECK_FALSE(gapIsNil);

        // Read back, the array is whole; the member that was null is gone.
        CHECK(dumpJson(roundTrip(*runtime, value), -1) == R"({"materials":["a",null,"c"]})");

        // To a script the value is cordite.null, which is read as null wherever it stands.
        CHECK(unwrap(runtime->run("return cordite.null", "null")).is_null());
        CHECK(dumpJson(unwrap(runtime->run("return {mesh = cordite.null, list = {1, cordite.null}}", "nulls")), -1) ==
              R"({"list":[1,null],"mesh":null})");
        // It is something, unlike nil: a script that asks whether it is there is told yes.
        CHECK(unwrap(runtime->run("return cordite.null ~= nil and cordite.null == cordite.null", "null")) ==
              Json(true));
    }

    TEST_CASE("a table is an array, an object, or refused") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(dumpJson(unwrap(runtime->run("return {10, 20, 30}", "value")), -1) == "[10,20,30]");
        CHECK(dumpJson(unwrap(runtime->run("return {name = 'Lamp', on = true}", "value")), -1) ==
              R"({"name":"Lamp","on":true})");
        CHECK(dumpJson(unwrap(runtime->run("return {}", "value")), -1) == "{}");
        CHECK(dumpJson(unwrap(runtime->run("return {{1, 2}, {x = {}}}", "value")), -1) == R"([[1,2],{"x":{}}])");
        // A whole number written as a float is the position it names.
        CHECK(dumpJson(unwrap(runtime->run("return {[1.0] = 'a', [2.0] = 'b'}", "value")), -1) == R"(["a","b"])");
        // The metatable of a table is not looked at.
        CHECK(dumpJson(unwrap(runtime->run(
                           "return setmetatable({x = 1}, {__index = function() return 2 end, __len = function() "
                           "return 9 end})",
                           "value")),
                       -1) == R"({"x":1})");

        CHECK(refusal(*runtime, "return {1, 2, nil, 4}") ==
              "what value returns: a table can be stored when its keys are 1 to n; this one has nothing at some of "
              "the positions up to 4");
        CHECK(refusal(*runtime, "return {1, 2, x = 3}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has both");
        CHECK(refusal(*runtime, "return {[1.5] = 1}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has a key 1.5");
        CHECK(refusal(*runtime, "return {[0] = 1}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has a key 0");
        CHECK(refusal(*runtime, "return {[-1] = 1}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has a key -1");
        CHECK(refusal(*runtime, "return {[true] = 1}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has a key "
              "that is a boolean");
        CHECK(refusal(*runtime, "return {[{}] = 1}") ==
              "what value returns: a table can be stored when its keys are 1 to n, or names; this one has a key "
              "that is a table");
    }

    TEST_CASE("what JSON cannot hold is refused, with where in the value it is") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(refusal(*runtime, "return function() end") == "what value returns: a function cannot be stored");
        CHECK(refusal(*runtime, "return coroutine.create(function() end)") ==
              "what value returns: a thread cannot be stored");
        CHECK(refusal(*runtime, "return 1 / 0") == "what value returns: a number that is not finite cannot be stored");
        CHECK(refusal(*runtime, "return -1 / 0") == "what value returns: a number that is not finite cannot be stored");
        CHECK(refusal(*runtime, "return 0 / 0") == "what value returns: a number that is not finite cannot be stored");
        CHECK(refusal(*runtime, "return {door = {open = print}}") ==
              "what value returns: 'door': 'open': a function cannot be stored");
        CHECK(refusal(*runtime, "return {1, {2, 0 / 0}}") ==
              "what value returns: element 2: element 2: a number that is not finite cannot be stored");
    }

    TEST_CASE("tables are read to a depth, which one that holds itself exceeds") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();
        const auto depth = static_cast<int>(kMaxScriptValueDepth);

        CHECK(unwrap(runtime->run(nestedTables(depth), "value")).is_object());
        const std::string tooDeep =
            fmt::format("tables are nested more than {} deep, or one of them holds itself", kMaxScriptValueDepth);
        CHECK(refusal(*runtime, nestedTables(depth + 1)).ends_with(tooDeep));
        CHECK(refusal(*runtime, "local ring = {} ring.self = ring return ring").ends_with(tooDeep));
        CHECK(refusal(*runtime, "local a, b = {}, {} a[1] = b b[1] = a return a").ends_with(tooDeep));
    }

    TEST_CASE("the same table is the same JSON every time") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        const Json sorted =
            unwrap(runtime->run("return {zebra = 1, apple = 2, mango = 3, Banana = 4, _hidden = 5}", "value"));
        CHECK(dumpJson(sorted, -1) == R"({"Banana":4,"_hidden":5,"apple":2,"mango":3,"zebra":1})");
    }

    TEST_CASE("reading a value leaves the stack as it is, also when the value is refused") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        for (const std::string_view source :
             {"return {1, 2, {a = {b = 'c'}}}", "return {a = 1, b = function() end, c = 3}",
              "return {1, function() end, 3}", "return {1, 2, x = 3}", "return {[true] = 1}", "return 5"}) {
            const std::string text(source);
            CAPTURE(text);
            int before = 0;
            int after = -1;
            const Status status = runtime->protect([&](lua_State* state) {
                ScriptRuntime::runChunk(state, source, "=value", 1);
                before = lua_gettop(state);
                const Result<Json> ignored = readScriptValue(state, -1);
                after = lua_gettop(state);
            });
            REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
            CHECK(before == 1);
            CHECK(after == before);
        }
    }
}
