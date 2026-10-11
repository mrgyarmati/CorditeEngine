#pragma once

/// @file
/// Thin wrappers over operating system services that the standard library does not cover.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {

/// Returns the absolute path of the running executable, or an empty path if the
/// operating system cannot report it.
std::filesystem::path executablePath();

/// Returns the directory containing the running executable.
std::filesystem::path executableDirectory();

/// Returns true when a debugger is attached. Always false on platforms without a cheap
/// way to tell (Linux, macOS).
bool isDebuggerPresent();

/// Makes fatal errors end the process instead of opening a dialog that waits for a
/// click: failed C runtime assertions, abort(), crashes and missing libraries are
/// reported on standard error and through the exit status. Call it first thing in any
/// program that runs unattended - tests, batch tools, the editor in headless mode -
/// where a dialog would hang the run until it times out. Does nothing on platforms that
/// have no such dialogs.
///
/// @code
/// int main(int argc, char** argv) {
///     if (runsUnattended(argc, argv)) {
///         cordite::suppressErrorDialogs();
///     }
///     ...
/// }
/// @endcode
void suppressErrorDialogs();

/// Returns the value of an environment variable, or nullopt when it is not set.
std::optional<std::string> environmentVariable(std::string_view name);

/// Returns the number of hardware threads, at least 1.
uint32_t hardwareThreadCount();

} // namespace cordite
