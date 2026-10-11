#include "cordite/core/Json.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

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

    TEST_CASE("jsonGetOptional tells a missing member from a mistyped one") {
        const Json json = *parseJson(R"({"width": 800, "title": null, "height": "tall"})");

        CHECK(unwrap(jsonGetOptional<int>(json, "width")) == std::optional<int>(800));
        CHECK(unwrap(jsonGetOptional<int>(json, "depth")) == std::nullopt);
        CHECK(unwrap(jsonGetOptional<std::string>(json, "title")) == std::nullopt);

        const Result<std::optional<int>> wrongType = jsonGetOptional<int>(json, "height");
        REQUIRE_FALSE(wrongType.has_value());
        CHECK(wrongType.error().code == ErrorCode::ParseError);
        CHECK(wrongType.error().message.find("'height'") != std::string::npos);
        CHECK(wrongType.error().message.find("string") != std::string::npos);

        const Result<std::optional<int>> notObject = jsonGetOptional<int>(Json(5), "width");
        REQUIRE_FALSE(notObject.has_value());
        CHECK(notObject.error().message.find("number") != std::string::npos);
    }

    TEST_CASE("jsonFromFloat writes floats the way a person would") {
        CHECK(dumpJson(jsonFromFloat(0.1f)) == "0.1");
        CHECK(dumpJson(jsonFromFloat(-2.5f)) == "-2.5");
        CHECK(dumpJson(jsonFromFloat(1.0f)) == "1.0");
        CHECK(dumpJson(jsonFromFloat(0.0f)) == "0.0");
        CHECK(dumpJson(jsonFromFloat(1.0f / 3.0f)) == "0.33333334");
        CHECK(dumpJson(jsonFromFloat(16777216.0f)) == "16777216.0");
        // Without it, the double nearest to the float is what gets written.
        CHECK(dumpJson(Json(0.1f)) == "0.10000000149011612");

        // The result is always a floating-point number, never an integer.
        CHECK(jsonFromFloat(3.0f).is_number_float());
    }

    TEST_CASE("jsonFromFloat never changes the value") {
        const auto roundTrips = [](float value) {
            const Json json = *parseJson(dumpJson(jsonFromFloat(value)));
            const float restored = static_cast<float>(json.get<double>());
            return std::bit_cast<uint32_t>(restored) == std::bit_cast<uint32_t>(value);
        };

        for (const float value : {0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 1e-45f, 1.17549435e-38f, 3.4028235e38f,
                                  -3.4028235e38f, 1e10f, 1e-10f, 123456.79f, 8388608.5f}) {
            CAPTURE(value);
            CHECK(roundTrips(value));
        }

        // A deterministic sweep over every exponent and many bit patterns.
        uint32_t state = 0x12345678u;
        int failures = 0;
        for (int index = 0; index < 200000; ++index) {
            state = state * 1664525u + 1013904223u;
            const float value = std::bit_cast<float>(state);
            if (std::isfinite(value) && !roundTrips(value)) {
                ++failures;
            }
        }
        CHECK(failures == 0);
    }

    TEST_CASE("jsonFromFloat writes non-finite values as null") {
        CHECK(dumpJson(jsonFromFloat(std::numeric_limits<float>::infinity())) == "null");
        CHECK(dumpJson(jsonFromFloat(std::numeric_limits<float>::quiet_NaN())) == "null");
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
