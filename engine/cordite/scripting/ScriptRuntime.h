#pragma once

/// @file
/// The Lua state that gameplay scripts run in, and the fence around it.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<ScriptRuntime> runtime, ScriptRuntime::create());
///
/// // A piece of Lua, run for what it gives back.
/// CORDITE_TRY_ASSIGN(const Json six, runtime->run("return 2 * 3", "sum"));
///
/// // Anything else is done with Lua's own API (cordite/scripting/Lua.h), inside the fence:
/// CORDITE_TRY(runtime->protect([&](lua_State* state) {
///     // Leaves what the script returns on the stack: here, a table of functions.
///     ScriptRuntime::runChunk(state, source, "@scripts/door.lua", 1);
///     lua_getfield(state, -1, "update");
///     lua_pushnumber(state, deltaSeconds);
///     lua_call(state, 1, 0);
/// }));
///
/// // What scripts may call is given a name, and said in words to whoever writes them:
/// const std::array functions = {
///     ScriptFunction{"seconds", "seconds() -> number", "The time since the game began.", pushSeconds},
/// };
/// CORDITE_TRY(runtime->registerLibrary("cordite.time", functions));
/// @endcode
///
/// A script is the work of whoever makes the game, and it is wrong as often as any other
/// work. The runtime sees to it that a script that is wrong is an error with a message -
/// a Status with ErrorCode::ScriptError, never an end of the program:
///
/// - **A mistake has a place.** What a script cannot do is said with the name of its
///   chunk and the line, and with the calls that led there.
/// - **A script that does not end is stopped.** A call into the scripts may execute
///   `instructionLimit` instructions; then it ends with an error. A script cannot catch
///   that error and carry on.
/// - **A script that takes all memory is stopped.** The scripts hold at most
///   `memoryLimitBytes` together; what asks for more ends with an error, which a script
///   may catch like any other.
/// - **Calls are nested to a depth.** Calls from script to script go as deep as Lua's
///   own stack, a million values. Calls that go through a function of the engine or of
///   a library - pcall(), a metamethod, string.gsub() with a function - use the stack of
///   the program, and are an error past 64 inside each other (LuaConfig.h). So is code
///   that is nested deeper than that: brackets, blocks, and chains of `..`.
/// - **Scripts do not reach the machine.** Of Lua's libraries there are `coroutine`,
///   `table`, `string`, `math` and `utf8`, and the basic functions without those that load
///   code or steer the collector. Files, the operating system, loading of libraries and
///   the debug library are not part of the build, and only text is compiled - a chunk
///   that is already compiled can be made to do anything.
/// - **The variables of a script are its own.** Every chunk runs in an environment of its
///   own: the globals it makes are its alone, and two scripts that use the same name do
///   not meet. What the engine provides is found through it, and a script that gives one
///   of those names another value has done so for itself only.
/// - **A name that was never given a value is a mistake.** Reading one is an error that
///   names it, instead of a nil that fails somewhere else. So is giving a value to one
///   once the chunk has run: its globals are made while it loads, and what is assigned
///   later without `local` is nearly always a `local` that was forgotten.
/// - **Scripts run the same way twice.** math.random() begins with the same seed in every
///   run - math.randomseed() without an argument asks for another - and pairs() walks
///   tables whose keys are names and numbers in the same order.
///
/// What the fence does not hold, and what it costs: counting instructions makes a script
/// that only computes take about four tenths longer (`instructionLimit` 0 turns it off);
/// one call of a library function - a pattern matched against a long string - is one
/// instruction however long it takes; and scripts share the libraries themselves: one
/// that replaces `string.format` has replaced it for all.
///
/// A runtime is not thread-safe: everything that touches it does so on one thread.

#include "cordite/core/Base.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct lua_State;

