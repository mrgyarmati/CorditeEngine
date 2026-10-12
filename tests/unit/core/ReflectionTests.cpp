#include "cordite/core/Reflection.h"

#include "support/TestSupport.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

enum class Quality : uint8_t {
    Low,
    Medium,
    High,
};

struct Inner {
    int32_t count = 3;
    std::string label = "inner";
    int32_t revision = 0;
};

struct Sample {
    bool enabled = true;
    int32_t lives = 3;
    uint8_t small = 7;
    uint64_t big = 0;
    float speed = 1.5f;
    double precise = 0.25;
    std::string name = "sample";
    Uuid target;
    Quality quality = Quality::Medium;
    std::vector<int32_t> scores = {1, 2, 3};
    std::vector<std::string> tags;
    Inner inner;
    float cached = 9.0f;
    int32_t generation = 1;
};

const TypeInfo& innerType() {
    static const TypeInfo type = TypeBuilder<Inner>("Inner")
                                     .property("count", &Inner::count, {.minimum = 0.0, .maximum = 10.0})
                                     .property("label", &Inner::label)
                                     .property("revision", &Inner::revision, {.readOnly = true})
                                     .build();
    return type;
}

TypeInfo makeSampleType() {
    return TypeBuilder<Sample>("Sample")
        .property("enabled", &Sample::enabled, {.description = "Whether it is on"})
        .property("lives", &Sample::lives, {.minimum = 0.0, .maximum = 9.0, .step = 1.0})
        .property("small", &Sample::small)
        .property("big", &Sample::big)
        .property("speed", &Sample::speed, {.minimum = 0.0, .unit = "m/s"})
        .property("precise", &Sample::precise, {.maximum = 1e300})
        .property("name", &Sample::name)
        .property("target", &Sample::target, {.semantic = PropertySemantic::EntityReference})
        .enumProperty("quality", &Sample::quality,
                      {{Quality::Low, "low"}, {Quality::Medium, "medium"}, {Quality::High, "high"}})
        .property("scores", &Sample::scores, {.minimum = 0.0, .maximum = 100.0})
        .property("tags", &Sample::tags)
        .objectProperty("inner", &Sample::inner, innerType())
        .property("cached", &Sample::cached, {.transient = true})
        .property("generation", &Sample::generation, {.readOnly = true})
        .build();
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// Everything a sample holds, including what toJson leaves out, for "nothing changed"
/// checks.
Json snapshot(const TypeInfo& type, const Sample& sample) {
    Json state = type.toJson(&sample);
    state["cached"] = sample.cached;
    return state;
}

} // namespace

