#include "cordite/core/Config.h"

#include "cordite/core/Json.h"

#include "support/TestSupport.h"

#include <limits>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::AssertionFailure;

namespace {

Config makeConfig() {
    Config config;
    REQUIRE(config.declare({.key = "render.vsync", .defaultValue = true, .description = "Wait for vertical sync"})
                .has_value());
    REQUIRE(config
                .declare({.key = "render.shadows.resolution",
                          .defaultValue = int64_t{2048},
                          .description = "Shadow map size",
                          .minimum = 256.0,
                          .maximum = 8192.0})
                .has_value());
    REQUIRE(config
                .declare({.key = "render.bloom.intensity",
                          .defaultValue = 0.6,
                          .description = "Bloom strength",
                          .minimum = 0.0,
                          .maximum = 4.0})
                .has_value());
    REQUIRE(
        config.declare({.key = "window.title", .defaultValue = std::string("Cordite"), .description = "Window title"})
            .has_value());
    return config;
}

} // namespace

TEST_SUITE("Config") {
    TEST_CASE("declared settings start at their defaults") {
        const Config config = makeConfig();
        CHECK(config.getBool("render.vsync"));
        CHECK(config.getInt("render.shadows.resolution") == 2048);
        CHECK(config.getFloat("render.bloom.intensity") == doctest::Approx(0.6));
        CHECK(config.getString("window.title") == "Cordite");
        CHECK(config.contains("render.vsync"));
        CHECK_FALSE(config.contains("render.missing"));
    }

    TEST_CASE("declare rejects malformed keys") {
        Config config;
        for (const char* key : {"", ".", "render.", ".render", "render..vsync", "Render.vsync", "render.vSync",
                                "render vsync", "render-vsync", "render/vsync"}) {
            const Status status = config.declare({.key = key, .defaultValue = true, .description = ""});
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
        }
        CHECK(config.entries().empty());

        CHECK(config.declare({.key = "a", .defaultValue = true, .description = ""}).has_value());
        CHECK(config.declare({.key = "render.shadow_map_2.size", .defaultValue = true, .description = ""}).has_value());
    }

    TEST_CASE("a key cannot be both a setting and a group") {
        Config config = makeConfig();

        const Status asGroup = config.declare({.key = "render", .defaultValue = true, .description = ""});
        REQUIRE_FALSE(asGroup.has_value());
        CHECK(asGroup.error().code == ErrorCode::AlreadyExists);

        const Status insideSetting =
            config.declare({.key = "render.vsync.mode", .defaultValue = true, .description = ""});
        REQUIRE_FALSE(insideSetting.has_value());
        CHECK(insideSetting.error().code == ErrorCode::AlreadyExists);

        // Sharing a textual prefix without a dot boundary is not a conflict.
        CHECK(config.declare({.key = "render.vsync_interval", .defaultValue = int64_t{1}, .description = ""})
                  .has_value());
        CHECK_NOTHROW((void)config.toJson());
    }

    TEST_CASE("declare validates ranges") {
        Config config;

        const Status onString = config.declare(
            {.key = "a.title", .defaultValue = std::string("x"), .description = "", .minimum = 0.0, .maximum = 1.0});
        REQUIRE_FALSE(onString.has_value());
        CHECK(onString.error().code == ErrorCode::InvalidArgument);

        const Status inverted = config.declare(
            {.key = "a.inverted", .defaultValue = 5.0, .description = "", .minimum = 9.0, .maximum = 1.0});
        REQUIRE_FALSE(inverted.has_value());
        CHECK(inverted.error().code == ErrorCode::OutOfRange);

        const Status defaultOutside = config.declare(
            {.key = "a.outside", .defaultValue = int64_t{50}, .description = "", .minimum = 0.0, .maximum = 10.0});
        REQUIRE_FALSE(defaultOutside.has_value());
        CHECK(defaultOutside.error().code == ErrorCode::OutOfRange);

        CHECK(config.declare({.key = "a.lower_bound_only", .defaultValue = 5.0, .description = "", .minimum = 0.0})
                  .has_value());
        CHECK(config.set("a.lower_bound_only", 1.0e12).has_value());
        CHECK_FALSE(config.set("a.lower_bound_only", -1.0).has_value());
    }

    TEST_CASE("declare rejects conflicting types") {
        Config config = makeConfig();

        const Status conflict = config.declare({.key = "render.vsync", .defaultValue = 1.0, .description = ""});
        REQUIRE_FALSE(conflict.has_value());
        CHECK(conflict.error().code == ErrorCode::AlreadyExists);

        // Same type: accepted, and the first declaration (and current value) is kept.
        REQUIRE(config.set("render.vsync", false).has_value());
        CHECK(config.declare({.key = "render.vsync", .defaultValue = true, .description = "again"}).has_value());
        CHECK_FALSE(config.getBool("render.vsync"));
    }

    TEST_CASE("set validates the key, the type and the range") {
        Config config = makeConfig();

        CHECK(config.set("render.shadows.resolution", int64_t{4096}).has_value());
        CHECK(config.getInt("render.shadows.resolution") == 4096);

        const Status unknown = config.set("render.typo", true);
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::NotFound);

        const Status wrongType = config.set("render.vsync", std::string("yes"));
        REQUIRE_FALSE(wrongType.has_value());
        CHECK(wrongType.error().code == ErrorCode::InvalidArgument);

        const Status tooLarge = config.set("render.shadows.resolution", int64_t{100000});
        REQUIRE_FALSE(tooLarge.has_value());
        CHECK(tooLarge.error().code == ErrorCode::OutOfRange);
        CHECK(config.getInt("render.shadows.resolution") == 4096);

        const Status tooSmall = config.set("render.bloom.intensity", -0.1);
        REQUIRE_FALSE(tooSmall.has_value());
        CHECK(tooSmall.error().code == ErrorCode::OutOfRange);
    }

    TEST_CASE("integers promote to float settings but floats do not demote") {
        Config config = makeConfig();

        CHECK(config.set("render.bloom.intensity", int64_t{2}).has_value());
        CHECK(config.getFloat("render.bloom.intensity") == doctest::Approx(2.0));

        const Status demote = config.set("render.shadows.resolution", 1024.5);
        REQUIRE_FALSE(demote.has_value());
        CHECK(demote.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("non-finite numbers are rejected") {
        Config config = makeConfig();
        const Status status = config.set("render.bloom.intensity", std::numeric_limits<double>::quiet_NaN());
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
    }

    TEST_CASE("setFromString parses according to the declared type") {
        Config config = makeConfig();

        CHECK(config.setFromString("render.vsync", "false").has_value());
        CHECK_FALSE(config.getBool("render.vsync"));
        CHECK(config.setFromString("render.vsync", " ON ").has_value());
        CHECK(config.getBool("render.vsync"));

        CHECK(config.setFromString("render.shadows.resolution", "1024").has_value());
        CHECK(config.getInt("render.shadows.resolution") == 1024);

        CHECK(config.setFromString("render.bloom.intensity", "1.25").has_value());
        CHECK(config.getFloat("render.bloom.intensity") == doctest::Approx(1.25));

        CHECK(config.setFromString("window.title", " My Game ").has_value());
        CHECK(config.getString("window.title") == " My Game ");

        CHECK_FALSE(config.setFromString("render.vsync", "maybe").has_value());
        CHECK_FALSE(config.setFromString("render.shadows.resolution", "12abc").has_value());
        CHECK_FALSE(config.setFromString("render.shadows.resolution", "").has_value());
        CHECK_FALSE(config.setFromString("render.bloom.intensity", "1.5x").has_value());
        CHECK_FALSE(config.setFromString("render.bloom.intensity", "").has_value());
        CHECK_FALSE(config.setFromString("render.unknown", "1").has_value());
    }

    TEST_CASE("applyJson flattens nested objects and reports every rejected key") {
        Config config = makeConfig();

        const Result<Json> json = parseJson(R"({
            "render": {
                "vsync": false,
                "bloom": { "intensity": 1.5 },
                "shadows": { "resolution": 99999 },
                "tyop": 1
            },
            "window": { "title": ["not", "a", "string"] }
        })");
        REQUIRE(json.has_value());

        const Status status = config.applyJson(*json);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("render.shadows.resolution") != std::string::npos);
        CHECK(status.error().message.find("render.tyop") != std::string::npos);
        CHECK(status.error().message.find("window.title") != std::string::npos);

        // The valid entries were still applied.
        CHECK_FALSE(config.getBool("render.vsync"));
        CHECK(config.getFloat("render.bloom.intensity") == doctest::Approx(1.5));
        CHECK(config.getInt("render.shadows.resolution") == 2048);
        CHECK(config.getString("window.title") == "Cordite");
    }

    TEST_CASE("applyJson rejects a non-object") {
        Config config = makeConfig();
        const Status status = config.applyJson(Json::array());
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::ParseError);
    }

    TEST_CASE("toJson nests keys and can emit only modified values") {
        Config config = makeConfig();
        REQUIRE(config.set("render.bloom.intensity", 2.0).has_value());

        const Json all = config.toJson();
        CHECK(all["render"]["vsync"] == true);
        CHECK(all["render"]["shadows"]["resolution"] == 2048);
        CHECK(all["window"]["title"] == "Cordite");

        const Json modified = config.toJson(true);
        CHECK(modified.size() == 1);
        CHECK(modified["render"].size() == 1);
        CHECK(modified["render"]["bloom"]["intensity"] == 2.0);
    }

    TEST_CASE("toJson output can be applied to a fresh config") {
        Config source = makeConfig();
        REQUIRE(source.set("render.vsync", false).has_value());
        REQUIRE(source.set("render.shadows.resolution", int64_t{512}).has_value());
        REQUIRE(source.set("window.title", std::string("Demo")).has_value());

        Config target = makeConfig();
        REQUIRE(target.applyJson(source.toJson()).has_value());
        CHECK(target.toJson() == source.toJson());
    }

    TEST_CASE("change callbacks fire only when a value actually changes") {
        Config config = makeConfig();

        std::vector<std::string> changes;
        const Config::CallbackId id = config.addChangeCallback(
            [&changes](std::string_view key, const ConfigValue&) { changes.emplace_back(key); });

        REQUIRE(config.set("render.vsync", false).has_value());
        REQUIRE(config.set("render.vsync", false).has_value());
        CHECK(config.set("render.shadows.resolution", int64_t{1}).has_value() == false);
        CHECK(changes == std::vector<std::string>{"render.vsync"});

        config.removeChangeCallback(id);
        REQUIRE(config.set("render.vsync", true).has_value());
        CHECK(changes.size() == 1);
    }

    TEST_CASE("resetToDefaults restores every value and notifies") {
        Config config = makeConfig();
        REQUIRE(config.set("render.vsync", false).has_value());
        REQUIRE(config.set("window.title", std::string("Changed")).has_value());

        int notifications = 0;
        config.addChangeCallback([&notifications](std::string_view, const ConfigValue&) { ++notifications; });
        config.resetToDefaults();

        CHECK(config.getBool("render.vsync"));
        CHECK(config.getString("window.title") == "Cordite");
        CHECK(notifications == 2);
        CHECK(config.toJson(true).empty());
    }

    TEST_CASE("entries are sorted by key and expose their declarations") {
        const Config config = makeConfig();
        const std::vector<ConfigEntry> entries = config.entries();
        REQUIRE(entries.size() == 4);
        CHECK(entries[0].declaration.key == "render.bloom.intensity");
        CHECK(entries[0].type() == ConfigType::Float);
        CHECK(entries[0].isDefault());
        CHECK(entries[3].declaration.key == "window.title");
        CHECK(entries[3].type() == ConfigType::String);
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("reading an undeclared or mistyped setting is a programmer error") {
        const Config config = makeConfig();
        CHECK_THROWS_AS((void)config.getBool("render.missing"), AssertionFailure);
        CHECK_THROWS_AS((void)config.getInt("render.vsync"), AssertionFailure);
    }
#endif
}
