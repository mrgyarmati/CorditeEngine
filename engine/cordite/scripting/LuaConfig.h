#pragma once

/// @file
/// Compile-time configuration of Lua.
///
/// The build passes this file to Lua as LUA_USER_H, so `lua.h` includes it wherever it is
/// used - in Lua's own sources as well as in the engine's - and all of them agree on the
/// settings. Do not include it yourself; include "cordite/scripting/Lua.h".
///
/// @code
/// #include "cordite/scripting/Lua.h"      // already configured
/// @endcode

#include "cordite/core/ExternalAssert.h"

// Lua can check how its API is used - an index that is not on the stack, a call for which
// too little was pushed - when it is told how to report what it finds. Report it like any
// other failed engine assertion; in a shipping build nothing is checked.
#define luai_apicheck(state, expression) (static_cast<void>(state), CORDITE_EXTERNAL_ASSERT(expression))

// The same seed for the hashes of strings in every run, so that pairs() walks a table
// whose keys are names and numbers in the same order every time. Lua picks the seed at
// random to make collisions hard to provoke for someone who sends keys to a server; the
// scripts of a game are the game's own, and a game that runs the same way twice can be
// tested.
#define luai_makeseed(state) (static_cast<void>(state), 0x436F7264U)

// Lua reads and writes numbers in hexadecimal - 0x1.8p3, string.format('%a') - with the C
// library where that is a C99 one, and with code of its own where it is not, which is
// how Lua is built for Windows. Its own code everywhere: a script gets the same answer
// on every machine.
#undef lua_strx2number
#undef lua_number2strx

// How deep calls may be nested that go through C: a function of a library that calls back
// into a script, a metamethod, pcall() - and how deep the code of a chunk may be nested,
// which the compiler follows the same way. Every such level takes room on the stack of
// the program, and running out of that is not an error but the end.
//
// Lua allows 200, which an optimized build takes in its stride. Without optimization
// MSVC spends up to 6.6 kB on a level (string.gsub() with a function to call), Lua lets
// a tenth more through while it reports the overflow, and the main thread of a Windows
// program has 1 MB: 220 levels are 1.4 MB, and were a crash. 64 are less than half of
// that megabyte, which a test holds every configuration to.
#define LUAI_MAXCCALLS 64
