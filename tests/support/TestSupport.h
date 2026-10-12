#pragma once

/// @file
/// Fixtures shared by the unit test suites.

#include "cordite/core/Log.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite::test {

/// Thrown by the assertion handler the test harness installs, so tests can verify that a
/// misuse is caught: `CHECK_THROWS_AS(pool.free(foreign), AssertionFailure)`.
///
/// Assertions compile to nothing in Release, so guard such checks with
/// `#if defined(CORDITE_ENABLE_ASSERTS)`.
class AssertionFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Installs the throwing assertion handler and silences console logging. Called once by
/// the test entry point.
void installTestEnvironment();

/// What went wrong in `result`, for the message of an assertion; nothing when it holds a
/// value. Use it instead of `result.error()` there: doctest builds the message of an
/// assertion that passes too, when it is asked to list those (`--success`), and asking
/// a value for its error is undefined behavior. The `test_sources` test rejects the
/// other spelling.
template <typename T>
std::string errorOf(const Result<T>& result) {
    return result.has_value() ? std::string() : result.error().describe();
}

/// Returns the value of a Result, failing the current test with the error when there is
/// none. Use it instead of `*result`, which is undefined behavior for an error and turns
/// a clean failure into a crash.
template <typename T>
T unwrap(Result<T> result) {
    REQUIRE_MESSAGE(result.has_value(), errorOf(result));
    return std::move(*result);
}

/// Absolute path of the repository root, for tests that read checked-in data.
std::filesystem::path sourceDirectory();

/// A unique empty directory inside the build tree, deleted when the object goes out of
/// scope.
class TempDirectory {
public:
    TempDirectory();
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

/// Records log messages at or above a level while in scope.
class LogCapture {
public:
    explicit LogCapture(LogLevel minimumLevel = LogLevel::Warn);
    ~LogCapture();
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] std::vector<LogMessage> messages() const;
    [[nodiscard]] size_t count(LogLevel level) const;

    /// True when any captured message text contains `fragment`.
    [[nodiscard]] bool contains(std::string_view fragment) const;

private:
    LogLevel m_minimumLevel;
    LogLevel m_previousLevel;
    Log::ListenerId m_listener = 0;
    mutable std::mutex m_mutex;
    std::vector<LogMessage> m_messages;
};

} // namespace cordite::test
