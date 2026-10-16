#include "cordite/core/Log.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;

namespace {

/// Restores the quiet test logging configuration when a test reconfigures the log.
struct LogConfigGuard {
    ~LogConfigGuard() {
        LogConfig config;
        config.console = false;
        config.level = LogLevel::Warn;
        const Status status = Log::initialize(config);
        (void)status;
        Log::clearHistory();
    }
};

} // namespace

TEST_SUITE("Log") {
    TEST_CASE("messages below the level are discarded") {
        LogCapture capture(LogLevel::Trace);
        Log::setLevel(LogLevel::Warn);

        CORDITE_LOG_INFO("Test", "dropped");
        CORDITE_LOG_WARN("Test", "kept {}", 1);
        CORDITE_LOG_ERROR("Test", "kept {}", 2);

        const std::vector<LogMessage> messages = capture.messages();
        REQUIRE(messages.size() == 2);
        CHECK(messages[0].level == LogLevel::Warn);
        CHECK(messages[0].category == "Test");
        CHECK(messages[0].text == "kept 1");
        CHECK(messages[1].level == LogLevel::Error);
        CHECK(messages[1].text == "kept 2");
    }

    TEST_CASE("LogLevel::Off silences everything") {
        LogCapture capture(LogLevel::Trace);
        Log::setLevel(LogLevel::Off);

        CORDITE_LOG_CRITICAL("Test", "nobody hears this");

        CHECK(capture.messages().empty());
        CHECK_FALSE(Log::shouldLog(LogLevel::Critical));
        CHECK_FALSE(Log::shouldLog(LogLevel::Off));
    }

    TEST_CASE("history keeps the most recent messages in order") {
        LogConfigGuard guard;
        LogConfig config;
        config.console = false;
        config.level = LogLevel::Info;
        config.historyCapacity = 3;
        REQUIRE(Log::initialize(config).has_value());
        Log::clearHistory();

        for (int index = 0; index < 5; ++index) {
            CORDITE_LOG_INFO("History", "message {}", index);
        }

        const std::vector<LogMessage> history = Log::history();
        REQUIRE(history.size() == 3);
        CHECK(history[0].text == "message 2");
        CHECK(history[1].text == "message 3");
        CHECK(history[2].text == "message 4");

        Log::clearHistory();
        CHECK(Log::history().empty());
    }

    TEST_CASE("the file sink receives messages") {
        LogConfigGuard guard;
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "engine.log";

        LogConfig config;
        config.console = false;
        config.level = LogLevel::Info;
        config.file = file;
        REQUIRE(Log::initialize(config).has_value());

        CORDITE_LOG_INFO("File", "written to disk {}", 123);
        Log::shutdown();

        const Result<std::string> contents = readTextFile(file);
        REQUIRE(contents.has_value());
        CHECK(contents->find("[File] written to disk 123") != std::string::npos);
    }

    TEST_CASE("an unopenable log file is reported and logging keeps working") {
        LogConfigGuard guard;
        TempDirectory directory;

        LogConfig config;
        config.console = false;
        config.level = LogLevel::Info;
        // A directory cannot be opened as a file.
        config.file = directory.path();
        const Status status = Log::initialize(config);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::IoError);

        Log::clearHistory();
        CORDITE_LOG_INFO("Test", "still alive");
        CHECK(Log::history().size() == 1);
    }

    TEST_CASE("a listener may log without deadlocking") {
        std::atomic<int> nested{0};
        const Log::ListenerId id = Log::addListener([&nested](const LogMessage& message) {
            if (message.category == "Outer") {
                CORDITE_LOG_WARN("Inner", "reacting to {}", message.text);
            } else if (message.category == "Inner") {
                ++nested;
            }
        });

        CORDITE_LOG_WARN("Outer", "ping");
        Log::removeListener(id);

        CHECK(nested.load() == 1);

        CORDITE_LOG_WARN("Outer", "after removal");
        CHECK(nested.load() == 1);
    }

    TEST_CASE("logging from many threads loses no messages") {
        LogCapture capture(LogLevel::Warn);

        constexpr int kThreads = 8;
        constexpr int kMessagesPerThread = 200;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([thread] {
                for (int index = 0; index < kMessagesPerThread; ++index) {
                    CORDITE_LOG_WARN("Thread", "{}:{}", thread, index);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        CHECK(capture.count(LogLevel::Warn) == static_cast<size_t>(kThreads * kMessagesPerThread));
    }

    TEST_CASE("level names are stable") {
        CHECK(nameOf(LogLevel::Trace) == "trace");
        CHECK(nameOf(LogLevel::Debug) == "debug");
        CHECK(nameOf(LogLevel::Info) == "info");
        CHECK(nameOf(LogLevel::Warn) == "warn");
        CHECK(nameOf(LogLevel::Error) == "error");
        CHECK(nameOf(LogLevel::Critical) == "critical");
        CHECK(nameOf(LogLevel::Off) == "off");
    }

    TEST_CASE("a level is found by its name, and by nothing else") {
        // Every level, in the order of the enumeration.
        for (size_t index = 0; index < kLogLevels.size(); ++index) {
            CHECK(kLogLevels[index] == static_cast<LogLevel>(index));
        }
        CHECK(kLogLevels.back() == LogLevel::Off);
        for (const LogLevel level : kLogLevels) {
            CHECK(logLevelFromName(nameOf(level)) == level);
        }
        for (const std::string_view name : {"", "Warn", "INFO", "warning", " info", "info "}) {
            CAPTURE(name);
            CHECK_FALSE(logLevelFromName(name).has_value());
        }
    }
}
