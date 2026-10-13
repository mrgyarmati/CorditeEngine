#include "cordite/assets/Prefab.h"

#include "cordite/core/Json.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// A prefab of two entities, as a file holds it.
constexpr const char* kCart = R"({
    "format": "cordite.prefab",
    "version": 1,
    "name": "Cart",
    "entities": [
        {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "Cart", "enabled": true, "parent": null,
         "components": {}},
        {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Wheel", "enabled": true,
         "parent": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "components": {}}
    ]
})";

/// The error of a result that has to be one.
template <typename T>
Error errorIn(const Result<T>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_SUITE("Prefab") {
    TEST_CASE("a prefab file is read as its name and its entities, and written as it was read") {
        const PrefabData prefab = unwrap(parsePrefab(json(kCart)));
        CHECK(prefab.name == "Cart");
        REQUIRE(prefab.entities.is_array());
        REQUIRE(prefab.entities.size() == 2);
        CHECK(prefab.entities[0]["name"] == "Cart");
        CHECK(prefab.entities[1]["parent"] == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");

        // Written, it is the document it was read from - and has the four members a
        // prefab file has, by these names.
        const Json document = prefabDocument(prefab);
        CHECK(document == json(kCart));
        CHECK(document.size() == 4);
        CHECK(document["format"] == "cordite.prefab");
        CHECK(document["version"] == 1);
        CHECK(kPrefabFormatVersion == 1);

        // A file need not say what it is called: whoever loads it knows the file's name.
        Json unnamed = json(kCart);
        unnamed.erase("name");
        CHECK(unwrap(parsePrefab(unnamed)).name.empty());
        unnamed["name"] = nullptr;
        CHECK(unwrap(parsePrefab(unnamed)).name.empty());
    }

    TEST_CASE("a document that is no prefab is refused, and the reason is given") {
        const auto refused = [](const Json& document) {
            const Error error = errorIn(parsePrefab(document));
            CHECK(error.code == ErrorCode::ParseError);
            return error.message;
        };
        const auto without = [](const char* member) {
            Json document = json(kCart);
            document.erase(member);
            return document;
        };
        const auto with = [](const char* member, const Json& value) {
            Json document = json(kCart);
            document[member] = value;
            return document;
        };

        // The header is the one every document of the engine has, and is checked as theirs is.
        CHECK(refused(json("[]")) == "a prefab must be a JSON object, found array");
        CHECK(refused(json("5")) == "a prefab must be a JSON object, found number");
        CHECK(refused(without("format")) == "not a Cordite prefab: 'format' must be \"cordite.prefab\"");
        CHECK(refused(with("format", "cordite.scene")) == "not a Cordite prefab: 'format' must be \"cordite.prefab\"");
        CHECK(refused(without("version")) == "the prefab has no 'version'");
        CHECK(refused(with("version", "1")).starts_with("'version': "));
        CHECK(refused(with("version", -1)).starts_with("'version': "));
        CHECK(refused(with("version", 1.5)).starts_with("'version': "));
        // A member a prefab does not have is a mistake somebody made, not something to drop.
        CHECK(refused(with("note", "made on a Tuesday")) == "unknown member 'note'");
        CHECK(refused(with("entites", Json::array())) == "unknown member 'entites'");
        CHECK(refused(with("name", 7)) == "field 'name' has the wrong type (number)");

        CHECK(refused(without("entities")) == "the prefab has no 'entities' array");
        CHECK(refused(with("entities", Json::object())) == "the prefab has no 'entities' array");
        CHECK(refused(with("entities", Json::array())) == "the prefab has no entities; it needs one at least");
        CHECK(refused(with("entities", json(R"([{"name": "Cart"}, "Wheel"])"))) ==
              "entity #1 must be a JSON object, found string");

        // More entities than a prefab may have: one too many, and exactly as many.
        Json many = Json::array();
        for (size_t index = 0; index < kMaxPrefabEntities; ++index) {
            many.push_back(Json::object());
        }
        CHECK(unwrap(parsePrefab(with("entities", many))).entities.size() == kMaxPrefabEntities);
        many.push_back(Json::object());
        CHECK(refused(with("entities", many)) == "the prefab has 10001 entities; it may have 10000 at most");

        // A file from an engine that is newer says so, and is another kind of mistake -
        // as is a version there never was.
        for (const int version : {2, 0}) {
            const Error other = errorIn(parsePrefab(with("version", version)));
            CHECK(other.code == ErrorCode::VersionMismatch);
            CHECK(other.message ==
                  fmt::format("the prefab has format version {}, but this engine reads versions 1 to 1", version));
        }
    }
}
