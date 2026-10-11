#include "cordite/renderer/RenderComponents.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/world/ComponentRegistry.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

using namespace cordite;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// A registry holding the renderer's components.
struct Components {
    Components() { REQUIRE(registerRendererComponents(registry).has_value()); }

    [[nodiscard]] const TypeInfo& type(std::string_view name) const {
        const ComponentDescriptor* descriptor = registry.find(name);
        REQUIRE(descriptor != nullptr);
        return descriptor->type;
    }

    ComponentRegistry registry;
};

} // namespace

TEST_SUITE("RenderComponents") {
    TEST_CASE("the renderer registers its components under the names scene files use") {
        const Components components;
        REQUIRE(components.registry.components().size() == 3);
        CHECK(components.registry.components()[0]->type.name() == "MeshRenderer");
        CHECK(components.registry.components()[1]->type.name() == "Light");
        CHECK(components.registry.components()[2]->type.name() == "AmbientLight");
        for (const ComponentDescriptor* descriptor : components.registry.components()) {
            CAPTURE(descriptor->type.name());
            CHECK(descriptor->traits.optional);
            CHECK_FALSE(descriptor->traits.description.empty());
            for (const PropertyInfo& property : descriptor->type.properties()) {
                CAPTURE(property.name());
                CHECK_FALSE(property.attributes().description.empty());
            }
        }

        // Registering twice is refused: a name stands for one type.
        ComponentRegistry registry;
        REQUIRE(registerRendererComponents(registry).has_value());
        CHECK_FALSE(registerRendererComponents(registry).has_value());
    }

    TEST_CASE("the kinds of light have the names scene files use") {
        CHECK(nameOf(LightType::Directional) == "directional");
        CHECK(nameOf(LightType::Point) == "point");
        CHECK(nameOf(LightType::Spot) == "spot");
    }

    TEST_CASE("a new light is a white point light of intensity one in the first lighting channel") {
        const LightComponent light;
        CHECK(light.type == LightType::Point);
        CHECK(light.color == Vec3(1.0f));
        CHECK(light.intensity == 1.0f);
        CHECK(light.range == 10.0f);
        CHECK(light.innerConeDegrees == 25.0f);
        CHECK(light.outerConeDegrees == 35.0f);
        CHECK(light.channels == 1);

        // What is drawn is in the first channel too, so every light shines on everything.
        CHECK(MeshRendererComponent().lightChannels == 1);
        CHECK(kAllLightChannels == 255);
    }

    TEST_CASE("a light is saved with every property and read back the same") {
        const Components components;
        const TypeInfo& type = components.type("Light");

        const LightComponent light{.type = LightType::Spot,
                                   .color = Vec3(1.0f, 0.5f, 0.25f),
                                   .intensity = 2.5f,
                                   .range = 8.0f,
                                   .innerConeDegrees = 20.0f,
                                   .outerConeDegrees = 30.0f,
                                   .channels = 6};
        const Json saved = type.toJson(&light);
        // The file format: pinned, because scenes on disk are written this way.
        CHECK(saved == json(R"({"type": "spot", "color": [1.0, 0.5, 0.25], "intensity": 2.5, "range": 8.0,
                                "innerConeDegrees": 20.0, "outerConeDegrees": 30.0, "channels": 6})"));

        LightComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.type == LightType::Spot);
        CHECK(loaded.color == light.color);
        CHECK(loaded.intensity == light.intensity);
        CHECK(loaded.range == light.range);
        CHECK(loaded.innerConeDegrees == light.innerConeDegrees);
        CHECK(loaded.outerConeDegrees == light.outerConeDegrees);
        CHECK(loaded.channels == light.channels);

        // What a file leaves out keeps its default.
        LightComponent sparse;
        REQUIRE(type.fromJson(&sparse, json(R"({"type": "directional", "intensity": 0.5})")).has_value());
        CHECK(sparse.type == LightType::Directional);
        CHECK(sparse.intensity == 0.5f);
        CHECK(sparse.range == 10.0f);
        CHECK(sparse.channels == 1);
    }

    TEST_CASE("values a light cannot have are refused, and leave the light as it was") {
        const Components components;
        const TypeInfo& type = components.type("Light");

        for (const std::string_view bad : {
                 R"({"type": "area"})",
                 R"({"type": 1})",
                 R"({"intensity": -0.1})",
                 R"({"intensity": "bright"})",
                 R"({"color": [1.0, -0.5, 0.0]})",
                 R"({"color": [1.0, 1.0]})",
                 R"({"range": 0.0})",
                 R"({"range": -3.0})",
                 R"({"innerConeDegrees": -1.0})",
                 R"({"innerConeDegrees": 89.5})",
                 R"({"outerConeDegrees": 90.0})",
                 R"({"channels": 256})",
                 R"({"channels": -1})",
                 R"({"channels": 1.5})",
                 R"({"castsShadows": true})",
             }) {
            CAPTURE(bad);
            LightComponent light;
            light.intensity = 3.0f;
            CHECK_FALSE(type.fromJson(&light, json(bad)).has_value());
            CHECK_FALSE(type.parseEdit(json(bad)).has_value());
            CHECK(light.intensity == 3.0f);
            CHECK(light.type == LightType::Point);
        }

        // The ends of the ranges are allowed.
        LightComponent light;
        REQUIRE(type.fromJson(&light, json(R"({"intensity": 0.0, "range": 0.01, "innerConeDegrees": 0.0,
                                               "outerConeDegrees": 89.0, "channels": 255, "color": [0, 0, 0]})"))
                    .has_value());
        CHECK(light.outerConeDegrees == 89.0f);
        CHECK(light.channels == 255);
        REQUIRE(type.fromJson(&light, json(R"({"channels": 0})")).has_value());
        CHECK(light.channels == 0);
        // A light may be brighter than any color: there is no upper end to brightness.
        REQUIRE(type.fromJson(&light, json(R"({"intensity": 50000.0, "color": [4.0, 4.0, 4.0]})")).has_value());
        CHECK(light.intensity == 50000.0f);
    }

    TEST_CASE("lighting channels are described as a set of switches") {
        const Components components;
        for (const auto& [typeName, propertyName] :
             std::initializer_list<std::pair<std::string_view, std::string_view>>{{"Light", "channels"},
                                                                                  {"MeshRenderer", "lightChannels"}}) {
            CAPTURE(typeName);
            const PropertyInfo* property = components.type(typeName).findProperty(propertyName);
            REQUIRE(property != nullptr);
            CHECK(property->type() == PropertyType::Int);
            CHECK(property->attributes().semantic == PropertySemantic::BitMask);
            CHECK(property->attributes().minimum == 0.0);
            CHECK(property->attributes().maximum == 255.0);
        }
    }

    TEST_CASE("a mesh renderer keeps its lighting channels in scene files, and files without them still load") {
        const Components components;
        const TypeInfo& type = components.type("MeshRenderer");

        MeshRendererComponent renderer;
        renderer.lightChannels = 5;
        const Json saved = type.toJson(&renderer);
        CHECK(saved["lightChannels"] == 5);

        MeshRendererComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.lightChannels == 5);

        // A scene written before there were lighting channels.
        MeshRendererComponent older;
        REQUIRE(type.fromJson(&older, json(R"({"mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345", "materials": []})"))
                    .has_value());
        CHECK(older.lightChannels == 1);

        CHECK_FALSE(type.fromJson(&loaded, json(R"({"lightChannels": 256})")).has_value());
        CHECK_FALSE(type.fromJson(&loaded, json(R"({"lightChannels": -1})")).has_value());
        CHECK(loaded.lightChannels == 5);
    }

    TEST_CASE("ambient light is saved as two colors and an intensity") {
        const Components components;
        const TypeInfo& type = components.type("AmbientLight");

        const AmbientLightComponent ambient{
            .skyColor = Vec3(0.25f, 0.5f, 1.0f), .groundColor = Vec3(0.5f, 0.25f, 0.125f), .intensity = 1.5f};
        const Json saved = type.toJson(&ambient);
        CHECK(saved == json(R"({"skyColor": [0.25, 0.5, 1.0], "groundColor": [0.5, 0.25, 0.125], "intensity": 1.5})"));

        AmbientLightComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.skyColor == ambient.skyColor);
        CHECK(loaded.groundColor == ambient.groundColor);
        CHECK(loaded.intensity == 1.5f);

        CHECK_FALSE(type.fromJson(&loaded, json(R"({"intensity": -1.0})")).has_value());
        CHECK_FALSE(type.fromJson(&loaded, json(R"({"skyColor": [0.0, -0.1, 0.0]})")).has_value());
        CHECK_FALSE(type.fromJson(&loaded, json(R"({"horizonColor": [0.0, 0.0, 0.0]})")).has_value());
        CHECK(loaded.intensity == 1.5f);
    }
}
