#include "cordite/core/StringUtils.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::AssertionFailure;

TEST_SUITE("StringUtils") {
    TEST_CASE("trim removes surrounding whitespace only") {
        CHECK(trim("  hello world \t\r\n") == "hello world");
        CHECK(trim("hello") == "hello");
        CHECK(trim("   ").empty());
        CHECK(trim("").empty());
        CHECK(trim(" a b ") == "a b");
    }

    TEST_CASE("split keeps empty parts") {
        CHECK(split("a,b,c", ',') == std::vector<std::string_view>{"a", "b", "c"});
        CHECK(split("a,,b", ',') == std::vector<std::string_view>{"a", "", "b"});
        CHECK(split(",a,", ',') == std::vector<std::string_view>{"", "a", ""});
        CHECK(split("", ',') == std::vector<std::string_view>{""});
        CHECK(split("no-delimiter", ',') == std::vector<std::string_view>{"no-delimiter"});
    }

    TEST_CASE("toLowerAscii leaves non-ASCII bytes untouched") {
        CHECK(toLowerAscii("Hello WORLD 123") == "hello world 123");
        CHECK(toLowerAscii("\xc3\x89T\xc3\x89") == "\xc3\x89t\xc3\x89");
        CHECK(toLowerAscii("").empty());
    }

    TEST_CASE("equalsIgnoreCase compares ASCII letters case-insensitively") {
        CHECK(equalsIgnoreCase("True", "tRUE"));
        CHECK(equalsIgnoreCase("", ""));
        CHECK_FALSE(equalsIgnoreCase("true", "true "));
        CHECK_FALSE(equalsIgnoreCase("abc", "abd"));
    }

    TEST_CASE("replaceAll replaces every non-overlapping occurrence") {
        CHECK(replaceAll("a.b.c", ".", "/") == "a/b/c");
        CHECK(replaceAll("aaaa", "aa", "b") == "bb");
        CHECK(replaceAll("hello", "xyz", "!") == "hello");
        CHECK(replaceAll("x", "x", "xx") == "xx");
        CHECK(replaceAll("", "a", "b").empty());
        CHECK(replaceAll("a-b", "-", "") == "ab");
    }

    TEST_CASE("hexDigitValue accepts both letter cases and rejects everything else") {
        static_assert(hexDigitValue('0') == 0 && hexDigitValue('9') == 9);
        static_assert(hexDigitValue('a') == 10 && hexDigitValue('f') == 15);
        static_assert(hexDigitValue('A') == 10 && hexDigitValue('F') == 15);

        // The characters immediately outside each accepted range.
        for (const char character : {'/', ':', '@', 'G', '`', 'g', ' ', '\0', '-'}) {
            CHECK(hexDigitValue(character) == -1);
        }
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("replaceAll with an empty pattern is a programmer error") {
        CHECK_THROWS_AS((void)replaceAll("text", "", "x"), AssertionFailure);
    }
#endif
}
