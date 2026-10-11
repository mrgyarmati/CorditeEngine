#include "support/TestSupport.h"

#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Uuid.h"

#include <algorithm>
#include <system_error>

namespace cordite::test {
namespace {

void throwingAssertionHandler(const AssertionInfo& info) {
    throw AssertionFailure(
        fmt::format("assertion failed: {} {} ({}:{})", info.expression, info.message, info.file, info.line));
}

} // namespace

void installTestEnvironment() {
    setAssertionHandler(&throwingAssertionHandler);

    LogConfig config;
    config.console = false;
    config.level = LogLevel::Warn;
    const Status status = Log::initialize(config);
    (void)status;
}

std::filesystem::path sourceDirectory() {
    return pathFromUtf8(CORDITE_TEST_SOURCE_DIR);
}

TempDirectory::TempDirectory() {
    m_path = pathFromUtf8(CORDITE_TEST_TEMP_DIR) / pathFromUtf8(Uuid::generate().toString());
    std::filesystem::create_directories(m_path);
}

TempDirectory::~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
}

LogCapture::LogCapture(LogLevel minimumLevel) : m_minimumLevel(minimumLevel), m_previousLevel(Log::level()) {
    if (minimumLevel < m_previousLevel) {
        Log::setLevel(minimumLevel);
    }
    m_listener = Log::addListener([this](const LogMessage& message) {
        if (message.level >= m_minimumLevel) {
            std::lock_guard lock(m_mutex);
            m_messages.push_back(message);
        }
    });
}

LogCapture::~LogCapture() {
    Log::removeListener(m_listener);
    Log::setLevel(m_previousLevel);
}

std::vector<LogMessage> LogCapture::messages() const {
    std::lock_guard lock(m_mutex);
    return m_messages;
}

size_t LogCapture::count(LogLevel level) const {
    std::lock_guard lock(m_mutex);
    return static_cast<size_t>(std::count_if(m_messages.begin(), m_messages.end(),
                                             [level](const LogMessage& message) { return message.level == level; }));
}

bool LogCapture::contains(std::string_view fragment) const {
    std::lock_guard lock(m_mutex);
    return std::any_of(m_messages.begin(), m_messages.end(), [fragment](const LogMessage& message) {
        return message.text.find(fragment) != std::string::npos;
    });
}

} // namespace cordite::test
