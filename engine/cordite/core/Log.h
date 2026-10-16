#pragma once

/// @file
/// Logging.
///
/// Messages carry a category naming the subsystem that produced them. They are written
/// to stderr (stdout stays free for machine-readable output), optionally to a file, and
/// kept in an in-memory history that the editor console and automation API read.
///
/// @code
/// CORDITE_LOG_INFO("Assets", "Imported {} in {:.1f} ms", name, milliseconds);
/// CORDITE_LOG_ERROR("Render", "Shader {} failed to compile: {}", name, error.message);
/// @endcode
///
/// Logging is thread-safe and works before Log::initialize() is called (stderr, Info).

#include "cordite/core/Result.h"

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

/// Every level, from the one that logs the most to the one that logs nothing.
inline constexpr std::array<LogLevel, 7> kLogLevels = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                                       LogLevel::Warn,  LogLevel::Error, LogLevel::Critical,
                                                       LogLevel::Off};

/// Returns the lowercase name of a level, e.g. "warn".
std::string_view nameOf(LogLevel level);

/// The level that nameOf() calls `name`, or nullopt when no level is called that: the case
/// counts, and "Warn" is none.
[[nodiscard]] std::optional<LogLevel> logLevelFromName(std::string_view name);

struct LogMessage {
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string text;
    std::chrono::system_clock::time_point time;
};

struct LogConfig {
    /// Messages below this level are discarded.
    LogLevel level = LogLevel::Info;
    /// Write messages to stderr.
    bool console = true;
    /// Also write messages to this file (truncated on open). Empty disables the file sink.
    std::filesystem::path file;
    /// Number of recent messages kept for Log::history().
    size_t historyCapacity = 4096;
};

class Log {
public:
    using ListenerId = uint32_t;
    using Listener = std::function<void(const LogMessage&)>;

    /// Reconfigures the sinks. Fails with IoError when the log file cannot be opened, in
    /// which case logging continues to the console only.
    static Status initialize(const LogConfig& config);

    /// Flushes and closes the sinks. Logging afterwards falls back to stderr.
    static void shutdown();

    static void setLevel(LogLevel level);
    static LogLevel level();
    static bool shouldLog(LogLevel level);

    /// Writes an already formatted message. Prefer the CORDITE_LOG_* macros.
    static void write(LogLevel level, std::string_view category, std::string_view text);
    static void flush();

    /// Returns the retained messages, oldest first.
    static std::vector<LogMessage> history();
    static void clearHistory();

    /// Registers a callback invoked for every message that passes the level filter. The
    /// callback runs on the logging thread, outside the log lock, and may itself log.
    static ListenerId addListener(Listener listener);
    static void removeListener(ListenerId id);

    template <typename... Args>
    static void format(LogLevel level, std::string_view category, fmt::format_string<Args...> pattern, Args&&... args) {
        if (shouldLog(level)) {
            write(level, category, fmt::format(pattern, std::forward<Args>(args)...));
        }
    }
};

} // namespace cordite

#define CORDITE_LOG_TRACE(category, ...) ::cordite::Log::format(::cordite::LogLevel::Trace, category, __VA_ARGS__)
#define CORDITE_LOG_DEBUG(category, ...) ::cordite::Log::format(::cordite::LogLevel::Debug, category, __VA_ARGS__)
#define CORDITE_LOG_INFO(category, ...) ::cordite::Log::format(::cordite::LogLevel::Info, category, __VA_ARGS__)
#define CORDITE_LOG_WARN(category, ...) ::cordite::Log::format(::cordite::LogLevel::Warn, category, __VA_ARGS__)
#define CORDITE_LOG_ERROR(category, ...) ::cordite::Log::format(::cordite::LogLevel::Error, category, __VA_ARGS__)
#define CORDITE_LOG_CRITICAL(category, ...) ::cordite::Log::format(::cordite::LogLevel::Critical, category, __VA_ARGS__)
