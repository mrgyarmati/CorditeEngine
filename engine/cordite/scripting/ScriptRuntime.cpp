#include "cordite/scripting/ScriptRuntime.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptValue.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <utility>

namespace cordite {
namespace {

/// How many instructions pass between two looks at the limit. Lua counts them one by
/// one either way; this is how often it calls back.
constexpr int kInstructionsPerCount = 1000;

/// The seed math.random() begins with.
constexpr lua_Integer kRandomSeed = 20071107;

/// A function that was registered, and what is said of it.
struct NativeEntry {
    ScriptFunctionInfo info;
    ScriptNative function = nullptr;
};

/// Everything the functions that Lua calls back need to know of their runtime. It is
/// found through the extra space of a state, which every coroutine of it shares.
struct RuntimeData {
    ScriptRuntime* owner = nullptr;
    ScriptRuntimeDesc desc;
    /// The main thread of the state.
    lua_State* state = nullptr;
    /// The thread whose script has called a function of the engine that has not
    /// returned yet. A call into the scripts made from there has to use that thread: the
    /// main one is in the middle of something.
    lua_State* running = nullptr;
    size_t memoryBytes = 0;
    /// Calls of protect() that are under way, one inside the other.
    uint32_t depth = 0;
    /// What the outermost of them may still execute.
    int64_t instructionsLeft = 0;
    uint64_t instructions = 0;
    /// The limit was reached, and what was said of it then: where the script was.
    bool exhausted = false;
    std::string exhaustedMessage;
    /// The pieces of a warning that Lua is still handing over.
    std::string warning;
    /// A deque, because Lua holds on to the address of every entry.
    std::deque<NativeEntry> functions;
    void* host = nullptr;
};

RuntimeData& dataOf(lua_State* state) {
    return **static_cast<RuntimeData**>(lua_getextraspace(state));
}

/// Lua's memory: malloc(), counted, with a limit. Lua takes for granted that making a
/// block smaller works.
void* allocate(void* userData, void* block, size_t oldSize, size_t newSize) {
    RuntimeData& data = *static_cast<RuntimeData*>(userData);
    // For a new block, Lua says in `oldSize` what kind of object it is for.
    const size_t held = block != nullptr ? oldSize : 0;
    if (newSize == 0) {
        std::free(block);
        data.memoryBytes -= held;
        return nullptr;
    }
    const size_t limit = data.desc.memoryLimitBytes;
    if (newSize > held && newSize - held > limit - std::min(limit, data.memoryBytes)) {
        return nullptr;
    }
    void* moved = std::realloc(block, newSize);
    if (moved == nullptr) {
        return newSize <= held ? block : nullptr;
    }
    data.memoryBytes = data.memoryBytes - held + newSize;
    return moved;
}

/// Lua calls this for an error that nothing protects against, and ends the program when
/// it returns. It cannot happen while Lua's API is only used inside protect().
int panic(lua_State* state) {
    const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "(no message)";
    CORDITE_LOG_CRITICAL("Script", "Lua was used outside ScriptRuntime::protect(), and failed: {}", message);
    return 0;
}

/// Puts the pieces of a warning together and logs it. A piece that begins with '@' and
/// stands alone is a command to Lua's own printer of warnings, not a warning.
void warn(void* userData, const char* message, int toBeContinued) {
    RuntimeData& data = *static_cast<RuntimeData*>(userData);
    const bool isCommand = data.warning.empty() && toBeContinued == 0 && message[0] == '@';
    if (isCommand) {
        return;
    }
    data.warning += message;
    if (toBeContinued == 0) {
        CORDITE_LOG_WARN("Script", "{}", data.warning);
        data.warning.clear();
    }
}

/// Raises the error on top of the stack. Does not return; the int is what lets a
/// function end with `return raise(state);`.
int raise(lua_State* state) {
    return lua_error(state);
}

/// Runs `function`, which does the work of something Lua has called, and turns what it
/// throws into a Lua error. Lua takes anything thrown through it for an error of its
/// own, without knowing what it says.
template <typename Function>
int guarded(lua_State* state, Function&& function) {
    std::string thrown;
    try {
        return std::forward<Function>(function)();
    } catch (const std::exception& exception) {
        thrown = exception.what();
    }
    lua_pushlstring(state, thrown.data(), thrown.size());
    return raise(state);
}

/// Whether a script is among the calls that are under way. When only functions of the
/// engine are - text that would not compile, say - listing them says nothing.
bool isScriptRunning(lua_State* state) {
    lua_Debug record;
    for (int level = 0; lua_getstack(state, level, &record) != 0; ++level) {
        lua_getinfo(state, "S", &record);
        if (*record.what != 'C') {
            return true;
        }
    }
    return false;
}

/// The message handler of protect(): makes a message of whatever was raised, and adds
/// the calls that led to it.
int describeError(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (message == nullptr) {
        if (luaL_callmeta(state, 1, "__tostring") != 0 && lua_type(state, -1) == LUA_TSTRING) {
            message = lua_tostring(state, -1);
        } else {
            message = lua_pushfstring(state, "(the error is a %s value, not a message)", luaL_typename(state, 1));
        }
    }
    if (isScriptRunning(state)) {
        luaL_traceback(state, state, message, 1);
    } else {
        lua_pushstring(state, message);
    }
    return 1;
}

/// What protect() hands to Lua: the body to run, as a pointer to this.
struct ProtectedBody {
    void (*body)(lua_State* state, void* context) = nullptr;
    void* context = nullptr;
};

int runProtectedBody(lua_State* state) {
    const auto* call = static_cast<const ProtectedBody*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    return guarded(state, [&] {
        call->body(state, call->context);
        return 0;
    });
}

/// Called by Lua every kInstructionsPerCount instructions. Ends the script with an
/// error once the call it is part of has used up what it may execute.
void countInstructions(lua_State* state, lua_Debug* /*record*/) {
    RuntimeData& data = dataOf(state);
    data.instructions += kInstructionsPerCount;
    data.instructionsLeft -= kInstructionsPerCount;
    if (data.instructionsLeft > 0) {
        return;
    }
    if (!data.exhausted) {
        data.exhausted = true;
        const std::string what = fmt::format(
            "ran for more than {} instructions without coming to an end, and was stopped", data.desc.instructionLimit);
        // Said once, with where the script was when its time ran out. What is raised
        // from here on may be caught and changed on its way out; protect() reports this.
        // Should there be no memory left to say where, it is said without.
        data.exhaustedMessage = "a script " + what;
        luaL_where(state, 0);
        const std::string message = lua_tostring(state, -1) + what;
        luaL_traceback(state, state, message.c_str(), 0);
        data.exhaustedMessage = lua_tostring(state, -1);
        lua_pop(state, 2);
    }
    lua_pushlstring(state, data.exhaustedMessage.data(), data.exhaustedMessage.size());
    raise(state);
}

/// pcall() and xpcall(), as Lua's own are - but for the error that says the script has
/// run too long, which they pass on instead of handing it to the script: caught, it would
/// let a loop of calls that each run out of time go on for ever.
int finishProtectedCall(lua_State* state, int status, lua_KContext extra) {
    if (status != LUA_OK && status != LUA_YIELD) {
        if (dataOf(state).exhausted) {
            return raise(state);
        }
        lua_pushboolean(state, 0);
        lua_pushvalue(state, -2);
        return 2;
    }
    return lua_gettop(state) - static_cast<int>(extra);
}

int protectedCall(lua_State* state) {
    luaL_checkany(state, 1);
    lua_pushboolean(state, 1);
    lua_insert(state, 1);
    const int status = lua_pcallk(state, lua_gettop(state) - 2, LUA_MULTRET, 0, 0, finishProtectedCall);
    return finishProtectedCall(state, status, 0);
}

/// The handler that xpcall() installs: the script's own (the first upvalue) - which is
/// not asked about the error that says the script has run too long. That error is
/// raised while Lua is counting, and while it is on its way Lua counts nothing: a
/// handler that does not end would never be stopped.
int handleUnlessStopped(lua_State* state) {
    if (dataOf(state).exhausted) {
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, 1, 1);
    return 1;
}

/// After the rotation the stack holds the function, the handler, true, the function
/// again and its arguments; the first two are not results.
int protectedCallWithHandler(lua_State* state) {
    const int arguments = lua_gettop(state);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_pushvalue(state, 2);
    lua_pushcclosure(state, handleUnlessStopped, 1);
    lua_replace(state, 2);
    lua_pushboolean(state, 1);
    lua_pushvalue(state, 1);
    lua_rotate(state, 3, 2);
    const int status = lua_pcallk(state, arguments - 2, LUA_MULTRET, 2, 2, finishProtectedCall);
    return finishProtectedCall(state, status, 2);
}

/// coroutine.resume(), which hands an error in the coroutine to whoever resumed it:
/// Lua's own (the first upvalue), passing on the one error that is not to be caught.
/// The argument is checked here, where Lua can say which function a script got wrong.
int resumeCoroutine(lua_State* state) {
    luaL_argexpected(state, lua_isthread(state, 1), 1, "coroutine");
    const int arguments = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, arguments, LUA_MULTRET);
    if (dataOf(state).exhausted && lua_gettop(state) >= 2 && lua_toboolean(state, 1) == 0) {
        lua_settop(state, 2);
        return raise(state);
    }
    return lua_gettop(state);
}

/// setmetatable(), as Lua's own is - without finalizers. Lua runs a finalizer whenever
/// it collects, and counts no instructions while it does: one that does not end would
/// stop the program for good.
int setMetatable(lua_State* state) {
    const int kind = lua_type(state, 2);
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_argexpected(state, kind == LUA_TNIL || kind == LUA_TTABLE, 2, "nil or table");
    if (luaL_getmetafield(state, 1, "__metatable") != LUA_TNIL) {
        return luaL_error(state, "cannot change a protected metatable");
    }
    if (kind == LUA_TTABLE) {
        lua_pushliteral(state, "__gc");
        lua_rawget(state, 2);
        if (!lua_isnil(state, -1)) {
            return luaL_error(state, "a script cannot give anything a finalizer ('__gc')");
        }
    }
    lua_settop(state, 2);
    lua_setmetatable(state, 1);
    return 1;
}

/// The arguments of a call, as print() writes them: side by side, with tabs between.
std::string joinArguments(lua_State* state) {
    std::string text;
    const int arguments = lua_gettop(state);
    for (int index = 1; index <= arguments; ++index) {
        size_t length = 0;
        const char* piece = luaL_tolstring(state, index, &length);
        if (index > 1) {
            text += '\t';
        }
        text.append(piece, length);
        lua_pop(state, 1);
    }
    return text;
}

int logDebug(lua_State* state) {
    CORDITE_LOG_DEBUG("Script", "{}", joinArguments(state));
    return 0;
}

int logInfo(lua_State* state) {
    CORDITE_LOG_INFO("Script", "{}", joinArguments(state));
    return 0;
}

int logWarning(lua_State* state) {
    CORDITE_LOG_WARN("Script", "{}", joinArguments(state));
    return 0;
}

int logError(lua_State* state) {
    CORDITE_LOG_ERROR("Script", "{}", joinArguments(state));
    return 0;
}

/// print(), which is not one of the engine's own functions and so is called by Lua
/// directly.
int print(lua_State* state) {
    return guarded(state, [&] { return logInfo(state); });
}

constexpr std::array kLogFunctions = {
    ScriptFunction{"debug", "debug(...)",
                   "Writes its arguments to the log, side by side, as a message for whoever is looking for a "
                   "mistake: it is left out unless the log is asked for such messages.",
                   logDebug},
    ScriptFunction{"info", "info(...)", "Writes its arguments to the log, side by side. print() does the same.",
                   logInfo},
    ScriptFunction{"warn", "warn(...)",
                   "Writes its arguments to the log, side by side, as a warning: something that should not be so.",
                   logWarning},
    ScriptFunction{"error", "error(...)",
                   "Writes its arguments to the log, side by side, as an error. The script goes on; Lua's own "
                   "error() is what stops it.",
                   logError},
};

/// Raises what Lua says of text it did not compile, which is on top of the stack.
/// Where Lua leaves out which chunk it is talking about, that is added; and what it
/// says of code that is nested too deeply is said in words that mean something to
/// whoever wrote the code.
int raiseLoadError(lua_State* state, int status, std::string_view chunkName) {
    // The name as Lua shows it: without the character that says what kind of name it is.
    if (chunkName.starts_with('@') || chunkName.starts_with('=')) {
        chunkName.remove_prefix(1);
    }
    const std::string shown(chunkName);
    const std::string_view message = lua_tostring(state, -1);
    if (status == LUA_ERRMEM) {
        lua_pushfstring(state, "%s: out of memory while it was compiled", shown.c_str());
    } else if (message == "C stack overflow") {
        lua_pushfstring(state,
                        "%s: the code is nested too deeply: brackets in brackets, blocks in blocks, or a long "
                        "chain of '..'",
                        shown.c_str());
    } else if (message.starts_with("attempt to load")) {
        lua_pushfstring(state, "%s: %s", shown.c_str(), lua_tostring(state, -1));
    }
    return raise(state);
}

/// The name a key has in a message about a variable.
const char* nameOfVariable(lua_State* state, int index) {
    return lua_type(state, index) == LUA_TSTRING ? lua_tostring(state, index) : luaL_typename(state, index);
}

/// What an environment answers when it is asked for a name it does not have: what the
/// engine provides under that name (the globals of the state, the first upvalue) - or
/// that there is no such thing.
int readVariable(lua_State* state) {
    lua_pushvalue(state, 2);
    lua_rawget(state, lua_upvalueindex(1));
    if (!lua_isnil(state, -1)) {
        return 1;
    }
    return luaL_error(state, "'%s' is not defined: nothing was ever given that name", nameOfVariable(state, 2));
}

/// What an environment answers when a chunk that has run is given a new global.
int refuseNewVariable(lua_State* state) {
    return luaL_error(state, "'%s' is given a value but was never declared: is a 'local' missing?",
                      nameOfVariable(state, 2));
}

/// Takes `name` out of the table on top of the stack.
void removeMember(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setfield(state, -2, name);
}

/// Replaces the function `name` of the table on top of the stack by `replacement`,
/// which gets the function that was there as its first upvalue.
void wrapFunction(lua_State* state, const char* name, lua_CFunction replacement) {
    lua_getfield(state, -1, name);
    lua_pushcclosure(state, replacement, 1);
    lua_setfield(state, -2, name);
}

/// Gives the state the libraries that scripts may have, and takes out of them what
/// they may not.
void openLibraries(lua_State* state) {
    const std::array<luaL_Reg, 6> libraries = {{
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    }};
    for (const luaL_Reg& library : libraries) {
        luaL_requiref(state, library.name, library.func, 1);
        lua_pop(state, 1);
    }

    lua_pushglobaltable(state);
    // What loads code: from files, which scripts have no business with, and from
    // strings, which may hold code that is compiled already.
    removeMember(state, "dofile");
    removeMember(state, "loadfile");
    removeMember(state, "load");
    // Whoever steers the collector can stop it.
    removeMember(state, "collectgarbage");
    // Every chunk has a _G of its own: its environment.
    removeMember(state, LUA_GNAME);
    lua_pushcfunction(state, protectedCall);
    lua_setfield(state, -2, "pcall");
    lua_pushcfunction(state, protectedCallWithHandler);
    lua_setfield(state, -2, "xpcall");
    lua_pushcfunction(state, print);
    lua_setfield(state, -2, "print");
    lua_pushcfunction(state, setMetatable);
    lua_setfield(state, -2, "setmetatable");

    // A chunk that is compiled already is of no use without load(), and of none with it.
    lua_getfield(state, -1, LUA_STRLIBNAME);
    removeMember(state, "dump");
    lua_pop(state, 1);

    lua_getfield(state, -1, LUA_COLIBNAME);
    wrapFunction(state, "resume", resumeCoroutine);
    lua_pop(state, 1);

    lua_getfield(state, -1, LUA_MATHLIBNAME);
    lua_getfield(state, -1, "randomseed");
    lua_pushinteger(state, kRandomSeed);
    lua_call(state, 1, 0);
    lua_pop(state, 1);

    // What the engine itself provides is in one table.
    lua_newtable(state);
    pushScriptNull(state);
    lua_setfield(state, -2, "null");
    lua_setfield(state, -2, "cordite");
    lua_pop(state, 1);
}

/// Leaves the table that `library` names - "cordite.time" - on the stack: found among
/// the globals of the state, and made where it is not there. The globals themselves for
/// no name.
void pushLibrary(lua_State* state, std::string_view library) {
    lua_pushglobaltable(state);
    size_t begin = 0;
    while (begin < library.size()) {
        const size_t end = std::min(library.find('.', begin), library.size());
        const std::string_view name = library.substr(begin, end - begin);
        lua_pushlstring(state, name.data(), name.size());
        lua_rawget(state, -2);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushlstring(state, name.data(), name.size());
            lua_pushvalue(state, -2);
            lua_rawset(state, -4);
        }
        lua_remove(state, -2);
        begin = end + 1;
    }
}

