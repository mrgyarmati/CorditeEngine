#include "cordite/core/Assert.h"

#include "cordite/core/ExternalAssert.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"

#include <atomic>
#include <cstdlib>

namespace cordite {
namespace {

void defaultAssertionHandler(const AssertionInfo& info) {
    if (info.message.empty()) {
        CORDITE_LOG_CRITICAL("Assert", "Assertion failed: {} ({}:{})", info.expression, info.file, info.line);
    } else {
        CORDITE_LOG_CRITICAL("Assert", "Assertion failed: {} - {} ({}:{})", info.expression, info.message, info.file,
                             info.line);
    }
    Log::flush();

    if (isDebuggerPresent()) {
        CORDITE_DEBUG_BREAK();
    }
    std::abort();
}

std::atomic<AssertionHandler> g_assertionHandler{&defaultAssertionHandler};

} // namespace

AssertionHandler setAssertionHandler(AssertionHandler handler) {
    return g_assertionHandler.exchange(handler != nullptr ? handler : &defaultAssertionHandler);
}

namespace detail {

void reportAssertionFailure(std::string_view expression, std::string_view file, int line, std::string_view message) {
    const AssertionInfo info{expression, file, line, message};
    g_assertionHandler.load()(info);
}

void reportExternalAssertionFailure(const char* expression, const char* file, int line) {
    reportAssertionFailure(expression != nullptr ? expression : "", file != nullptr ? file : "", line, {});
}

} // namespace detail
} // namespace cordite
