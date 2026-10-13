#include "cordite/core/Json.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
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

    TEST_CASE("parseJson refuses an object that has a key twice") {
        // The library behind it would keep one of the two and say nothing: whoever wrote
        // the file would never learn that half of what they wrote counts for nothing.
        const Result<Json> twice = parseJson(R"({"bodies": 15, "joints": 5, "joints": 8})", "budgets.json");
        REQUIRE_FALSE(twice.has_value());
        CHECK(twice.error().code == ErrorCode::ParseError);
        CHECK(twice.error().message == "budgets.json: an object has the key \"joints\" twice");

        // However deep the object is, and whatever the two values are.
        CHECK_FALSE(parseJson(R"({"a": [1, {"b": {"c": 1, "d": 2, "c": 1}}]})").has_value());
        CHECK_FALSE(parseJson(R"({"a": {"x": 1}, "a": {"y": 2}})").has_value());
        CHECK_FALSE(parseJson(R"({"": 1, "": 2})").has_value());
        // The first of several is the one that is named.
        CHECK(parseJson(R"({"p": 1, "q": 1, "q": 2, "p": 2})", "file").error().message ==
              "file: an object has the key \"q\" twice");

        // The same key in two objects is twice in neither: beside each other, in each
        // other, or one after the other in an array.
        const Result<Json> apart =
            parseJson(R"({"a": {"name": 1, "a": {"name": 2}}, "b": {"name": 3}, "list": [{"name": 4}, {"name": 5}],
                          "name": 6})");
        REQUIRE(apart.has_value());
        CHECK((*apart)["a"]["a"]["name"] == 2);
        CHECK((*apart)["list"][1]["name"] == 5);
        CHECK((*apart)["name"] == 6);
        // Keys differ by case, and a value may be what a key is elsewhere.
        CHECK(parseJson(R"({"Name": "name", "name": "Name"})").has_value());
    }

    TEST_CASE("parseJson refuses text that is nested too deeply") {
        const auto arrays = [](int levels) {
            const auto count = static_cast<size_t>(levels);
            return std::string(count, '[') + std::string(count, ']');
        };
        const auto objects = [](int levels) {
            std::string text;
            for (int level = 1; level < levels; ++level) {
                text += R"({"inner": )";
            }
            return text + "{}" + std::string(static_cast<size_t>(levels - 1), '}');
        };
        const std::string tooDeep = "deep.json: arrays and objects are nested more than 64 deep";
        REQUIRE(kMaxJsonNesting == 64);

        // As deep as is allowed, and one deeper.
        CHECK(parseJson(arrays(kMaxJsonNesting)).has_value());
        CHECK(parseJson(objects(kMaxJsonNesting)).has_value());
        const Result<Json> deeper = parseJson(arrays(kMaxJsonNesting + 1), "deep.json");
        REQUIRE_FALSE(deeper.has_value());
        CHECK(deeper.error().code == ErrorCode::ParseError);
        CHECK(deeper.error().message == tooDeep);
        CHECK(parseJson(objects(kMaxJsonNesting + 1), "deep.json").error().message == tooDeep);

        // Text that is deep without end is refused the same way, and not followed down:
        // neither reading it nor throwing it away takes the stack.
        CHECK(parseJson(arrays(200000), "deep.json").error().message == tooDeep);
        CHECK(parseJson(objects(200000), "deep.json").error().message == tooDeep);
        // What is wrong further down is not looked at, and nothing of the text is kept.
        const std::string twiceDeepDown = std::string(100, '[') + R"({"a": 1, "a": 2})" + std::string(100, ']');
        CHECK(parseJson(twiceDeepDown, "deep.json").error().message == tooDeep);
        CHECK(parseJson(R"({"kept": 1, "deep": )" + arrays(kMaxJsonNesting) + "}", "deep.json").error().message ==
              tooDeep);

        // It is depth that counts, not how much there is side by side.
        std::string wide = "[";
        for (int item = 0; item < 5000; ++item) {
            wide += item == 0 ? "[[]]" : ",[[]]";
        }
        wide += "]";
        CHECK(unwrap(parseJson(wide)).size() == 5000);
        // And a key that is there twice is still noticed beside something deep.
        CHECK(
            parseJson(R"({"deep": )" + arrays(kMaxJsonNesting - 1) + R"(, "a": 1, "a": 2})", "file").error().message ==
            "file: an object has the key \"a\" twice");
    }

    TEST_CASE("checkJsonData lets through what can be kept and written out again") {
        const Json data =
            unwrap(parseJson(R"({"speed": 2.5, "tags": ["a", "b"], "door": {"open": true, "key": null}})"));
        CHECK(checkJsonData(data, 2).has_value());
        CHECK(checkJsonData(data, 16).has_value());
        // What is not an array or an object is nested in nothing.
        for (const Json& plain : {Json(nullptr), Json(true), Json(7), Json(-0.5), Json("text")}) {
            CHECK(checkJsonData(plain, 0).has_value());
        }

        // One level less than it has is one too few.
        const Status shallow = checkJsonData(data, 1);
        REQUIRE_FALSE(shallow.has_value());
        CHECK(shallow.error().code == ErrorCode::InvalidArgument);
        CHECK(shallow.error().message == "arrays and objects are nested too deeply");
        CHECK_FALSE(checkJsonData(Json::array(), 0).has_value());
        CHECK(checkJsonData(Json::array(), 1).has_value());
        CHECK_FALSE(checkJsonData(Json::object(), 0).has_value());
        CHECK(checkJsonData(Json::object(), 1).has_value());

        // A number that is not finite would be written as null: it is not kept.
        for (const double number : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::quiet_NaN()}) {
            Json holder = Json::object();
            holder["list"] = Json::array({1, number});
            const Status status = checkJsonData(holder, 16);
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().message == "a number that is not finite cannot be kept");
            CHECK_FALSE(checkJsonData(Json(number), 0).has_value());
        }

        // A value made in code can be as deep as memory allows; the check goes down no
        // further than it was told to.
        Json nested = 1;
        for (int level = 0; level < 20000; ++level) {
            Json outer = Json::array();
            outer.push_back(std::move(nested));
            nested = std::move(outer);
        }
        CHECK_FALSE(checkJsonData(nested, 16).has_value());
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
