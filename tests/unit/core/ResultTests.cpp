#include "cordite/core/Result.h"

#include "support/TestSupport.h"

#include <functional>
#include <string>
#include <tuple>

using namespace cordite;

namespace {

Result<int> parsePositive(int value) {
    if (value <= 0) {
        return makeError(ErrorCode::OutOfRange, "{} is not positive", value);
    }
    return value;
}

Status requirePositive(int value) {
    CORDITE_TRY(parsePositive(value));
    return {};
}

Result<int> doubled(int value) {
    CORDITE_TRY_ASSIGN(const int parsed, parsePositive(value));
    return parsed * 2;
}

Result<int> callWith(int value, const std::function<Result<int>(int)>& function) {
    return function(value);
}

/// Uses the macros several times on one line and inside the argument of another macro,
/// where every use is expanded as if it were on the same line.
Result<int> sumOfThree(int first, int second, int third) {
    // clang-format off
    CORDITE_TRY_ASSIGN(const int a, parsePositive(first)); CORDITE_TRY_ASSIGN(const int b, parsePositive(second)); CORDITE_TRY(parsePositive(third));
    // clang-format on
    CORDITE_TRY_ASSIGN(const int c, callWith(third, [](int value) -> Result<int> {
                           CORDITE_TRY(parsePositive(value));
                           CORDITE_TRY_ASSIGN(const int inner, parsePositive(value));
                           CORDITE_TRY_ASSIGN(const int again, parsePositive(inner));
                           return again;
                       }));
    return a + b + c;
}

} // namespace

TEST_SUITE("Result") {
    TEST_CASE("a successful Result holds its value") {
        const Result<int> result = parsePositive(7);
        REQUIRE(result.has_value());
        CHECK(*result == 7);
    }

    TEST_CASE("makeError formats the message and keeps the code") {
        const Result<int> result = parsePositive(-3);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::OutOfRange);
        CHECK(result.error().message == "-3 is not positive");
        CHECK(result.error().describe() == "OutOfRange: -3 is not positive");
    }

    TEST_CASE("withContext prefixes the message without changing the code") {
        const Error error{ErrorCode::IoError, "disk full"};
        const Error wrapped = error.withContext("saving scene");
        CHECK(wrapped.code == ErrorCode::IoError);
        CHECK(wrapped.message == "saving scene: disk full");
    }

    TEST_CASE("CORDITE_TRY propagates errors and continues on success") {
        CHECK(requirePositive(1).has_value());

        const Status failed = requirePositive(0);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == ErrorCode::OutOfRange);
    }

    TEST_CASE("CORDITE_TRY_ASSIGN binds the value or propagates the error") {
        const Result<int> ok = doubled(21);
        REQUIRE(ok.has_value());
        CHECK(*ok == 42);

        const Result<int> failed = doubled(-1);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message == "-1 is not positive");
    }

    TEST_CASE("the macros can share a line and nest inside each other") {
        const Result<int> sum = sumOfThree(1, 2, 3);
        REQUIRE(sum.has_value());
        CHECK(*sum == 6);

        for (const auto& [first, second, third, culprit] :
             {std::tuple{0, 2, 3, "0"}, std::tuple{1, -2, 3, "-2"}, std::tuple{1, 2, -3, "-3"}}) {
            const Result<int> failed = sumOfThree(first, second, third);
            REQUIRE_FALSE(failed.has_value());
            CHECK(failed.error().message == std::string(culprit) + " is not positive");
        }
    }

    TEST_CASE("every error code has a distinct name") {
        const ErrorCode codes[] = {
            ErrorCode::Unknown,      ErrorCode::InvalidArgument, ErrorCode::NotFound,    ErrorCode::AlreadyExists,
            ErrorCode::IoError,      ErrorCode::ParseError,      ErrorCode::Unsupported, ErrorCode::OutOfRange,
            ErrorCode::InvalidState, ErrorCode::VersionMismatch, ErrorCode::DeviceError, ErrorCode::ScriptError,
            ErrorCode::DeviceLost,
        };
        for (const ErrorCode a : codes) {
            CHECK_FALSE(nameOf(a).empty());
            for (const ErrorCode b : codes) {
                if (a != b) {
                    CHECK(nameOf(a) != nameOf(b));
                }
            }
        }
    }
}
