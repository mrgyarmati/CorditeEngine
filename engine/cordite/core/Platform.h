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

/// Returns the value of an environment variable, or nullopt when it is not set.
std::optional<std::string> environmentVariable(std::string_view name);

/// Returns the number of hardware threads, at least 1.
uint32_t hardwareThreadCount();

} // namespace cordite
