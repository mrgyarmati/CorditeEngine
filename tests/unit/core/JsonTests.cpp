#include "cordite/core/Json.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;

TEST_SUITE("Json") {
    TEST_CASE("parseJson reads valid documents") {
        const Result<Json> json = parseJson(R"({"name": "crate", "mass": 12.5, "tags": ["prop", "wood"]})");
        REQUIRE(json.has_value());
        CHECK((*json)["name"] == "crate");
        CHECK((*json)["mass"] == 12.5);
        CHECK((*json)["tags"].size() == 2);
    }

    TEST_CASE("parseJson accepts comments") {
        const Result<Json> json = parseJson(R"({
            // line comment
            "value": 1 /* block comment */
        })");
        REQUIRE(json.has_value());
        CHECK((*json)["value"] == 1);
    }

    TEST_CASE("parseJson reports malformed input with the source name") {
        const Result<Json> json = parseJson(R"({"name": )", "scene.json");
        REQUIRE_FALSE(json.has_value());
        CHECK(json.error().code == ErrorCode::ParseError);
        CHECK(json.error().message.find("scene.json") != std::string::npos);

        CHECK_FALSE(parseJson("").has_value());
        CHECK_FALSE(parseJson("{'single': 1}").has_value());
        CHECK_FALSE(parseJson("[1, 2,]").has_value());
    }

    TEST_CASE("object keys keep their insertion order") {
        Json json = Json::object();
        json["zebra"] = 1;
        json["apple"] = 2;
        json["mango"] = 3;

        std::vector<std::string> keys;
        for (const auto& [key, value] : json.items()) {
            (void)value;
            keys.push_back(key);
        }
        CHECK(keys == std::vector<std::string>{"zebra", "apple", "mango"});

        const Result<Json> reparsed = parseJson(dumpJson(json));
        REQUIRE(reparsed.has_value());
        CHECK(dumpJson(*reparsed) == dumpJson(json));
    }

    TEST_CASE("dumpJson is deterministic and never throws on invalid UTF-8") {
        Json json = Json::object();
        json["text"] = std::string("bad \xff byte");
        std::string first;
        CHECK_NOTHROW(first = dumpJson(json));
        CHECK(first == dumpJson(json));
        CHECK(first.find("bad") != std::string::npos);
    }

    TEST_CASE("jsonGet returns typed members") {
        const Json json = *parseJson(R"({"name": "lamp", "count": 3, "ratio": 0.5, "enabled": true})");

        CHECK(*jsonGet<std::string>(json, "name") == "lamp");
        CHECK(*jsonGet<int>(json, "count") == 3);
        CHECK(*jsonGet<double>(json, "ratio") == 0.5);
        CHECK(*jsonGet<bool>(json, "enabled"));
    }

    TEST_CASE("jsonGet explains what is wrong") {
        const Json json = *parseJson(R"({"name": 42})");

        const Result<std::string> wrongType = jsonGet<std::string>(json, "name");
        REQUIRE_FALSE(wrongType.has_value());
        CHECK(wrongType.error().code == ErrorCode::ParseError);
        CHECK(wrongType.error().message.find("name") != std::string::npos);

        const Result<int> missing = jsonGet<int>(json, "count");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().message.find("count") != std::string::npos);

        const Result<int> notObject = jsonGet<int>(Json::array(), "count");
        REQUIRE_FALSE(notObject.has_value());
        CHECK(notObject.error().message.find("array") != std::string::npos);
    }

    TEST_CASE("jsonGetOr falls back for missing, null and mistyped members") {
        const Json json = *parseJson(R"({"width": 800, "title": null, "height": "tall"})");

        CHECK(jsonGetOr<int>(json, "width", 1) == 800);
        CHECK(jsonGetOr<int>(json, "depth", 7) == 7);
        CHECK(jsonGetOr<std::string>(json, "title", "untitled") == "untitled");
        CHECK(jsonGetOr<int>(json, "height", 600) == 600);
        CHECK(jsonGetOr<int>(Json(5), "width", 9) == 9);
    }

    TEST_CASE("JSON files round-trip and end with a newline") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "data.json";

        Json json = Json::object();
        json["list"] = Json::array({1, 2, 3});
        json["nested"]["flag"] = false;

        REQUIRE(writeJsonFile(file, json).has_value());

        const Result<std::string> text = readTextFile(file);
        REQUIRE(text.has_value());
        CHECK(text->back() == '\n');

        const Result<Json> read = readJsonFile(file);
        REQUIRE(read.has_value());
        CHECK(*read == json);
    }

    TEST_CASE("readJsonFile distinguishes missing files from invalid ones") {
        TempDirectory directory;

        const Result<Json> missing = readJsonFile(directory.path() / "missing.json");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);

        const std::filesystem::path broken = directory.path() / "broken.json";
        REQUIRE(writeTextFile(broken, "{ not json").has_value());
        const Result<Json> invalid = readJsonFile(broken);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::ParseError);
        CHECK(invalid.error().message.find("broken.json") != std::string::npos);
    }

    TEST_CASE("jsonTypeName names every JSON type") {
        CHECK(jsonTypeName(Json::object()) == "object");
        CHECK(jsonTypeName(Json::array()) == "array");
        CHECK(jsonTypeName(Json("text")) == "string");
        CHECK(jsonTypeName(Json(1.5)) == "number");
        CHECK(jsonTypeName(Json(true)) == "boolean");
        CHECK(jsonTypeName(Json()) == "null");
    }
}