/// Says which thread a function of the engine is running on, for as long as it is -
/// however it is left.
class RunningThread {
public:
    RunningThread(RuntimeData& data, lua_State* state) : m_data(data), m_previous(std::exchange(data.running, state)) {}

    ~RunningThread() { m_data.running = m_previous; }

    CORDITE_NON_MOVABLE(RunningThread);

private:
    RuntimeData& m_data;
    lua_State* m_previous;
};

/// What Lua calls for every function that was registered: counts the call, and runs the
/// function with what it throws turned into an error of the script.
int callNative(lua_State* state) {
    auto* entry = static_cast<NativeEntry*>(lua_touserdata(state, lua_upvalueindex(1)));
    ++entry->info.calls;
    const RunningThread running(dataOf(state), state);
    return guarded(state, [&] { return entry->function(state); });
}

} // namespace

struct ScriptRuntime::Impl : RuntimeData {};

Result<std::unique_ptr<ScriptRuntime>> ScriptRuntime::create(const ScriptRuntimeDesc& desc) {
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;
    RuntimeData* const data = impl.get();
    lua_State* state = lua_newstate(allocate, data);
    if (state == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the {} bytes that the scripts may hold do not hold Lua itself",
                         desc.memoryLimitBytes);
    }
    impl->state = state;
    *static_cast<RuntimeData**>(lua_getextraspace(state)) = data;
    lua_atpanic(state, panic);
    lua_setwarnf(state, warn, data);

    std::unique_ptr<ScriptRuntime> runtime(new ScriptRuntime(std::move(impl)));
    data->owner = runtime.get();
    Status prepared = runtime->protect([](lua_State* lua) { openLibraries(lua); });
    if (prepared) {
        prepared = runtime->registerLibrary("cordite.log", kLogFunctions);
    }
    if (!prepared) {
        return makeError(ErrorCode::InvalidArgument,
                         "the {} bytes that the scripts may hold do not hold Lua itself: {}", desc.memoryLimitBytes,
                         prepared.error().message);
    }
    // Counted from here on, in the main thread and in every coroutine made from it.
    if (desc.instructionLimit > 0) {
        lua_sethook(state, countInstructions, LUA_MASKCOUNT, kInstructionsPerCount);
    }
    return runtime;
}

