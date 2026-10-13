#include "cordite/scripting/ScriptRuntime.h"

#include "cordite/core/Base.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/scripting/Lua.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::unwrap;

// Inside ScriptRuntime::protect(), and in a function that a script calls, a test uses
// CHECK, never REQUIRE or unwrap(): Lua takes whatever is thrown through it for an error
// of a script, and the test would go on.

namespace {

/// A limit small enough that a script which never ends is stopped at once.
constexpr uint64_t kFewInstructions = 100'000;

std::unique_ptr<ScriptRuntime> makeRuntime(const ScriptRuntimeDesc& desc = {}) {
    return unwrap(ScriptRuntime::create(desc));
}

bool contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

/// What running `source` fails with.
std::string failure(ScriptRuntime& runtime, std::string_view source, std::string_view name = "test") {
    const Result<Json> result = runtime.run(source, name);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::ScriptError);
    return result.error().message;
}

/// What `source` returns, written out.
std::string value(ScriptRuntime& runtime, std::string_view source) {
    return dumpJson(unwrap(runtime.run(source, "test")), -1);
}

/// What the functions below leave behind for their test to look at, found through
/// ScriptRuntime::host().
struct Witness {
    /// Set by the destructor of something that a function had on its stack when an error
    /// went through it.
    bool destroyed = false;
    /// The runtime a function found through the state it was given.
    ScriptRuntime* found = nullptr;
    /// How much was on the stack of a function before and after it called the scripts.
    int topBefore = -1;
    int topAfter = -1;
};

Witness& witness(lua_State* state) {
    return *static_cast<Witness*>(ScriptRuntime::of(state).host());
}

int add(lua_State* state) {
    lua_pushinteger(state, luaL_checkinteger(state, 1) + luaL_checkinteger(state, 2));
    return 1;
}

/// Where the stack of the program is at the moment, as a number.
int stackAddress(lua_State* state) {
    const volatile char here = 0;
    lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(&here)));
    return 1;
}

int trip(lua_State* /*state*/) {
    throw std::runtime_error("the engine tripped");
}

/// Raises a Lua error with something on its stack that has a destructor.
int failWithSomethingToDestroy(lua_State* state) {
    struct Flag {
        explicit Flag(bool& destroyed) : m_destroyed(destroyed) {}

        ~Flag() { m_destroyed = true; }

        Flag(const Flag&) = delete;
        Flag& operator=(const Flag&) = delete;

    private:
        bool& m_destroyed;
    };

    const Flag flag(witness(state).destroyed);
    return luaL_error(state, "left in a hurry");
}

/// Calls its argument the way the engine calls a script from inside one of its own
/// functions: through the runtime, with the function kept where Lua keeps what C holds.
int callBack(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);
    ScriptRuntime& runtime = ScriptRuntime::of(state);
    Witness& seen = witness(state);
    seen.found = &runtime;
    lua_pushvalue(state, 1);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    seen.topBefore = lua_gettop(state);
    const Status called = runtime.protect([&](lua_State* inner) {
        lua_rawgeti(inner, LUA_REGISTRYINDEX, reference);
        lua_call(inner, 0, 0);
    });
    seen.topAfter = lua_gettop(state);
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    if (!called) {
        return luaL_error(state, "%s", called.error().message.c_str());
    }
    return 0;
}

constexpr std::array kTestFunctions = {
    ScriptFunction{"add", "add(a, b) -> integer", "The sum of two whole numbers.", add},
    ScriptFunction{"trip", "trip()", "Throws, as a function of the engine must not but might.", trip},
    ScriptFunction{"stack", "stack() -> integer", "Where the stack of the program is at the moment.", stackAddress},
    ScriptFunction{"fail", "fail()", "Raises an error past something that has a destructor.",
                   failWithSomethingToDestroy},
    ScriptFunction{"call", "call(function)", "Calls a function through the runtime.", callBack},
};

/// A runtime that has the functions above as the library `test`.
std::unique_ptr<ScriptRuntime> makeRuntimeWithFunctions(Witness& seen, const ScriptRuntimeDesc& desc = {}) {
    std::unique_ptr<ScriptRuntime> runtime = makeRuntime(desc);
    runtime->setHost(&seen);
    const Status registered = runtime->registerLibrary("test", kTestFunctions);
    REQUIRE_MESSAGE(registered.has_value(), test::errorOf(registered));
    return runtime;
}

} // namespace

