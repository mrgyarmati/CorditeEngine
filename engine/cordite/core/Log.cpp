#include "cordite/core/Log.h"

#include "cordite/core/FileSystem.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace cordite {
namespace {

struct LogState {
    std::mutex mutex;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::shared_ptr<spdlog::logger> logger;
    std::deque<LogMessage> history;
    size_t historyCapacity = LogConfig{}.historyCapacity;
    std::vector<std::pair<Log::ListenerId, Log::Listener>> listeners;
    Log::ListenerId nextListenerId = 1;
};

std::shared_ptr<spdlog::logger> makeLogger(std::vector<spdlog::sink_ptr> sinks) {
    auto logger = std::make_shared<spdlog::logger>("cordite", sinks.begin(), sinks.end());
    // Filtering happens in Log::shouldLog so that history and listeners see the same messages.
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->flush_on(spdlog::level::warn);
    return logger;
}

std::shared_ptr<spdlog::logger> makeConsoleLogger() {
    return makeLogger({std::make_shared<spdlog::sinks::stderr_color_sink_mt>()});
}

LogState& state() {
    // Intentionally leaked: logging must stay usable during static destruction.
    static LogState* instance = [] {
        auto* created = new LogState();
        created->logger = makeConsoleLogger();
        return created;
    }();
    return *instance;
}

spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return spdlog::level::trace;
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
        case LogLevel::Critical:
            return spdlog::level::critical;
        case LogLevel::Off:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

std::string_view nameOf(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Critical:
            return "critical";
        case LogLevel::Off:
            return "off";
    }
    return "info";
}

std::optional<LogLevel> logLevelFromName(std::string_view name) {
    for (const LogLevel level : kLogLevels) {
        if (nameOf(level) == name) {
            return level;
        }
    }
    return std::nullopt;
}

Status Log::initialize(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }

    Status status;
    if (!config.file.empty()) {
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.file.native(), true));
        } catch (const spdlog::spdlog_ex& exception) {
            status =
                makeError(ErrorCode::IoError, "cannot open log file {}: {}", pathToUtf8(config.file), exception.what());
        }
    }

    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    s.logger = makeLogger(std::move(sinks));
    s.level.store(config.level);
    s.historyCapacity = config.historyCapacity;
    while (s.history.size() > s.historyCapacity) {
        s.history.pop_front();
    }
    return status;
}

void Log::shutdown() {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.logger) {
        s.logger->flush();
    }
    s.logger = makeConsoleLogger();
    s.listeners.clear();
}

void Log::setLevel(LogLevel level) {
    state().level.store(level);
}

LogLevel Log::level() {
    return state().level.load();
}

bool Log::shouldLog(LogLevel level) {
    return level != LogLevel::Off && level >= state().level.load(std::memory_order_relaxed);
}

void Log::write(LogLevel level, std::string_view category, std::string_view text) {
    if (!shouldLog(level)) {
        return;
    }

    const LogMessage message{level, std::string(category), std::string(text), std::chrono::system_clock::now()};

    LogState& s = state();
    std::shared_ptr<spdlog::logger> logger;
    std::vector<Listener> listeners;
    {
        const std::lock_guard lock(s.mutex);
        if (s.historyCapacity > 0) {
            if (s.history.size() >= s.historyCapacity) {
                s.history.pop_front();
            }
            s.history.push_back(message);
        }
        logger = s.logger;
        listeners.reserve(s.listeners.size());
        for (const auto& entry : s.listeners) {
            listeners.push_back(entry.second);
        }
    }

    logger->log(toSpdlogLevel(level), "[{}] {}", category, text);

    // Listeners run outside the lock so that a listener may log without deadlocking.
    for (const Listener& listener : listeners) {
        listener(message);
    }
}

void Log::flush() {
    LogState& s = state();
    std::shared_ptr<spdlog::logger> logger;
    {
        const std::lock_guard lock(s.mutex);
        logger = s.logger;
    }
    logger->flush();
}

std::vector<LogMessage> Log::history() {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    return {s.history.begin(), s.history.end()};
}

void Log::clearHistory() {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    s.history.clear();
}

Log::ListenerId Log::addListener(Listener listener) {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    const ListenerId id = s.nextListenerId++;
    s.listeners.emplace_back(id, std::move(listener));
    return id;
}

void Log::removeListener(ListenerId id) {
    LogState& s = state();
    const std::lock_guard lock(s.mutex);
    std::erase_if(s.listeners, [id](const auto& entry) { return entry.first == id; });
}

} // namespace cordite