ScriptRuntime::ScriptRuntime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

ScriptRuntime::~ScriptRuntime() {
    lua_close(m_impl->state);
    CORDITE_ASSERT(m_impl->memoryBytes == 0, "Lua gave back {} bytes less than it took", m_impl->memoryBytes);
}

ScriptRuntime& ScriptRuntime::of(lua_State* state) {
    return *dataOf(state).owner;
}

Status ScriptRuntime::protectErased(void (*body)(lua_State* state, void* context), void* context) {
    RuntimeData& data = *m_impl;
    lua_State* state = data.running != nullptr ? data.running : data.state;
    if (data.depth == 0) {
        data.instructionsLeft = static_cast<int64_t>(
            std::min<uint64_t>(data.desc.instructionLimit, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        data.exhausted = false;
        data.exhaustedMessage.clear();
    }
    // The handler, the function and its argument. Asking for room raises nothing.
    if (lua_checkstack(state, 3) == 0) {
        return makeError(ErrorCode::ScriptError, "the scripts are out of stack");
    }
    const int base = lua_gettop(state);
    ProtectedBody call{.body = body, .context = context};
    lua_pushcfunction(state, describeError);
    lua_pushcfunction(state, runProtectedBody);
    lua_pushlightuserdata(state, &call);
    ++data.depth;
    const int status = lua_pcall(state, 1, 0, base + 1);
    --data.depth;

    Status result;
    // A call that used up its instructions has failed, whatever the script made of the
    // error on its way out: there are ways to swallow it - coroutine.close() is one -
    // and none of them makes the call one that came to an end by itself.
    if (status != LUA_OK || data.exhausted) {
        // What is on top of the stack is the message, unless there is none: when memory
        // ran out, for which Lua makes none, or when something was thrown that Lua knows
        // nothing of.
        std::string message = "(an error without a message)";
        if (data.exhausted) {
            message = data.exhaustedMessage;
        } else if (status == LUA_ERRMEM) {
            message = fmt::format("out of memory: the scripts hold {} of the {} bytes they may", data.memoryBytes,
                                  data.desc.memoryLimitBytes);
        } else if (lua_type(state, -1) == LUA_TSTRING) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            message.assign(text, length);
        }
        result = makeError(ErrorCode::ScriptError, "{}", message);
    }
    lua_settop(state, base);
    return result;
}

Result<Json> ScriptRuntime::run(std::string_view source, std::string_view name) {
    const std::string chunkName = fmt::format("={}", name);
    Result<Json> value = Json(nullptr);
    CORDITE_TRY(protect([&](lua_State* state) {
        runChunk(state, source, chunkName, 1);
        value = readScriptValue(state, -1);
    }));
    if (!value) {
        return tl::unexpected(value.error().withContext(fmt::format("what {} returns", name)));
    }
    return value;
}

void ScriptRuntime::runChunk(lua_State* state, std::string_view source, std::string_view chunkName, int results) {
    luaL_checkstack(state, 6, "the scripts are out of stack");
    const std::string name(chunkName);
    // Text only: Lua trusts a chunk that says it is compiled already.
    const int loaded = luaL_loadbufferx(state, source.data(), source.size(), name.c_str(), "t");
    if (loaded != LUA_OK) {
        raiseLoadError(state, loaded, name);
    }
    const int chunk = lua_gettop(state);

    // The environment: a table of the chunk's own, which asks the globals of the state
    // for what it does not have itself. Its metatable is kept below the chunk, to be
    // closed to new names once the chunk has run.
    lua_newtable(state);
    lua_newtable(state);
    lua_pushglobaltable(state);
    lua_pushcclosure(state, readVariable, 1);
    lua_setfield(state, -2, "__index");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pushvalue(state, -1);
    lua_insert(state, chunk);
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    lua_setfield(state, -2, LUA_GNAME);
    // The first upvalue of a chunk is its environment.
    if (lua_setupvalue(state, -2, 1) == nullptr) {
        lua_pop(state, 1);
    }

    lua_call(state, 0, results);

    const int metatable = chunk;
    lua_pushcfunction(state, refuseNewVariable);
    lua_setfield(state, metatable, "__newindex");
    lua_remove(state, metatable);
}

Status ScriptRuntime::registerLibrary(std::string_view library, std::span<const ScriptFunction> functions) {
    RuntimeData& data = *m_impl;
    return protect([&](lua_State* state) {
        pushLibrary(state, library);
        if (!library.empty()) {
            // Lua looks here for what to call a function that it was not called by
            // name - handed to pcall(), say - when it has something to say of it:
            // "bad argument #1 to 'cordite.time.seconds'" instead of "to '?'".
            const std::string name(library);
            luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, name.c_str());
            lua_pop(state, 1);
        }
        for (const ScriptFunction& function : functions) {
            CORDITE_ASSERT(function.function != nullptr, "the script function '{}' of '{}' has no function",
                           function.name, library);
            if (function.function == nullptr) {
                continue;
            }
            NativeEntry& entry = data.functions.emplace_back();
            entry.info.name =
                library.empty() ? std::string(function.name) : fmt::format("{}.{}", library, function.name);
            entry.info.signature = function.signature;
            entry.info.description = function.description;
            entry.function = function.function;
            lua_pushlightuserdata(state, &entry);
            lua_pushcclosure(state, callNative, 1);
            lua_setfield(state, -2, function.name);
        }
        lua_pop(state, 1);
    });
}

std::vector<ScriptFunctionInfo> ScriptRuntime::functions() const {
    std::vector<ScriptFunctionInfo> all;
    all.reserve(m_impl->functions.size());
    for (const NativeEntry& entry : m_impl->functions) {
        all.push_back(entry.info);
    }
    return all;
}

size_t ScriptRuntime::memoryBytes() const {
    return m_impl->memoryBytes;
}

void ScriptRuntime::collectGarbage() {
    // Collecting runs no script - scripts have no finalizers - but it is Lua's API, and
    // that is used inside the fence.
    if (const Status collected = protect([](lua_State* state) { lua_gc(state, LUA_GCCOLLECT, 0); }); !collected) {
        CORDITE_LOG_ERROR("Script", "Lua could not collect what is no longer held: {}", collected.error().message);
    }
}

uint64_t ScriptRuntime::instructions() const {
    return m_impl->instructions;
}

void ScriptRuntime::setHost(void* host) {
    m_impl->host = host;
}

void* ScriptRuntime::host() const {
    return m_impl->host;
}

} // namespace cordite