TEST_SUITE("ScriptRuntime") {
    TEST_CASE("a piece of Lua is run for what it returns") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(value(*runtime, "return 2 * 3") == "6");
        CHECK(value(*runtime, "return 'door' .. ' ' .. 'open'") == R"("door open")");
        CHECK(value(*runtime, "local position = {x = 1, y = 2} return position") == R"({"x":1,"y":2})");
        CHECK(value(*runtime, "local unused = 1") == "null");
        CHECK(value(*runtime, "") == "null");
        // Of several values the first.
        CHECK(value(*runtime, "return 1, 2, 3") == "1");
        // Numbers in hexadecimal are read and written by Lua's own code, the same everywhere.
        CHECK(value(*runtime, "return {0x1.8p3, tonumber('0x.1p4'), string.format('%a %A', 1.5, -0.25)}") ==
              R"([12.0,1.0,"0x1.8p+0 -0X1P-2"])");
        // Lua 5.4: integers and floats are different numbers, and there is a floor division.
        CHECK(value(*runtime, "return {7 // 2, 7 / 2, math.type(1), math.type(1.0), _VERSION}") ==
              R"([3,3.5,"integer","float","Lua 5.4"])");
    }

    TEST_CASE("text that is not Lua is an error that names the chunk and the line") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        // Nothing has run yet, so there are no calls to list.
        CHECK(failure(*runtime, "local x = 1\nreturn +", "broken") == "broken:2: unexpected symbol near '+'");
        CHECK(failure(*runtime, "x = = 2", "broken") == "broken:1: unexpected symbol near '='");
        CHECK(failure(*runtime, "return (", "broken") == "broken:1: unexpected symbol near <eof>");
    }

    TEST_CASE("an error in a script says where it was and what led there") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        const std::string message = failure(*runtime,
                                            "local function inner() error('the door is stuck') end\n"
                                            "local function outer() inner() end\n"
                                            "outer()\n",
                                            "door");
        CHECK_MESSAGE(message.starts_with("door:1: the door is stuck\nstack traceback:"), message);
        CHECK_MESSAGE(contains(message, "door:1: in upvalue 'inner'"), message);
        CHECK_MESSAGE(contains(message, "door:2: in local 'outer'"), message);
        CHECK_MESSAGE(contains(message, "door:3: in main chunk"), message);

        // What Lua itself objects to is said the same way.
        CHECK(failure(*runtime, "local door = nil\nreturn door.open", "door")
                  .starts_with("door:2: attempt to index a nil value (local 'door')"));
        CHECK(failure(*runtime, "return 1 + {}", "sum").starts_with("sum:1: attempt to perform arithmetic on a table"));
        CHECK(failure(*runtime, "return #5", "length").starts_with("length:1: attempt to get length of a number"));
    }

    TEST_CASE("an error that is no message is described all the same") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(failure(*runtime, "error({code = 7})").starts_with("(the error is a table value, not a message)"));
        CHECK(failure(*runtime, "error()").starts_with("(the error is a nil value, not a message)"));
        CHECK(failure(*runtime, "error(true)").starts_with("(the error is a boolean value, not a message)"));
        // A number is as good as a message, and so is what says of itself how it is written.
        CHECK(failure(*runtime, "error(42, 0)").starts_with("42\nstack traceback:"));
        CHECK(failure(*runtime, "error(setmetatable({}, {__tostring = function() return 'jammed' end}))")
                  .starts_with("jammed\nstack traceback:"));
    }

    TEST_CASE("the runtime goes on after an error") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        for (int round = 0; round < 3; ++round) {
            CHECK(contains(failure(*runtime, "error('again')"), "again"));
            CHECK(contains(failure(*runtime, "return ("), "unexpected symbol"));
            CHECK(value(*runtime, "return 'fine'") == R"("fine")");
        }
    }

    TEST_CASE("a script that does not end is stopped, and told where it was") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.instructionLimit = kFewInstructions});

        const std::string message =
            failure(*runtime, "local turns = 0\nwhile true do\n    turns = turns + 1\nend", "loop");
        CHECK_MESSAGE(message.starts_with("loop:"), message);
        CHECK_MESSAGE(
            contains(message, ": ran for more than 100000 instructions without coming to an end, and was stopped"),
            message);
        CHECK_MESSAGE(contains(message, "stack traceback:"), message);
        CHECK_MESSAGE(contains(message, "in main chunk"), message);
        CHECK(runtime->instructions() >= kFewInstructions);
    }

    TEST_CASE("every call into the scripts has the whole limit to itself") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.instructionLimit = kFewInstructions});

        // Sixty thousand turns of an empty loop are sixty thousand instructions: within
        // the limit, however often they are run.
        for (int round = 0; round < 5; ++round) {
            CHECK(value(*runtime, "local last = 0 for turn = 1, 60000 do end return 'done'") == R"("done")");
        }
        CHECK(runtime->instructions() >= 5 * 60000);
        // Twice as many in one call are not.
        CHECK(contains(failure(*runtime, "for turn = 1, 120000 do end"), "ran for more than 100000 instructions"));
    }

    TEST_CASE("the error that stops a script cannot be caught by it") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.instructionLimit = kFewInstructions});
        const std::string_view endless = "local function endless() while true do end end\n";

        for (const std::string_view attempt : {
                 "while true do pcall(endless) end",
                 "while true do xpcall(endless, function(message) return message end) end",
                 "xpcall(endless, endless)",
                 "while true do coroutine.resume(coroutine.create(endless)) end",
                 "local wrapped = coroutine.wrap(endless) while true do pcall(wrapped) end",
                 "return pcall(endless)",
                 "return select(2, pcall(endless))",
                 "return coroutine.resume(coroutine.create(endless))",
                 "return pcall(pcall, pcall, endless)",
                 "local closing <close> = setmetatable({}, {__close = endless}) endless()",
                 "return pcall(string.gsub, 'abc', '.', endless)",
                 "return pcall(table.sort, {3, 2, 1}, endless)",
                 // Closing a coroutine runs what it still has to close, and says how that went.
                 "local sleeper = coroutine.create(function()\n"
                 "    local closing <close> = setmetatable({}, {__close = endless})\n"
                 "    coroutine.yield()\n"
                 "end)\n"
                 "coroutine.resume(sleeper)\n"
                 "return coroutine.close(sleeper)",
             }) {
            const std::string source = std::string(endless) + std::string(attempt);
            CAPTURE(source);
            const std::string message = failure(*runtime, source, "escape");
            CHECK_MESSAGE(contains(message, "ran for more than 100000 instructions"), message);
            // What is reported is where the script first ran out of time, not what it made of it.
            CHECK_MESSAGE(message.starts_with("escape:1: ran for more than"), message);
        }
        CHECK(value(*runtime, "return 'fine'") == R"("fine")");
    }

    TEST_CASE("a script catches its own errors as Lua has it") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.instructionLimit = kFewInstructions});

        CHECK(value(*runtime, "return {pcall(error, 'stuck', 0)}") == R"([false,"stuck"])");
        CHECK(value(*runtime, "return {pcall(function(a, b) return a + b, a * b end, 3, 4)}") == "[true,7,12]");
        CHECK(value(*runtime, "return {pcall(function() end)}") == "[true]");
        CHECK(
            value(*runtime, "return {xpcall(error, function(message) return 'handled ' .. message end, 'stuck', 0)}") ==
            R"([false,"handled stuck"])");
        CHECK(value(*runtime, "return {xpcall(function(a) return a * 2 end, print, 21)}") == "[true,42]");
        CHECK(value(*runtime, "return {coroutine.resume(coroutine.create(function() error('stuck', 0) end))}") ==
              R"([false,"stuck"])");
        CHECK(value(*runtime, "return {coroutine.resume(coroutine.create(function(a) return a + 1 end), 1)}") ==
              "[true,2]");
        CHECK(contains(failure(*runtime, "pcall()"), "bad argument #1 to 'pcall' (value expected)"));
        CHECK(contains(failure(*runtime, "xpcall(print)"),
                       "bad argument #2 to 'xpcall' (function expected, got no value)"));
        CHECK(failure(*runtime, "coroutine.resume(5)", "wake")
                  .starts_with("wake:1: bad argument #1 to 'resume' (coroutine expected, got number)"));

        // A coroutine may yield from inside a protected call, and come back into it.
        CHECK(value(*runtime,
                    "local step = coroutine.wrap(function()\n"
                    "    local ok, sum = pcall(function() return coroutine.yield('waiting') + 1 end)\n"
                    "    coroutine.yield(ok and sum)\n"
                    "    local caught, message = xpcall(function() coroutine.yield('again') error('late', 0) end,\n"
                    "                                    function(text) return 'caught ' .. text end)\n"
                    "    return message\n"
                    "end)\n"
                    "return {step(), step(41), step(), step()}") == R"(["waiting",42,"again","caught late"])");
    }

    TEST_CASE("a function of the engine that calls back into the scripts counts against the same limit") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime =
            makeRuntimeWithFunctions(seen, {.instructionLimit = kFewInstructions});
        const std::string_view work = "local function work() for turn = 1, 60000 do end end\n";

        CHECK(value(*runtime, std::string(work) + "test.call(work) return 'once'") == R"("once")");
        const std::string message = failure(*runtime, std::string(work) + "test.call(work) test.call(work)");
        CHECK_MESSAGE(contains(message, "ran for more than 100000 instructions"), message);
        // Nor does a script get past the limit by catching what the function passes on.
        CHECK(
            contains(failure(*runtime, std::string(work) + "test.call(work) while true do pcall(test.call, work) end"),
                     "ran for more than 100000 instructions"));
    }

    TEST_CASE("instructions are not counted when there is no limit") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.instructionLimit = 0});

        CHECK(value(*runtime, "local sum = 0 for turn = 1, 300000 do sum = sum + turn end return sum") ==
              "45000150000");
        CHECK(runtime->instructions() == 0);
    }

    TEST_CASE("a script that takes all the memory there is is stopped") {
        constexpr size_t kLimit = size_t{8} * 1024 * 1024;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime({.memoryLimitBytes = kLimit, .instructionLimit = 0});
        const size_t atRest = runtime->memoryBytes();
        CHECK(atRest > 0);
        CHECK(atRest < kLimit);

        const std::string message =
            failure(*runtime, "local hoard = {} while true do hoard[#hoard + 1] = string.rep('x', 4096) .. #hoard end");
        CHECK_MESSAGE(message.starts_with("out of memory: the scripts hold "), message);
        CHECK_MESSAGE(message.ends_with(" of the 8388608 bytes they may"), message);
        CHECK(runtime->memoryBytes() <= kLimit);

        // One allocation that is too large is refused like many small ones.
        CHECK(failure(*runtime, "return #string.rep('x', 64 * 1024 * 1024)").starts_with("out of memory"));

        // What was hoarded is given back, and the runtime goes on.
        CHECK(value(*runtime, "return #string.rep('x', 1024 * 1024)") == "1048576");
        CHECK(value(*runtime, "local hoard = {} for turn = 1, 1000 do hoard[turn] = {turn} end return #hoard") ==
              "1000");

        // A script may catch it, as any error of its own.
        CHECK(value(*runtime, "local hoard = {}\n"
                              "local ok, message = pcall(function()\n"
                              "    while true do hoard[#hoard + 1] = string.rep('x', 4096) .. #hoard end\n"
                              "end)\n"
                              "hoard = nil\n"
                              "return {ok, message}") == R"([false,"not enough memory"])");
    }

    TEST_CASE("a runtime whose memory does not hold Lua itself is not made") {
        // Not even the state:
        for (const size_t limit : {size_t{0}, size_t{1000}}) {
            CAPTURE(limit);
            const Result<std::unique_ptr<ScriptRuntime>> runtime = ScriptRuntime::create({.memoryLimitBytes = limit});
            REQUIRE_FALSE(runtime.has_value());
            CHECK(runtime.error().code == ErrorCode::InvalidArgument);
            CHECK(runtime.error().message ==
                  fmt::format("the {} bytes that the scripts may hold do not hold Lua itself", limit));
        }
        // The state, but not the libraries:
        const Result<std::unique_ptr<ScriptRuntime>> bare = ScriptRuntime::create({.memoryLimitBytes = 8192});
        REQUIRE_FALSE(bare.has_value());
        CHECK(bare.error().code == ErrorCode::InvalidArgument);
        const std::string said = bare.error().message;
        CHECK_MESSAGE(said.starts_with("the 8192 bytes that the scripts may hold do not hold Lua itself: out of "
                                       "memory: the scripts hold "),
                      said);
        // What does hold it is not much.
        CHECK(ScriptRuntime::create({.memoryLimitBytes = size_t{32} * 1024}).has_value());
    }

    TEST_CASE("what reaches the machine is not there") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        for (const std::string_view name : {"io", "os", "package", "debug", "require", "dofile", "loadfile", "load",
                                            "loadstring", "collectgarbage"}) {
            const std::string source = "return " + std::string(name);
            CHECK(failure(*runtime, source, "reach")
                      .starts_with(
                          fmt::format("reach:1: '{}' is not defined: nothing was ever given that name\n", name)));
        }
        CHECK(value(*runtime, "return string.dump == nil") == "true");

        // What is there: the libraries that compute, and the basic functions.
        CHECK(value(*runtime, "local kinds = {}\n"
                              "for _, thing in ipairs({coroutine.create, table.concat, string.format, math.floor,\n"
                              "                         utf8.char, pcall, xpcall, select, tostring, tonumber, type,\n"
                              "                         next, pairs, ipairs, rawget, rawset, rawequal, rawlen,\n"
                              "                         setmetatable, getmetatable, assert, error, print, warn}) do\n"
                              "    kinds[#kinds + 1] = type(thing)\n"
                              "end\n"
                              "return {#kinds, (table.concat(kinds, ' '):gsub('function ?', ''))}") == R"([24,""])");
    }

    TEST_CASE("only text is compiled") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        // A chunk that says it is compiled already - the first bytes of one are these -
        // is not even looked at: Lua would trust it.
        const std::string compiled("\x1bLua\x54\x00\x19\x93\r\n\x1a\n", 12);
        CHECK(failure(*runtime, compiled, "compiled") == "compiled: attempt to load a binary chunk (mode is 't')");
    }

    TEST_CASE("the variables of a script are its own") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(value(*runtime, "shared = 5 return shared") == "5");
        CHECK(contains(failure(*runtime, "return shared"), "'shared' is not defined"));
        // Not even what the engine provides is replaced for anyone else.
        CHECK(value(*runtime, "print = 'mine' return print") == R"("mine")");
        CHECK(value(*runtime, "return type(print)") == R"("function")");
        // _G is the script's own environment, and closed to prying.
        CHECK(value(*runtime, "answer = 42 return {_G.answer, _G == _ENV, _G._G == _G, getmetatable(_G)}") ==
              "[42,true,true,false]");
        CHECK(contains(failure(*runtime, "setmetatable(_G, {})"), "cannot change a protected metatable"));
    }

    TEST_CASE("a name that was never given a value is a mistake, read or written") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(failure(*runtime, "local health = 3\nreturn heatlh", "typo")
                  .starts_with("typo:2: 'heatlh' is not defined: nothing was ever given that name"));
        CHECK(failure(*runtime, "return undefined.field", "typo").starts_with("typo:1: 'undefined' is not defined"));
        CHECK(failure(*runtime, "missing()", "typo").starts_with("typo:1: 'missing' is not defined"));
        // Whoever means to ask whether there is such a thing asks without going through that.
        CHECK(value(*runtime, "return rawget(_G, 'heatlh') == nil") == "true");

        // While a chunk loads it makes the globals it wants; once it has run, a name it
        // did not make then is not made later.
        std::string slip;
        int64_t counted = 0;
        const Status status = runtime->protect([&](lua_State* state) {
            ScriptRuntime::runChunk(state,
                                    "counter = 0\n"
                                    "function count() counter = counter + 1 return counter end\n"
                                    "function slip() cuonter = counter + 1 end\n"
                                    "function insist() rawset(_G, 'late', true) return late end\n"
                                    "return {count = count, slip = slip, insist = insist}",
                                    "@scripts/counter.lua", 1);
            lua_getfield(state, -1, "count");
            lua_call(state, 0, 0);
            lua_getfield(state, -1, "count");
            lua_call(state, 0, 1);
            counted = lua_tointeger(state, -1);
            lua_pop(state, 1);
            // rawset() is the way around, for whoever means it.
            lua_getfield(state, -1, "insist");
            lua_call(state, 0, 1);
            CHECK(lua_toboolean(state, -1) != 0);
            lua_pop(state, 1);

            lua_getfield(state, -1, "slip");
            if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
                slip = lua_tostring(state, -1);
                lua_pop(state, 1);
            }
        });
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(counted == 2);
        CHECK(slip ==
              "scripts/counter.lua:3: 'cuonter' is given a value but was never declared: is a 'local' missing?");
    }

    TEST_CASE("a script cannot give anything a finalizer") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        CHECK(failure(*runtime, "setmetatable({}, {__gc = function() end})", "final")
                  .starts_with("final:1: a script cannot give anything a finalizer ('__gc')"));
        // Everything else a metatable does is as Lua has it.
        CHECK(value(*runtime,
                    "local thing = {}\n"
                    "local same = setmetatable(thing, {__index = function(_, key) return key .. '!' end})\n"
                    "return {same == thing, thing.door, getmetatable(thing) ~= nil}") == R"([true,"door!",true])");
        CHECK(value(*runtime, "return getmetatable(setmetatable({}, nil)) == nil") == "true");
        CHECK(contains(failure(*runtime, "setmetatable(1, {})"), "bad argument #1 to 'setmetatable' (table expected"));
        CHECK(contains(failure(*runtime, "setmetatable(setmetatable({}, {__metatable = 'locked'}), {})"),
                       "cannot change a protected metatable"));
    }

    TEST_CASE("print, warn and the log functions write to the log") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();
        const LogCapture log(LogLevel::Debug);

        CHECK(value(*runtime, "print('door', 1, 2.5, true, nil, {} ~= nil)\n"
                              "warn('careful', ' ', 'now')\n"
                              "warn('@off')\n"
                              "cordite.log.debug('checking', 7)\n"
                              "cordite.log.info('opened')\n"
                              "cordite.log.warn('creaks')\n"
                              "cordite.log.error('jammed', 'shut')\n"
                              "print()\n"
                              "return 'said'") == R"("said")");

        std::vector<std::string> lines;
        for (const LogMessage& message : log.messages()) {
            CHECK(message.category == "Script");
            lines.push_back(fmt::format("{}: {}", nameOf(message.level), message.text));
        }
        const std::vector<std::string> expected = {
            "info: door\t1\t2.5\ttrue\tnil\ttrue",
            "warn: careful now",
            "debug: checking\t7",
            "info: opened",
            "warn: creaks",
            "error: jammed\tshut",
            "info: ",
        };
        CHECK(lines == expected);
    }

    TEST_CASE("a function of the engine is called by the name it was given, and counted") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);

        CHECK(value(*runtime, "return test.add(2, 3)") == "5");
        CHECK(value(*runtime, "local sum = 0 for turn = 1, 4 do sum = test.add(sum, turn) end return sum") == "10");
        // What Lua's own checks of an argument say names the function and the place.
        CHECK(failure(*runtime, "return test.add(2, 'three')", "sum")
                  .starts_with("sum:1: bad argument #2 to 'add' (number expected, got string)"));

        uint64_t added = 0;
        uint64_t tripped = 1;
        for (const ScriptFunctionInfo& function : runtime->functions()) {
            if (function.name == "test.add") {
                added = function.calls;
                CHECK(function.signature == "add(a, b) -> integer");
                CHECK(function.description == "The sum of two whole numbers.");
            } else if (function.name == "test.trip") {
                tripped = function.calls;
            }
        }
        CHECK(added == 6);
        CHECK(tripped == 0);

        // A library is made where there is none, however deep; no name is the globals.
        const std::array more = {ScriptFunction{"sum", "sum(a, b) -> integer", "The same again.", add}};
        const Status deep = runtime->registerLibrary("test.deeper.still", more);
        REQUIRE_MESSAGE(deep.has_value(), test::errorOf(deep));
        const Status global = runtime->registerLibrary("", more);
        REQUIRE_MESSAGE(global.has_value(), test::errorOf(global));
        CHECK(value(*runtime, "return {test.deeper.still.sum(1, 1), sum(2, 2), test.add(3, 3)}") == "[2,4,6]");
        const std::vector<ScriptFunctionInfo> functions = runtime->functions();
        REQUIRE(functions.size() >= 2);
        CHECK(functions[functions.size() - 2].name == "test.deeper.still.sum");
        CHECK(functions.back().name == "sum");
    }

    TEST_CASE("every function the runtime provides says what it is for") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        const std::vector<ScriptFunctionInfo> functions = runtime->functions();
        std::vector<std::string> names;
        for (const ScriptFunctionInfo& function : functions) {
            CAPTURE(function.name);
            names.push_back(function.name);
            CHECK(function.signature.find('(') != std::string::npos);
            CHECK(function.description.size() > 20);
            CHECK(function.description.back() == '.');
            CHECK(function.calls == 0);
        }
        CHECK(names == std::vector<std::string>{"cordite.log.debug", "cordite.log.info", "cordite.log.warn",
                                                "cordite.log.error"});
    }

    TEST_CASE("what a function of the engine throws is an error of the script") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);

        const std::string message = failure(*runtime, "test.trip()", "trip");
        CHECK_MESSAGE(message.starts_with("the engine tripped\nstack traceback:"), message);
        CHECK_MESSAGE(contains(message, "trip:1: in main chunk"), message);
        CHECK(value(*runtime, "return {pcall(test.trip)}") == R"([false,"the engine tripped"])");
        CHECK(value(*runtime, "return test.add(1, 1)") == "2");
    }

    TEST_CASE("an error leaves a function of the engine the way an exception does") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);

        CHECK(failure(*runtime, "test.fail()", "hurry").starts_with("hurry:1: left in a hurry"));
        CHECK(seen.destroyed);
    }

    TEST_CASE("a function of the engine calls back into the scripts through the runtime") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);

        CHECK(value(*runtime, "local seen = {}\n"
                              "test.call(function() seen[#seen + 1] = 'inside' end)\n"
                              "seen[#seen + 1] = 'after'\n"
                              "return seen") == R"(["inside","after"])");
        CHECK(seen.found == runtime.get());
        // The call leaves the stack of the function as it found it.
        CHECK(seen.topBefore == 1);
        CHECK(seen.topAfter == 1);

        // From a coroutine as well: it is the coroutine's thread that calls.
        CHECK(value(*runtime, "local order = {}\n"
                              "local step = coroutine.wrap(function()\n"
                              "    test.call(function() order[#order + 1] = 'first' end)\n"
                              "    coroutine.yield()\n"
                              "    test.call(function() test.call(function() order[#order + 1] = 'second' end) end)\n"
                              "end)\n"
                              "step() order[#order + 1] = 'between' step()\n"
                              "return order") == R"(["first","between","second"])");

        // What goes wrong inside comes out as an error of the script that called.
        const std::string message = failure(*runtime, "test.call(function() error('deep down') end)", "nested");
        CHECK_MESSAGE(contains(message, "nested:1: deep down"), message);
        // A coroutine cannot be put to sleep across a function of the engine.
        CHECK(contains(failure(*runtime, "coroutine.wrap(function() test.call(coroutine.yield) end)()"),
                       "attempt to yield across a C-call boundary"));
        CHECK(value(*runtime, "return test.add(1, 1)") == "2");
    }

    TEST_CASE("code that is nested too deeply is refused, with words for whoever wrote it") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();
        const std::string tooDeep =
            "nest: the code is nested too deeply: brackets in brackets, blocks in blocks, or a long chain of '..'";

        const auto brackets = [](size_t depth) {
            return "return " + std::string(depth, '(') + "1" + std::string(depth, ')');
        };
        CHECK(value(*runtime, brackets(40)) == "1");
        CHECK(failure(*runtime, brackets(80), "nest") == tooDeep);
        CHECK(failure(*runtime, brackets(100000), "nest") == tooDeep);

        // A chain of '..' is taken from the right, one inside the other.
        const auto chain = [](size_t links) {
            std::string source = "return 'a'";
            for (size_t link = 0; link < links; ++link) {
                source += " .. 'a'";
            }
            return source;
        };
        CHECK(value(*runtime, "return #(" + chain(40).substr(7) + ")") == "41");
        CHECK(failure(*runtime, chain(80), "nest") == tooDeep);
        // Operators that are taken from the left go on as long as they like.
        std::string sum = "return 0";
        for (int term = 0; term < 1000; ++term) {
            sum += " + 1";
        }
        CHECK(value(*runtime, sum) == "1000");
    }

    TEST_CASE("calls that never end are stopped before the stack of the program is used up") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);

        // From script to script, Lua's own stack is what ends.
        CHECK(contains(failure(*runtime, "local function again() return 1 + again() end return again()"),
                       "stack overflow"));

        // Through a function of the engine or of a library, it is the stack of the program
        // that fills - and what Lua allows of that has to fit into the smallest stack a
        // main thread has, with room to spare: 1 MB on Windows, 8 MB elsewhere. Each of
        // these goes as deep as it is let, and says how much of the stack that took.
        const size_t allowed = (CORDITE_PLATFORM_WINDOWS != 0 ? size_t{512} : size_t{4096}) * 1024;
        const std::string_view head = "local top = test.stack() local deepest = top local levels = 0\n"
                                      "local function mark() levels = levels + 1 deepest = test.stack() end\n";
        const std::string_view tail = "\nreturn {levels, math.abs(top - deepest)}";

        struct Way {
            std::string_view name;
            std::string_view script;
            /// How deep it gets: one way takes two calls through C for every level.
            int64_t levels = 0;
        };

        for (const Way& way : {
                 Way{"pcall", "local function deeper() mark() pcall(deeper) end deeper()", 62},
                 Way{"gsub", "local function deeper() mark() return string.gsub('x', 'x', deeper) end pcall(deeper)",
                     61},
                 Way{"sort",
                     "local function deeper() mark() table.sort({2, 1}, function(a, b) deeper() return a < b end) end "
                     "pcall(deeper)",
                     61},
                 Way{"format",
                     "local thing thing = setmetatable({}, {__tostring = function() mark() return "
                     "string.format('%s', thing) end}) pcall(tostring, thing)",
                     60},
                 Way{"index",
                     "local thing thing = setmetatable({}, {__index = function(_, key) mark() return thing[key] end}) "
                     "pcall(function() return thing.door end)",
                     60},
                 Way{"coroutine", "local function deeper() mark() coroutine.wrap(deeper)() end pcall(deeper)", 61},
                 Way{"engine", "local function deeper() mark() test.call(deeper) end pcall(deeper)", 31},
             }) {
            const std::string name(way.name);
            CAPTURE(name);
            const Json measured =
                unwrap(runtime->run(std::string(head) + std::string(way.script) + std::string(tail), name));
            REQUIRE(measured.is_array());
            CHECK(measured[0].get<int64_t>() == way.levels);
            CHECK(measured[1].get<size_t>() > 0);
            CHECK(measured[1].get<size_t>() < allowed);
        }
        // What is left of such a fall is an error like any other.
        const std::string message =
            failure(*runtime, "local function deeper() return string.gsub('x', 'x', deeper) end deeper()", "fall");
        CHECK_MESSAGE(contains(message, "stack overflow"), message);
        CHECK(value(*runtime, "return 'fine'") == R"("fine")");
    }

    TEST_CASE("scripts run the same way twice") {
        // Two at once: were the seed of Lua's hashes taken from where a state lies in
        // memory, as Lua does by itself, they would have different ones.
        const std::unique_ptr<ScriptRuntime> first = makeRuntime();
        const std::unique_ptr<ScriptRuntime> second = makeRuntime();

        const std::string_view random =
            "return {math.random(1000), math.random(1000), math.random(1000), math.random(-5, 5), math.random()}";
        const std::string numbers = value(*first, random);
        CHECK(numbers == value(*second, random));
        // The numbers of the seed the runtime gives Lua; a game that was recorded plays back with them.
        CHECK(numbers == "[709,75,605,5,0.09409018116966306]");

        const std::string_view walk =
            "local visited = {}\n"
            "for _, name in ipairs({'north', 'south', 'east', 'west', 'up', 'down', 'alpha', 'beta', 'gamma',\n"
            "                       'delta', 'epsilon', 'zeta', 'eta', 'theta', 'iota', 'kappa'}) do\n"
            "    visited[name] = #name\n"
            "end\n"
            "local order = {}\n"
            "for name in pairs(visited) do order[#order + 1] = name end\n"
            "return order";
        const std::string order = value(*first, walk);
        CHECK(order == value(*second, walk));
    }

    TEST_CASE("what the engine does inside the fence is an error with a message too") {
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntime();

        const Status raised = runtime->protect([](lua_State* state) { luaL_error(state, "the %s is missing", "key"); });
        REQUIRE_FALSE(raised.has_value());
        CHECK(raised.error().code == ErrorCode::ScriptError);
        CHECK(raised.error().message == "the key is missing");

        const Status thrown =
            runtime->protect([](lua_State* /*state*/) { throw std::runtime_error("thrown by hand"); });
        REQUIRE_FALSE(thrown.has_value());
        CHECK(thrown.error().message == "thrown by hand");

        // The body has a stack of its own, and leaves nothing behind however it ends.
        int top = -1;
        for (int round = 0; round < 100; ++round) {
            const Status filled = runtime->protect([&](lua_State* state) {
                top = lua_gettop(state);
                for (int value = 0; value < 10; ++value) {
                    lua_pushinteger(state, value);
                }
                if (round % 2 == 1) {
                    luaL_error(state, "odd");
                }
            });
            CHECK(filled.has_value() == (round % 2 == 0));
            CHECK(top == 0);
        }
    }

    TEST_CASE("the runtime of a state is found through any of its coroutines") {
        Witness seen;
        const std::unique_ptr<ScriptRuntime> runtime = makeRuntimeWithFunctions(seen);
        CHECK(runtime->host() == &seen);

        CHECK(value(*runtime, "coroutine.wrap(function() test.call(function() end) end)() return true") == "true");
        CHECK(seen.found == runtime.get());

        ScriptRuntime* fromMain = nullptr;
        const Status status = runtime->protect([&](lua_State* state) { fromMain = &ScriptRuntime::of(state); });
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(fromMain == runtime.get());
    }
}
