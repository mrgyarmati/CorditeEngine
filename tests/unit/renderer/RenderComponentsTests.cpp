#include "cordite/renderer/RenderComponents.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/Uuid.h"
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
        REQUIRE(components.registry.components().size() == 6);
        CHECK(components.registry.components()[0]->type.name() == "MeshRenderer");
        CHECK(components.registry.components()[1]->type.name() == "Light");
        CHECK(components.registry.components()[2]->type.name() == "AmbientLight");
        CHECK(components.registry.components()[3]->type.name() == "Environment");
        CHECK(components.registry.components()[4]->type.name() == "Fog");
        CHECK(components.registry.components()[5]->type.name() == "PostProcess");
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

        CHECK(nameOf(ShadowResolution::Low) == "low");
        CHECK(nameOf(ShadowResolution::Medium) == "medium");
        CHECK(nameOf(ShadowResolution::High) == "high");
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
        // Shadows are asked for, light by light.
        CHECK_FALSE(light.castsShadows);
        CHECK(light.shadowBias == 1.0f);
        CHECK(light.shadowNormalOffset == 1.0f);
        CHECK(light.shadowResolution == ShadowResolution::Medium);
        CHECK(light.shadowDistance == 60.0f);
        CHECK(light.shadowCascades == 4);
        CHECK(light.shadowCascadeDistribution == 0.75f);

        // What is drawn is in the first channel too, so every light shines on everything,
        // and throws a shadow in the light of the lights that cast them.
        CHECK(MeshRendererComponent().lightChannels == 1);
        CHECK(MeshRendererComponent().castsShadows);
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
                                   .channels = 6,
                                   .castsShadows = true,
                                   .shadowBias = 1.5f,
                                   .shadowNormalOffset = 2.5f,
                                   .shadowResolution = ShadowResolution::High,
                                   .shadowDistance = 80.0f,
                                   .shadowCascades = 3,
                                   .shadowCascadeDistribution = 0.5f};
        const Json saved = type.toJson(&light);
        // The file format: pinned, because scenes on disk are written this way.
        CHECK(saved == json(R"({"type": "spot", "color": [1.0, 0.5, 0.25], "intensity": 2.5, "range": 8.0,
                                "innerConeDegrees": 20.0, "outerConeDegrees": 30.0, "channels": 6,
                                "castsShadows": true, "shadowBias": 1.5, "shadowNormalOffset": 2.5,
                                "shadowResolution": "high", "shadowDistance": 80.0, "shadowCascades": 3,
                                "shadowCascadeDistribution": 0.5})"));

        LightComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.type == LightType::Spot);
        CHECK(loaded.color == light.color);
        CHECK(loaded.intensity == light.intensity);
        CHECK(loaded.range == light.range);
        CHECK(loaded.innerConeDegrees == light.innerConeDegrees);
        CHECK(loaded.outerConeDegrees == light.outerConeDegrees);
        CHECK(loaded.channels == light.channels);
        CHECK(loaded.castsShadows);
        CHECK(loaded.shadowBias == 1.5f);
        CHECK(loaded.shadowNormalOffset == 2.5f);
        CHECK(loaded.shadowResolution == ShadowResolution::High);
        CHECK(loaded.shadowDistance == 80.0f);
        CHECK(loaded.shadowCascades == 3);
        CHECK(loaded.shadowCascadeDistribution == 0.5f);

        // What a file leaves out keeps its default.
        LightComponent sparse;
        REQUIRE(type.fromJson(&sparse, json(R"({"type": "directional", "intensity": 0.5})")).has_value());
        CHECK(sparse.type == LightType::Directional);
        CHECK(sparse.intensity == 0.5f);
        CHECK(sparse.range == 10.0f);
        CHECK(sparse.channels == 1);
        // A scene written before lights cast shadows: they still do not.
        CHECK_FALSE(sparse.castsShadows);
        CHECK(sparse.shadowCascades == 4);
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
                 R"({"castsShadows": 1})",
                 R"({"shadowBias": -0.5})",
                 R"({"shadowBias": 16.5})",
                 R"({"shadowNormalOffset": -1.0})",
                 R"({"shadowResolution": "ultra"})",
                 R"({"shadowDistance": 0.5})",
                 R"({"shadowDistance": 2001.0})",
                 R"({"shadowCascades": 0})",
                 R"({"shadowCascades": 5})",
                 R"({"shadowCascades": 2.5})",
                 R"({"shadowCascadeDistribution": 1.5})",
                 R"({"volumetric": true})",
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
        REQUIRE(type.fromJson(&light, json(R"({"shadowBias": 0.0, "shadowNormalOffset": 16.0, "shadowDistance": 1.0,
                                               "shadowCascades": 1, "shadowCascadeDistribution": 1.0,
                                               "shadowResolution": "low", "castsShadows": true})"))
                    .has_value());
        CHECK(light.shadowCascades == 1);
        CHECK(light.shadowResolution == ShadowResolution::Low);
        CHECK(light.castsShadows);
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
        renderer.castsShadows = false;
        const Json saved = type.toJson(&renderer);
        CHECK(saved["lightChannels"] == 5);
        CHECK(saved["castsShadows"] == false);

        MeshRendererComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.lightChannels == 5);
        CHECK_FALSE(loaded.castsShadows);

        // A scene written before there were lighting channels.
        MeshRendererComponent older;
        REQUIRE(type.fromJson(&older, json(R"({"mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345", "materials": []})"))
                    .has_value());
        CHECK(older.lightChannels == 1);
        CHECK(older.castsShadows);

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

    TEST_CASE("an environment is saved as an image, how bright and how turned it is, and whether it is seen") {
        const Components components;
        const TypeInfo& type = components.type("Environment");

        // A new one has no image yet, and shows the one it gets as it is.
        const EnvironmentComponent fresh;
        CHECK(fresh.texture.isNil());
        CHECK(fresh.intensity == 1.0f);
        CHECK(fresh.rotationDegrees == 0.0f);
        CHECK(fresh.visible);

        const EnvironmentComponent environment{.texture = *Uuid::parse("3f0c8d5e-6f0a-4f38-9d3b-1b2a7c0de481"),
                                               .intensity = 2.5f,
                                               .rotationDegrees = -45.0f,
                                               .visible = false};
        const Json saved = type.toJson(&environment);
        CHECK(saved == json(R"({"texture": "3f0c8d5e-6f0a-4f38-9d3b-1b2a7c0de481", "intensity": 2.5,
                                "rotationDegrees": -45.0, "visible": false})"));

        EnvironmentComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.texture == environment.texture);
        CHECK(loaded.intensity == 2.5f);
        CHECK(loaded.rotationDegrees == -45.0f);
        CHECK_FALSE(loaded.visible);

        // The image is a reference to a texture asset, which is what an editor offers
        // and what a scene is checked for.
        const PropertyInfo* texture = type.findProperty("texture");
        REQUIRE(texture != nullptr);
        CHECK(texture->attributes().semantic == PropertySemantic::AssetReference);
        CHECK(texture->attributes().referenceType == "Texture");

        for (const char* text : {
                 R"({"intensity": -0.5})",
                 R"({"intensity": 1001.0})",
                 R"({"rotationDegrees": 400.0})",
                 R"({"rotationDegrees": -361.0})",
                 R"({"texture": "sky.hdr"})",
                 R"({"texture": 7})",
                 R"({"visible": "yes"})",
                 R"({"exposure": 1.0})",
                 R"({"intensity": 3.0, "rotationDegrees": 1000.0})",
             }) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&loaded, json(text)).has_value());
            CHECK(loaded.intensity == 2.5f);
            CHECK(loaded.rotationDegrees == -45.0f);
        }
    }

    TEST_CASE("a fog is saved as a color, how dense it is and how it thins out, and what it makes of the sun") {
        const Components components;
        const TypeInfo& type = components.type("Fog");

        // A new one is a light haze that hangs low and has no sun in it.
        const FogComponent fresh;
        CHECK(fresh.color == Vec3(0.5f, 0.6f, 0.7f));
        CHECK(fresh.density == 0.02f);
        CHECK(fresh.heightFalloff == 0.2f);
        CHECK(fresh.startDistance == 0.0f);
        CHECK(fresh.maxOpacity == 1.0f);
        CHECK(fresh.sunColor == Vec3(0.0f));
        CHECK(fresh.sunExponent == 8.0f);

        const FogComponent fog{.color = Vec3(0.25f, 0.5f, 0.75f),
                               .density = 0.125f,
                               .heightFalloff = 0.5f,
                               .startDistance = 4.0f,
                               .maxOpacity = 0.75f,
                               .sunColor = Vec3(2.0f, 1.0f, 0.5f),
                               .sunExponent = 16.0f};
        const Json saved = type.toJson(&fog);
        CHECK(saved == json(R"({"color": [0.25, 0.5, 0.75], "density": 0.125, "heightFalloff": 0.5,
                                "startDistance": 4.0, "maxOpacity": 0.75, "sunColor": [2.0, 1.0, 0.5],
                                "sunExponent": 16.0})"));

        FogComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.color == fog.color);
        CHECK(loaded.density == 0.125f);
        CHECK(loaded.heightFalloff == 0.5f);
        CHECK(loaded.startDistance == 4.0f);
        CHECK(loaded.maxOpacity == 0.75f);
        CHECK(loaded.sunColor == fog.sunColor);
        CHECK(loaded.sunExponent == 16.0f);

        // Its colors are colors to an editor, and may be brighter than white.
        for (const char* name : {"color", "sunColor"}) {
            CAPTURE(std::string(name));
            const PropertyInfo* color = type.findProperty(name);
            REQUIRE(color != nullptr);
            CHECK(color->attributes().semantic == PropertySemantic::Color);
            CHECK(color->attributes().minimum == 0.0);
            CHECK_FALSE(color->attributes().maximum.has_value());
        }

        for (const char* text : {
                 R"({"density": -0.01})",
                 R"({"density": 10.5})",
                 R"({"heightFalloff": -1.0})",
                 R"({"heightFalloff": 11.0})",
                 R"({"startDistance": -1.0})",
                 R"({"maxOpacity": 1.5})",
                 R"({"maxOpacity": -0.1})",
                 R"({"sunExponent": 0.5})",
                 R"({"sunExponent": 65.0})",
                 R"({"color": [-0.1, 0.0, 0.0]})",
                 R"({"sunColor": [0.0, 0.0]})",
                 R"({"color": "grey"})",
                 R"({"height": 3.0})",
                 R"({"density": 0.5, "maxOpacity": 2.0})",
             }) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&loaded, json(text)).has_value());
            CHECK(loaded.density == 0.125f);
            CHECK(loaded.maxOpacity == 0.75f);
            CHECK(loaded.color == fog.color);
        }
    }

    TEST_CASE("a new post-process component applies everywhere and changes little") {
        const PostProcessComponent component;
        CHECK(component.global);
        CHECK(component.priority == 0);
        CHECK(component.blendDistance == 1.0f);
        CHECK(component.weight == 1.0f);
        // Its settings are those a view has when nothing is said.
        const PostProcessSettings settings;
        CHECK(component.exposureMode == ExposureMode::Manual);
        CHECK(component.exposure == 0.0f);
        CHECK(component.toneMapper == ToneMapper::Filmic);
        CHECK(component.bloomIntensity == settings.bloomIntensity);
        CHECK(component.bloomThreshold == 1.0f);
        CHECK(component.contrast == 1.0f);
        CHECK(component.saturation == 1.0f);
        CHECK(component.lift == Vec3(0.0f));
        CHECK(component.gamma == Vec3(1.0f));
        CHECK(component.gain == Vec3(1.0f));
        CHECK(component.colorLut.isNil());
        CHECK(component.vignette == 0.0f);
        CHECK(component.chromaticAberration == 0.0f);
        CHECK(component.filmGrain == 0.0f);
        // What stands in the light from all around darkens it as the geometry says,
        // looked for half a metre around.
        CHECK(component.ambientOcclusionIntensity == 1.0f);
        CHECK(component.ambientOcclusionRadius == 0.5f);
        CHECK(component.ambientOcclusionBias == 2.0f);
    }

    TEST_CASE("a post-process component is saved with every property and read back the same") {
        const Components components;
        const TypeInfo& type = components.type("PostProcess");

        PostProcessComponent component;
        component.global = false;
        component.priority = 3;
        component.blendDistance = 2.5f;
        component.weight = 0.75f;
        component.exposureMode = ExposureMode::Automatic;
        component.exposure = -0.5f;
        component.autoExposureMinimum = -2.0f;
        component.autoExposureMaximum = 4.0f;
        component.adaptationSpeedUp = 5.0f;
        component.adaptationSpeedDown = 0.5f;
        component.toneMapper = ToneMapper::Aces;
        component.bloomIntensity = 0.25f;
        component.bloomThreshold = 1.5f;
        component.bloomScatter = 0.5f;
        component.temperature = 0.25f;
        component.tint = -0.125f;
        component.contrast = 1.25f;
        component.saturation = 0.75f;
        component.lift = Vec3(0.0f, 0.125f, 0.25f);
        component.gamma = Vec3(0.5f, 1.0f, 2.0f);
        component.gain = Vec3(1.5f, 1.0f, 0.5f);
        component.colorLut = *Uuid::parse("11111111-2222-4333-8444-555555555555");
        component.colorLutIntensity = 0.5f;
        component.vignette = 0.375f;
        component.chromaticAberration = 0.25f;
        component.filmGrain = 0.125f;
        component.ambientOcclusionIntensity = 1.5f;
        component.ambientOcclusionRadius = 0.75f;
        component.ambientOcclusionBias = 4.0f;
        component.focusDistance = 12.5f;
        component.focusRange = 1.5f;
        component.focusTransition = 6.0f;
        component.nearBlur = 0.25f;
        component.farBlur = 0.75f;
        component.blurSize = 0.015625f;

        const Json saved = type.toJson(&component);
        CHECK(saved == json(R"({
            "global": false, "priority": 3, "blendDistance": 2.5, "weight": 0.75,
            "exposureMode": "automatic", "exposure": -0.5,
            "autoExposureMinimum": -2.0, "autoExposureMaximum": 4.0,
            "adaptationSpeedUp": 5.0, "adaptationSpeedDown": 0.5,
            "toneMapper": "aces",
            "bloomIntensity": 0.25, "bloomThreshold": 1.5, "bloomScatter": 0.5,
            "temperature": 0.25, "tint": -0.125, "contrast": 1.25, "saturation": 0.75,
            "lift": [0.0, 0.125, 0.25], "gamma": [0.5, 1.0, 2.0], "gain": [1.5, 1.0, 0.5],
            "colorLut": "11111111-2222-4333-8444-555555555555", "colorLutIntensity": 0.5,
            "vignette": 0.375, "chromaticAberration": 0.25, "filmGrain": 0.125,
            "ambientOcclusionIntensity": 1.5, "ambientOcclusionRadius": 0.75, "ambientOcclusionBias": 4.0,
            "focusDistance": 12.5, "focusRange": 1.5, "focusTransition": 6.0,
            "nearBlur": 0.25, "farBlur": 0.75, "blurSize": 0.015625
        })"));

        PostProcessComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(type.toJson(&loaded) == saved);
        CHECK(loaded.exposureMode == ExposureMode::Automatic);
        CHECK(loaded.toneMapper == ToneMapper::Aces);
        CHECK(loaded.gamma == Vec3(0.5f, 1.0f, 2.0f));
        CHECK(loaded.colorLut == component.colorLut);

        // Every tone mapper and both exposure modes go by their names.
        for (const ToneMapper toneMapper : kToneMappers) {
            component.toneMapper = toneMapper;
            CHECK(type.toJson(&component)["toneMapper"] == std::string(nameOf(toneMapper)));
        }
        REQUIRE(type.fromJson(&loaded, json(R"({"toneMapper": "linear", "exposureMode": "manual"})")).has_value());
        CHECK(loaded.toneMapper == ToneMapper::Linear);
        CHECK(loaded.exposureMode == ExposureMode::Manual);

        // The widest blur is taken as it is written, and nothing wider.
        REQUIRE(type.fromJson(&loaded, json(R"({"blurSize": 0.03})")).has_value());
        CHECK(loaded.blurSize == kMaxBlurSize);
        CHECK_FALSE(type.fromJson(&loaded, json(R"({"blurSize": 0.031})")).has_value());

        // The lookup table is a reference to a texture.
        const PropertyInfo* lut = type.findProperty("colorLut");
        REQUIRE(lut != nullptr);
        CHECK(lut->attributes().semantic == PropertySemantic::AssetReference);
        CHECK(lut->attributes().referenceType == "Texture");
    }

    TEST_CASE("values a post-process component cannot have are refused, and leave it as it was") {
        const Components components;
        const TypeInfo& type = components.type("PostProcess");
        PostProcessComponent component;
        component.exposure = 1.0f;

        for (const char* text : {
                 R"({"weight": 1.5})",
                 R"({"weight": -0.1})",
                 R"({"blendDistance": -1.0})",
                 R"({"priority": 5000})",
                 R"({"exposure": 17.0})",
                 R"({"autoExposureMinimum": -20.0})",
                 R"({"adaptationSpeedUp": -1.0})",
                 R"({"toneMapper": "reinhard"})",
                 R"({"exposureMode": "aperture"})",
                 R"({"bloomIntensity": -0.5})",
                 R"({"bloomScatter": 1.5})",
                 R"({"temperature": 2.0})",
                 R"({"contrast": 0.1})",
                 R"({"saturation": 3.0})",
                 R"({"lift": [0.0, 2.0, 0.0]})",
                 R"({"gamma": [1.0, 0.0, 1.0]})",
                 R"({"gain": [5.0, 1.0, 1.0]})",
                 R"({"colorLut": "not an id"})",
                 R"({"colorLutIntensity": 2.0})",
                 R"({"vignette": 1.5})",
                 R"({"chromaticAberration": -0.5})",
                 R"({"filmGrain": 2.0})",
                 R"({"ambientOcclusionIntensity": -0.5})",
                 R"({"ambientOcclusionIntensity": 4.5})",
                 R"({"ambientOcclusionRadius": 0.0})",
                 R"({"ambientOcclusionRadius": 11.0})",
                 R"({"ambientOcclusionBias": -1.0})",
                 R"({"ambientOcclusionBias": 31.0})",
                 R"({"sharpen": 1.0})",
                 R"({"exposure": 0.5, "vignette": 9.0})",
             }) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&component, json(text)).has_value());
            CHECK(component.exposure == 1.0f);
        }
    }
}
