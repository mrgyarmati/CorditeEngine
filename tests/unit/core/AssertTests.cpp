#include "cordite/core/Assert.h"

#include "cordite/core/ExternalAssert.h"

#include "support/TestSupport.h"

#include <string>

using cordite::test::AssertionFailure;

namespace {

int g_evaluations = 0;

bool countedCondition(bool result) {
    ++g_evaluations;
    return result;
}

std::string g_customExpression;
std::string g_customMessage;
int g_customLine = 0;

void recordingHandler(const cordite::AssertionInfo& info) {
    g_customExpression = std::string(info.expression);
    g_customMessage = std::string(info.message);
    g_customLine = info.line;
}

} // namespace

TEST_SUITE("Assert") {
    TEST_CASE("CORDITE_VERIFY passes silently when the condition holds") {
        CHECK_NOTHROW(CORDITE_VERIFY(1 + 1 == 2));
        CHECK_NOTHROW(CORDITE_VERIFY(true, "never shown {}", 42));
    }

    TEST_CASE("CORDITE_VERIFY reports the expression and the formatted message") {
        try {
            CORDITE_VERIFY(2 + 2 == 5, "value was {}", 4);
            FAIL("the assertion handler was not invoked");
        } catch (const AssertionFailure& failure) {
            const std::string text = failure.what();
            CHECK(text.find("2 + 2 == 5") != std::string::npos);
            CHECK(text.find("value was 4") != std::string::npos);
        }
    }

    TEST_CASE("CORDITE_VERIFY evaluates its condition exactly once") {
        g_evaluations = 0;
        CORDITE_VERIFY(countedCondition(true));
        CHECK(g_evaluations == 1);
    }

    TEST_CASE("CORDITE_ASSERT is active only when assertions are enabled") {
        g_evaluations = 0;
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS(CORDITE_ASSERT(countedCondition(false), "boom"), AssertionFailure);
        CHECK(g_evaluations == 1);
#else
        CHECK_NOTHROW(CORDITE_ASSERT(countedCondition(false), "boom"));
        CHECK(g_evaluations == 0);
#endif
    }

    TEST_CASE("setAssertionHandler swaps the handler and returns the previous one") {
        const cordite::AssertionHandler previous = cordite::setAssertionHandler(&recordingHandler);
        g_customExpression.clear();
        g_customMessage.clear();

        const int line = __LINE__ + 1;
        CORDITE_VERIFY(false, "custom {}", "handler");

        CHECK(cordite::setAssertionHandler(previous) == &recordingHandler);
        CHECK(g_customExpression == "false");
        CHECK(g_customMessage == "custom handler");
        CHECK(g_customLine == line);

        // The previous (throwing) handler is back in place.
        CHECK_THROWS_AS(CORDITE_VERIFY(false), AssertionFailure);
    }

    TEST_CASE("assertions of third-party libraries reach the same handler") {
        g_evaluations = 0;
        CHECK_NOTHROW(CORDITE_EXTERNAL_ASSERT(countedCondition(true)));
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK(g_evaluations == 1);
        try {
            CORDITE_EXTERNAL_ASSERT(countedCondition(false) && "library message");
            FAIL("the assertion handler was not invoked");
        } catch (const AssertionFailure& failure) {
            const std::string text = failure.what();
            CHECK(text.find("countedCondition(false) && \"library message\"") != std::string::npos);
            CHECK(text.find("AssertTests.cpp") != std::string::npos);
        }
#else
        // Compiled out: the expression is not even evaluated.
        CHECK(g_evaluations == 0);
        CHECK_NOTHROW(CORDITE_EXTERNAL_ASSERT(countedCondition(false)));
        CHECK(g_evaluations == 0);
#endif
    }

    TEST_CASE("the external entry point tolerates missing text") {
        const cordite::AssertionHandler previous = cordite::setAssertionHandler(&recordingHandler);
        g_customExpression = "unset";
        cordite::detail::reportExternalAssertionFailure(nullptr, nullptr, 7);
        cordite::setAssertionHandler(previous);

        CHECK(g_customExpression.empty());
        CHECK(g_customLine == 7);
    }
}