TEST_SUITE("Reflection") {
    TEST_CASE("a struct that derives from another is described with the members of both") {
        // What a component is that adds where to something that says what.
        struct Placed : Inner {
            float radius = 2.0f;
            Quality quality = Quality::Low;
            Inner nested;
        };

        const TypeInfo type =
            TypeBuilder<Placed>("Placed")
                .property("count", &Placed::count, {.minimum = 0.0, .maximum = 10.0})
                .property("label", &Inner::label)
                .property("radius", &Placed::radius)
                .enumProperty("quality", &Placed::quality, {{Quality::Low, "low"}, {Quality::High, "high"}})
                .objectProperty("nested", &Placed::nested, innerType())
                .build();
        REQUIRE(type.properties().size() == 5);

        Placed placed;
        placed.count = 4;
        placed.label = "base";
        const Json saved = type.toJson(&placed);
        CHECK(saved["count"] == 4);
        CHECK(saved["label"] == "base");
        CHECK(saved["radius"] == 2.0);
        CHECK(saved["quality"] == "low");

        // Members of the base are read, checked and written like the struct's own.
        REQUIRE(type.fromJson(&placed, json(R"({"count": 7, "label": "changed", "radius": 3.5, "quality": "high",
                                                "nested": {"count": 2}})"))
                    .has_value());
        CHECK(placed.count == 7);
        CHECK(placed.label == "changed");
        CHECK(placed.radius == 3.5f);
        CHECK(placed.quality == Quality::High);
        CHECK(placed.nested.count == 2);
        CHECK_FALSE(type.fromJson(&placed, json(R"({"count": 11})")).has_value());
        CHECK(placed.count == 7);
    }

    TEST_CASE("a type lists its properties in declaration order with their types") {
        const TypeInfo type = makeSampleType();
        CHECK(type.name() == "Sample");
        REQUIRE(type.properties().size() == 14);
        CHECK(type.properties()[0].name() == "enabled");
        CHECK(type.properties()[0].type() == PropertyType::Bool);
        CHECK(type.properties()[1].type() == PropertyType::Int);
        CHECK(type.properties()[4].type() == PropertyType::Float);
        CHECK(type.properties()[6].type() == PropertyType::String);
        CHECK(type.properties()[7].type() == PropertyType::Uuid);
        CHECK(type.properties()[8].type() == PropertyType::Enum);
        CHECK(type.properties()[9].type() == PropertyType::List);
        CHECK(type.properties()[9].elementType() == PropertyType::Int);
        CHECK(type.properties()[10].elementType() == PropertyType::String);
        CHECK(type.properties()[11].type() == PropertyType::Object);
        CHECK(type.properties()[11].objectType() == &innerType());

        CHECK(type.findProperty("speed") == &type.properties()[4]);
        CHECK(type.findProperty("missing") == nullptr);
        CHECK(type.findProperty("Speed") == nullptr);
    }

    TEST_CASE("toJson writes every saved property in declaration order") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        sample.target = Uuid::parse("01234567-89ab-4def-8edc-ba9876543210").value();
        sample.tags = {"a", "b"};

        const Json result = type.toJson(&sample);
        CHECK(result["enabled"] == true);
        CHECK(result["lives"] == 3);
        CHECK(result["small"] == 7);
        CHECK(result["speed"] == 1.5);
        CHECK(result["name"] == "sample");
        CHECK(result["target"] == "01234567-89ab-4def-8edc-ba9876543210");
        CHECK(result["quality"] == "medium");
        CHECK(result["scores"] == json("[1, 2, 3]"));
        CHECK(result["tags"] == json(R"(["a", "b"])"));
        CHECK(result["inner"] == json(R"({"count": 3, "label": "inner", "revision": 0})"));
        CHECK(result["generation"] == 1);
        // Transient properties are runtime state and are not saved.
        CHECK_FALSE(result.contains("cached"));

        // Stable key order keeps saved files stable.
        CHECK(result.begin().key() == "enabled");
        CHECK(std::prev(result.end()).key() == "generation");
    }

    TEST_CASE("floats are written the way a person would write them") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        sample.speed = 0.1f;
        sample.precise = 0.1;

        const Json result = type.toJson(&sample);
        CHECK(dumpJson(result["speed"]) == "0.1");
        CHECK(dumpJson(result["precise"]) == "0.1");

        Sample restored;
        REQUIRE(type.fromJson(&restored, result).has_value());
        CHECK(restored.speed == 0.1f);
    }

    TEST_CASE("a nil Uuid is written as null and read back") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        CHECK(type.toJson(&sample)["target"].is_null());

        sample.target = Uuid::generate();
        REQUIRE(type.fromJson(&sample, json(R"({"target": null})")).has_value());
        CHECK(sample.target.isNil());
    }

    TEST_CASE("fromJson applies the given members and leaves the rest alone") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({
            "enabled": false,
            "lives": 5,
            "speed": 2,
            "name": "changed",
            "quality": "high",
            "scores": [9, 8],
            "tags": ["x"],
            "inner": {"count": 4}
        })"));
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));

        CHECK_FALSE(sample.enabled);
        CHECK(sample.lives == 5);
        // An integer is accepted where a float is expected.
        CHECK(sample.speed == 2.0f);
        CHECK(sample.name == "changed");
        CHECK(sample.quality == Quality::High);
        CHECK(sample.scores == std::vector<int32_t>{9, 8});
        CHECK(sample.tags == std::vector<std::string>{"x"});
        CHECK(sample.inner.count == 4);
        // Not mentioned: unchanged.
        CHECK(sample.inner.label == "inner");
        CHECK(sample.small == 7);
        CHECK(sample.precise == 0.25);
    }

    TEST_CASE("toJson and fromJson round-trip") {
        const TypeInfo type = makeSampleType();
        Sample original;
        original.enabled = false;
        original.lives = 9;
        original.big = std::numeric_limits<uint64_t>::max();
        original.speed = 123.456f;
        original.precise = -1e100;
        original.name = "round trip \xe2\x9c\x93";
        original.target = Uuid::generate();
        original.quality = Quality::Low;
        original.scores = {};
        original.tags = {"one", "two", "three"};
        original.inner = {10, "deep", 4};
        original.generation = 12;

        Sample restored;
        REQUIRE(type.fromJson(&restored, type.toJson(&original)).has_value());
        CHECK(type.toJson(&restored) == type.toJson(&original));
        CHECK(restored.big == std::numeric_limits<uint64_t>::max());
        CHECK(restored.speed == original.speed);
        CHECK(restored.target == original.target);
        // Read-only properties are part of the saved state.
        CHECK(restored.generation == 12);
        CHECK(restored.inner.revision == 4);
    }

    TEST_CASE("values of the wrong JSON type are rejected with the type and property names") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Json before = snapshot(type, sample);

        for (const char* text : {
                 R"({"enabled": 1})",
                 R"({"enabled": null})",
                 R"({"lives": "three"})",
                 R"({"lives": 2.5})",
                 R"({"lives": true})",
                 R"({"speed": "fast"})",
                 R"({"name": 5})",
                 R"({"target": 12})",
                 R"({"quality": 2})",
                 R"({"scores": "1,2"})",
                 R"({"scores": [1, "x"]})",
                 R"({"tags": [1]})",
                 R"({"inner": [1, 2]})",
             }) {
            CAPTURE(text);
            const Status status = type.fromJson(&sample, json(text));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
            CHECK(status.error().message.find("Sample: property '") == 0);
        }
        CHECK(snapshot(type, sample) == before);
    }

    TEST_CASE("an invalid identifier string is rejected") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({"target": "not-a-uuid"})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("not-a-uuid") != std::string::npos);
    }

    TEST_CASE("values outside the declared range are rejected") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        const Status tooMany = type.fromJson(&sample, json(R"({"lives": 10})"));
        REQUIRE_FALSE(tooMany.has_value());
        CHECK(tooMany.error().code == ErrorCode::OutOfRange);
        CHECK(tooMany.error().message == "Sample: property 'lives': 10 is outside the allowed range [0, 9]");

        const Status tooSlow = type.fromJson(&sample, json(R"({"speed": -0.5})"));
        REQUIRE_FALSE(tooSlow.has_value());
        CHECK(tooSlow.error().message == "Sample: property 'speed': -0.5 is below the minimum 0");

        const Status tooPrecise = type.fromJson(&sample, json(R"({"precise": 1e301})"));
        REQUIRE_FALSE(tooPrecise.has_value());
        CHECK(tooPrecise.error().message.find("is above the maximum") != std::string::npos);

        CHECK_FALSE(type.fromJson(&sample, json(R"({"lives": -1})")).has_value());

        // The bounds themselves are allowed; a range open at one end has no limit there.
        CHECK(type.fromJson(&sample, json(R"({"lives": 0})")).has_value());
        CHECK(type.fromJson(&sample, json(R"({"lives": 9})")).has_value());
        CHECK(type.fromJson(&sample, json(R"({"speed": 1e30})")).has_value());
    }

    TEST_CASE("the range applies to every element of a list") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        const Status status = type.fromJson(&sample, json(R"({"scores": [5, 50, 500]})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
        CHECK(sample.scores == std::vector<int32_t>{1, 2, 3});

        REQUIRE(type.fromJson(&sample, json(R"({"scores": [0, 100]})")).has_value());
        CHECK(sample.scores == std::vector<int32_t>{0, 100});
    }

    TEST_CASE("the range of a nested object's property is enforced") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({"inner": {"count": 11}})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
        // The message leads from the outer type to the offending value.
        CHECK(status.error().message.find("Sample: property 'inner': Inner: property 'count'") == 0);
    }

    TEST_CASE("integers that do not fit the member type are rejected") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        for (const char* text : {R"({"small": 256})", R"({"small": -1})", R"({"big": -1})", R"({"lives": 3000000000})",
                                 R"({"lives": 18446744073709551615})"}) {
            CAPTURE(text);
            const Status status = type.fromJson(&sample, json(text));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::OutOfRange);
        }

        REQUIRE(type.fromJson(&sample, json(R"({"small": 255})")).has_value());
        CHECK(sample.small == 255);
        REQUIRE(type.fromJson(&sample, json(R"({"big": 18446744073709551615})")).has_value());
        CHECK(sample.big == std::numeric_limits<uint64_t>::max());
    }

    TEST_CASE("a whole number written with a fraction part is accepted as an integer") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        REQUIRE(type.fromJson(&sample, json(R"({"lives": 4.0, "small": 200.0, "big": 1e3})")).has_value());
        CHECK(sample.lives == 4);
        CHECK(sample.small == 200);
        CHECK(sample.big == 1000);

        CHECK_FALSE(type.fromJson(&sample, json(R"({"small": 0.5})")).has_value());
        CHECK_FALSE(type.fromJson(&sample, json(R"({"small": -1.0})")).has_value());
        CHECK_FALSE(type.fromJson(&sample, json(R"({"big": 1e30})")).has_value());
    }

    TEST_CASE("numbers too large for a float are rejected") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({"speed": 1e300})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
        // The same magnitude is fine for a double.
        CHECK(type.fromJson(&sample, json(R"({"precise": 1e300})")).has_value());
    }

    TEST_CASE("enum properties are read and written by name") {
        const TypeInfo type = makeSampleType();
        const PropertyInfo* quality = type.findProperty("quality");
        REQUIRE(quality != nullptr);
        REQUIRE(quality->enumOptions().size() == 3);
        CHECK(quality->enumOptions()[2].name == "high");
        CHECK(quality->enumOptions()[2].value == 2);

        Sample sample;
        CHECK(quality->get(&sample) == "medium");
        REQUIRE(quality->set(&sample, "low").has_value());
        CHECK(sample.quality == Quality::Low);

        const Status unknown = quality->set(&sample, "ultra");
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::InvalidArgument);
        CHECK(unknown.error().message == "property 'quality': expected one of [low, medium, high], found 'ultra'");
        CHECK(sample.quality == Quality::Low);

        // Names are matched exactly.
        CHECK_FALSE(quality->set(&sample, "High").has_value());
    }

    TEST_CASE("an enum value without a name shows up as a number") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        sample.quality = static_cast<Quality>(17);
        CHECK(type.findProperty("quality")->get(&sample) == 17);
    }

    TEST_CASE("unknown members are rejected or ignored on request") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        const Status rejected = type.fromJson(&sample, json(R"({"lives": 4, "livez": 5})"));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == ErrorCode::InvalidArgument);
        CHECK(rejected.error().message == "Sample has no property 'livez'");
        CHECK(sample.lives == 3);

        REQUIRE(type.fromJson(&sample, json(R"({"lives": 4, "livez": 5})"), UnknownMembers::Ignore).has_value());
        CHECK(sample.lives == 4);

        // The choice carries into nested objects.
        REQUIRE(
            type.fromJson(&sample, json(R"({"inner": {"count": 7, "extra": 1}})"), UnknownMembers::Ignore).has_value());
        CHECK(sample.inner.count == 7);
        CHECK_FALSE(type.fromJson(&sample, json(R"({"inner": {"count": 8, "extra": 1}})")).has_value());
        CHECK(sample.inner.count == 7);
    }

    TEST_CASE("a rejected update leaves the object exactly as it was") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Json before = snapshot(type, sample);

        // Several valid members precede the invalid one.
        const Status status = type.fromJson(&sample, json(R"({
            "enabled": false,
            "name": "half applied",
            "quality": "high",
            "scores": [7],
            "inner": {"count": 9, "label": "changed"},
            "lives": 99
        })"));
        REQUIRE_FALSE(status.has_value());
        CHECK(snapshot(type, sample) == before);
    }

    TEST_CASE("a rejected nested update leaves the nested object as it was") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({"inner": {"label": "changed", "count": 99}})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(sample.inner.label == "inner");
        CHECK(sample.inner.count == 3);
    }

    TEST_CASE("a patch is validated once and can be applied many times") {
        const TypeInfo type = makeSampleType();
        const PropertyPatch patch =
            unwrap(type.parse(json(R"({"name": "shared", "scores": [4, 5], "inner": {"count": 2}})")));
        CHECK_FALSE(patch.isEmpty());

        Sample first;
        Sample second;
        second.lives = 8;
        patch.applyTo(&first);
        patch.applyTo(&second);
        patch.applyTo(&second);

        for (const Sample* sample : {&first, &second}) {
            CHECK(sample->name == "shared");
            CHECK(sample->scores == std::vector<int32_t>{4, 5});
            CHECK(sample->inner.count == 2);
            CHECK(sample->inner.label == "inner");
        }
        // Properties the patch does not mention keep the value each object had.
        CHECK(first.lives == 3);
        CHECK(second.lives == 8);

        CHECK(unwrap(type.parse(json("{}"))).isEmpty());
        CHECK_FALSE(type.parse(json(R"({"lives": 99})")).has_value());
    }

    TEST_CASE("read-only properties are loaded from saved data but cannot be edited") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        const PropertyInfo* generation = type.findProperty("generation");
        REQUIRE(generation != nullptr);
        const Status direct = generation->set(&sample, 5);
        REQUIRE_FALSE(direct.has_value());
        CHECK(direct.error().code == ErrorCode::InvalidArgument);
        CHECK(direct.error().message == "property 'generation' is read-only");
        CHECK(sample.generation == 1);

        const Result<PropertyPatch> edit = type.parseEdit(json(R"({"lives": 2, "generation": 5})"));
        REQUIRE_FALSE(edit.has_value());
        CHECK(edit.error().message.find("read-only") != std::string::npos);

        // The rule reaches into nested objects.
        CHECK_FALSE(type.parseEdit(json(R"({"inner": {"revision": 2}})")).has_value());
        CHECK(type.parseEdit(json(R"({"inner": {"count": 2}})")).has_value());

        REQUIRE(type.fromJson(&sample, json(R"({"generation": 5, "inner": {"revision": 2}})")).has_value());
        CHECK(sample.generation == 5);
        CHECK(sample.inner.revision == 2);
    }

    TEST_CASE("transient properties can be edited but are not part of saved data") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        // Saved data never contains them, so finding one there is an error like any
        // other unexpected member...
        const Status loaded = type.fromJson(&sample, json(R"({"cached": 1.0})"));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().message.find("runtime state") != std::string::npos);
        // ...which Ignore skips.
        REQUIRE(type.fromJson(&sample, json(R"({"cached": 1.0})"), UnknownMembers::Ignore).has_value());
        CHECK(sample.cached == 9.0f);

        // Tools read and change them like any other property.
        const PropertyInfo* cached = type.findProperty("cached");
        REQUIRE(cached != nullptr);
        CHECK(cached->get(&sample) == 9.0);
        REQUIRE(cached->set(&sample, 2.5).has_value());
        CHECK(sample.cached == 2.5f);

        unwrap(type.parseEdit(json(R"({"cached": 4.0, "lives": 1})"))).applyTo(&sample);
        CHECK(sample.cached == 4.0f);
        CHECK(sample.lives == 1);
    }

    TEST_CASE("edits reject unknown members and invalid values") {
        const TypeInfo type = makeSampleType();
        CHECK_FALSE(type.parseEdit(json(R"({"livez": 1})")).has_value());
        CHECK_FALSE(type.parseEdit(json(R"({"lives": 99})")).has_value());
        CHECK_FALSE(type.parseEdit(json(R"("lives")")).has_value());
    }

    TEST_CASE("parsing requires an object") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json("[1, 2, 3]"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message == "Sample expects an object, found array");
        CHECK(type.fromJson(&sample, json("{}")).has_value());
    }

    TEST_CASE("a failing list element is identified by its index") {
        const TypeInfo type = makeSampleType();
        Sample sample;
        const Status status = type.fromJson(&sample, json(R"({"tags": ["ok", "fine", 3]})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("element 2") != std::string::npos);
        CHECK(sample.tags.empty());
    }

    TEST_CASE("deeply nested arrays are rejected without recursing into them") {
        const TypeInfo type = makeSampleType();
        Sample sample;

        Json nested = 1;
        for (int level = 0; level < 20000; ++level) {
            Json outer = Json::array();
            outer.push_back(std::move(nested));
            nested = std::move(outer);
        }
        Json update = Json::object();
        update["scores"] = std::move(nested);
        CHECK_FALSE(type.fromJson(&sample, update).has_value());
    }

    TEST_CASE("describe publishes a schema for tools") {
        const TypeInfo type = makeSampleType();
        const Json schema = type.describe();
        CHECK(schema["name"] == "Sample");
        REQUIRE(schema["properties"].size() == 14);

        const Json& enabled = schema["properties"][0];
        CHECK(enabled["name"] == "enabled");
        CHECK(enabled["type"] == "bool");
        CHECK(enabled["description"] == "Whether it is on");
        CHECK_FALSE(enabled.contains("readOnly"));
        CHECK_FALSE(enabled.contains("transient"));

        const Json& lives = schema["properties"][1];
        CHECK(lives["type"] == "int");
        CHECK(lives["minimum"] == 0.0);
        CHECK(lives["maximum"] == 9.0);
        CHECK(lives["step"] == 1.0);

        const Json& speed = schema["properties"][4];
        CHECK(speed["unit"] == "m/s");
        CHECK_FALSE(speed.contains("maximum"));
        CHECK_FALSE(speed.contains("step"));

        CHECK(schema["properties"][7]["semantic"] == "entity");
        CHECK(schema["properties"][8]["options"] == json(R"(["low", "medium", "high"])"));
        CHECK(schema["properties"][9]["elementType"] == "int");
        CHECK(schema["properties"][11]["objectType"] == "Inner");
        CHECK(schema["properties"][12]["transient"] == true);
        CHECK(schema["properties"][13]["readOnly"] == true);
    }

    TEST_CASE("asset references publish the kind of asset they expect") {
        struct Holder {
            Uuid mesh;
            std::string notes;
        };

        const TypeInfo type = TypeBuilder<Holder>("Holder")
                                  .property("mesh", &Holder::mesh,
                                            {.semantic = PropertySemantic::AssetReference, .referenceType = "Mesh"})
                                  .property("notes", &Holder::notes, {.semantic = PropertySemantic::Multiline})
                                  .build();

        const Json schema = type.describe();
        CHECK(schema["properties"][0]["semantic"] == "asset");
        CHECK(schema["properties"][0]["assetType"] == "Mesh");
        CHECK(schema["properties"][1]["semantic"] == "multiline");
    }

    TEST_CASE("a number whose bits are switches says so, and stays a number") {
        struct Holder {
            uint32_t layers = 5;
        };

        const TypeInfo type = TypeBuilder<Holder>("Holder")
                                  .property("layers", &Holder::layers,
                                            {.semantic = PropertySemantic::BitMask, .minimum = 0.0, .maximum = 15.0})
                                  .build();

        const Json schema = type.describe();
        CHECK(schema["properties"][0]["type"] == "int");
        CHECK(schema["properties"][0]["semantic"] == "bitMask");
        CHECK(schema["properties"][0]["maximum"] == 15.0);

        // In data it is a plain number, held to its range like any other.
        Holder holder;
        CHECK(type.toJson(&holder) == json(R"({"layers": 5})"));
        REQUIRE(type.fromJson(&holder, json(R"({"layers": 10})")).has_value());
        CHECK(holder.layers == 10);
        CHECK_FALSE(type.fromJson(&holder, json(R"({"layers": 16})")).has_value());
        CHECK(holder.layers == 10);
    }

    TEST_CASE("property type names are stable") {
        CHECK(nameOf(PropertyType::Bool) == "bool");
        CHECK(nameOf(PropertyType::Int) == "int");
        CHECK(nameOf(PropertyType::Float) == "float");
        CHECK(nameOf(PropertyType::String) == "string");
        CHECK(nameOf(PropertyType::Vec2) == "vec2");
        CHECK(nameOf(PropertyType::Vec3) == "vec3");
        CHECK(nameOf(PropertyType::Vec4) == "vec4");
        CHECK(nameOf(PropertyType::Quat) == "quat");
        CHECK(nameOf(PropertyType::Uuid) == "uuid");
        CHECK(nameOf(PropertyType::Enum) == "enum");
        CHECK(nameOf(PropertyType::Object) == "object");
        CHECK(nameOf(PropertyType::List) == "list");
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("describing two properties with one name is a programmer error") {
        using cordite::test::AssertionFailure;
        TypeBuilder<Inner> builder("Inner");
        builder.property("count", &Inner::count);
        CHECK_THROWS_AS(builder.property("count", &Inner::revision), AssertionFailure);
        CHECK_THROWS_AS(builder.property("", &Inner::revision), AssertionFailure);
    }
#endif
}
