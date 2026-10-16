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

/// The file name of a program called `name` on this system: "name.exe" on Windows, the
/// name itself elsewhere.
[[nodiscard]] std::string executableFileName(std::string_view name);

/// Returns the value of an environment variable, or nullopt when it is not set.
std::optional<std::string> environmentVariable(std::string_view name);

/// Returns the directory in which programs keep what the person who runs them chose:
/// their roaming application data on Windows (%APPDATA%), ~/Library/Application Support
/// on macOS, and on Linux what xdgConfigurationDirectory() makes of the environment.
/// Empty when the system does not say. The directory need not exist yet.
std::filesystem::path userConfigurationDirectory();

/// The rule of the XDG base directories, which Linux follows, given the values of
/// XDG_CONFIG_HOME and HOME: the first when it is an absolute path, else ".config" in the
/// second when that is one, else nothing. A path that does not begin at the root, "/",
/// is not used, as the specification asks. A function of its own so that it is tested
/// on every platform.
std::filesystem::path xdgConfigurationDirectory(const std::optional<std::string>& configHome,
                                                const std::optional<std::string>& home);

/// Returns the number of hardware threads, at least 1.
uint32_t hardwareThreadCount();

} // namespace cordite
