#include "cordite/core/Document.h"

#include "support/TestSupport.h"

#include <array>
#include <string>

using namespace cordite;
using cordite::test::unwrap;

namespace {

constexpr DocumentFormat kRecipe{.name = "cordite.recipe", .version = 3, .kind = "recipe"};

Json parse(std::string_view text) {
    return unwrap(parseJson(text));
}

Error headerError(std::string_view text) {
    const Result<uint32_t> version = readDocumentHeader(parse(text), kRecipe);
    REQUIRE_FALSE(version.has_value());
    return version.error();
}

} // namespace

TEST_SUITE("Document") {
    TEST_CASE("a new document starts with its format and version, in that order") {
        Json document = makeDocument(kRecipe);
        document["servings"] = 4;
        CHECK(dumpJson(document, -1) == R"({"format":"cordite.recipe","version":3,"servings":4})");
    }

    TEST_CASE("what a document writes, it reads") {
        CHECK(unwrap(readDocumentHeader(makeDocument(kRecipe), kRecipe)) == 3);
    }

    TEST_CASE("older versions are accepted and reported, so that a reader can adapt") {
        CHECK(unwrap(readDocumentHeader(parse(R"({"format": "cordite.recipe", "version": 1})"), kRecipe)) == 1);
        CHECK(unwrap(readDocumentHeader(parse(R"({"format": "cordite.recipe", "version": 2})"), kRecipe)) == 2);
        // A whole number written with a fraction part is still a whole number.
        CHECK(unwrap(readDocumentHeader(parse(R"({"format": "cordite.recipe", "version": 2.0})"), kRecipe)) == 2);
    }

    TEST_CASE("other members are not the header's business") {
        const Json document = parse(R"({"version": 3, "anything": [1, 2], "format": "cordite.recipe"})");
        CHECK(readDocumentHeader(document, kRecipe).has_value());
    }

    TEST_CASE("a document that is something else is refused with what was expected") {
        SUBCASE("not an object") {
            const Error error = headerError("[]");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message == "a recipe must be a JSON object, found array");
        }
        SUBCASE("no format") {
            const Error error = headerError(R"({"version": 3})");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message == "not a Cordite recipe: 'format' must be \"cordite.recipe\"");
        }
        SUBCASE("another format") {
            CHECK(headerError(R"({"format": "cordite.scene", "version": 3})").message ==
                  "not a Cordite recipe: 'format' must be \"cordite.recipe\"");
        }
        SUBCASE("a format that differs in case") {
            CHECK(headerError(R"({"format": "Cordite.Recipe", "version": 3})").code == ErrorCode::ParseError);
        }
        SUBCASE("a format that is not text") {
            const Error error = headerError(R"({"format": 7, "version": 3})");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message.find("format") != std::string::npos);
        }
        SUBCASE("no version") {
            const Error error = headerError(R"({"format": "cordite.recipe"})");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message == "the recipe has no 'version'");
        }
        SUBCASE("a version that is not a number") {
            const Error error = headerError(R"({"format": "cordite.recipe", "version": "3"})");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message.starts_with("'version': "));
        }
        SUBCASE("a version that is not whole") {
            CHECK(headerError(R"({"format": "cordite.recipe", "version": 2.5})").code == ErrorCode::ParseError);
        }
        SUBCASE("a negative version") {
            CHECK(headerError(R"({"format": "cordite.recipe", "version": -1})").code == ErrorCode::ParseError);
        }
    }

    TEST_CASE("a version this engine does not read is a version mismatch, not a parse error") {
        SUBCASE("zero") {
            const Error error = headerError(R"({"format": "cordite.recipe", "version": 0})");
            CHECK(error.code == ErrorCode::VersionMismatch);
            CHECK(error.message == "the recipe has format version 0, but this engine reads versions 1 to 3");
        }
        SUBCASE("newer") {
            const Error error = headerError(R"({"format": "cordite.recipe", "version": 4, "invented": "later"})");
            CHECK(error.code == ErrorCode::VersionMismatch);
            CHECK(error.message == "the recipe has format version 4, but this engine reads versions 1 to 3");
        }
    }

    TEST_CASE("unknown members are named") {
        constexpr std::array<std::string_view, 3> kAllowed = {"format", "version", "servings"};
        CHECK(rejectUnknownMembers(parse(R"({"format": "x", "servings": 2})"), kAllowed).has_value());
        CHECK(rejectUnknownMembers(parse("{}"), kAllowed).has_value());

        const Status status = rejectUnknownMembers(parse(R"({"format": "x", "servngs": 2, "other": 1})"), kAllowed);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::ParseError);
        // The first one found, in the order of the document.
        CHECK(status.error().message == "unknown member 'servngs'");

        // Names are compared exactly.
        CHECK_FALSE(rejectUnknownMembers(parse(R"({"Format": "x"})"), kAllowed).has_value());
    }

    TEST_CASE("nothing is allowed when nothing is listed") {
        CHECK(rejectUnknownMembers(parse("{}"), {}).has_value());
        CHECK_FALSE(rejectUnknownMembers(parse(R"({"a": 1})"), {}).has_value());
    }
}