namespace cordite {

/// A function that Lua calls: its arguments are on the stack of `state`, and it returns
/// how many values it has pushed as results.
using ScriptNative = int (*)(lua_State* state);

/// A function of the engine that scripts may call.
struct ScriptFunction {
    /// What a script calls it inside its library: "find".
    const char* name = "";
    /// How it is called and what it gives back, for whoever writes scripts:
    /// "find(name) -> Entity or nil".
    const char* signature = "";
    /// What it does, in a sentence or two.
    const char* description = "";
    /// The function; not null. It may raise Lua errors and throw exceptions that derive
    /// from std::exception; a script sees both as errors.
    ScriptNative function = nullptr;
};

/// What the runtime knows of a function it was given.
struct ScriptFunctionInfo {
    /// Its name as a script writes it, library and all: "cordite.world.find".
    std::string name;
    std::string signature;
    std::string description;
    /// How often scripts have called it.
    uint64_t calls = 0;
};

struct ScriptRuntimeDesc {
    /// The most memory the scripts may hold at a time, in bytes.
    size_t memoryLimitBytes = size_t{256} * 1024 * 1024;
    /// The most instructions one call into the scripts may execute; they are counted a
    /// thousand at a time. Zero does not count them: scripts then run faster, and one
    /// that does not end takes the program with it.
    uint64_t instructionLimit = 10'000'000;
};

class ScriptRuntime {
public:
    /// Fails with InvalidArgument when `memoryLimitBytes` does not hold Lua itself.
    [[nodiscard]] static Result<std::unique_ptr<ScriptRuntime>> create(const ScriptRuntimeDesc& desc = {});
    ~ScriptRuntime();
    CORDITE_NON_MOVABLE(ScriptRuntime);

    /// The runtime that `state` - or the coroutine it is - belongs to.
    [[nodiscard]] static ScriptRuntime& of(lua_State* state);

    /// Calls `body(lua_State*)` inside the fence: what it does with the state may raise
    /// Lua errors, and whatever it calls in the scripts counts against the limits. The
    /// body has a stack of its own, empty at first, and whatever it leaves there is
    /// gone afterwards. Fails with ScriptError - the message, and the calls that led to
    /// it - when an error was raised and nothing caught it, and whenever the call used
    /// up its instructions, whatever a script made of that.
    ///
    /// The instructions of the outermost protect() are counted from zero; one that is
    /// called from inside another - by a function of the engine that a script called -
    /// goes on counting where that one is.
    ///
    /// An error leaves `body` the way an exception does: what it holds is destroyed, but
    /// what it had only half done stays half done. Take what is needed out of the engine
    /// first, then call the scripts.
    template <typename Body>
    [[nodiscard]] Status protect(Body&& body) {
        using Stored = std::remove_reference_t<Body>;
        // The body goes through Lua as a void*, and comes back as what it is.
        return protectErased([](lua_State* state, void* context) { (*static_cast<Stored*>(context))(state); },
                             const_cast<void*>(static_cast<const void*>(&body)));
    }

    /// Compiles `source`, runs it in an environment of its own and gives back the first
    /// value it returns, as JSON (ScriptValue.h); null when it returns nothing. `name` is
    /// what messages call the chunk. Fails with ScriptError for text that is not Lua and
    /// for an error while it runs, and with InvalidArgument for a result that JSON cannot
    /// hold.
    [[nodiscard]] Result<Json> run(std::string_view source, std::string_view name);

    /// For use inside protect(): compiles `source`, runs it in a new environment and
    /// leaves `results` values that it returned on the stack. Afterwards the chunk can
    /// make no more globals. `chunkName` is Lua's name for the chunk: "@" followed by a
    /// path for a file, "=" followed by anything else. Raises a Lua error for text that
    /// is not Lua, and for an error while the chunk runs.
    static void runChunk(lua_State* state, std::string_view source, std::string_view chunkName, int results);

    /// Gives scripts the functions of a library: `library` is the table they are in,
    /// such as "cordite.time", made if it is not there yet; no name puts them among the
    /// globals. A name that is taken is replaced, and nothing of `functions` is held on
    /// to. Fails with ScriptError when the scripts are out of memory.
    [[nodiscard]] Status registerLibrary(std::string_view library, std::span<const ScriptFunction> functions);

    /// Every function that was registered, in the order it was, with how often it has
    /// been called since.
    [[nodiscard]] std::vector<ScriptFunctionInfo> functions() const;

    /// The memory the scripts hold now, in bytes.
    [[nodiscard]] size_t memoryBytes() const;

    /// Has Lua give back, now, everything that nothing holds any more. Lua does that by
    /// itself as it goes, a little at a time; this is for the moment after much was let
    /// go of at once - a scene that ended - and for tests that measure.
    void collectGarbage();

    /// The instructions the scripts have executed since the runtime was made, to the
    /// nearest thousand below. Zero when they are not counted.
    [[nodiscard]] uint64_t instructions() const;

    /// Something of whoever owns the runtime, for the functions it registers to find
    /// through of(). The runtime does nothing with it.
    void setHost(void* host);
    [[nodiscard]] void* host() const;

private:
    struct Impl;

    explicit ScriptRuntime(std::unique_ptr<Impl> impl);

    [[nodiscard]] Status protectErased(void (*body)(lua_State* state, void* context), void* context);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
