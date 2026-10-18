#pragma once

/// @file
/// The debug views: what a view of a scene can show instead of the finished picture.
///
/// They are for looking into the renderer - is this surface as rough as it should be,
/// which way do these normals point, how often is this pixel drawn - and are chosen per
/// view in SceneRenderSettings.
///
/// @code
/// SceneRenderSettings settings;
/// settings.debugView = RenderDebugView::Normals;
/// sceneRenderer.render(commandList, world, view, targets, settings);
///
/// for (const RenderDebugView view : kRenderDebugViews) {
///     menu.add(displayNameOf(view), nameOf(view));          // "Base color", "baseColor"
/// }
/// const std::optional<RenderDebugView> chosen = renderDebugViewFromName("roughness");
/// @endcode

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

enum class RenderDebugView : uint8_t {
    /// The finished picture.
    None,

    // What can be read off the G-buffer and the depth, as a picture. The values of
    // these are known to shaders/passes/DebugView.slang.
    /// The color of the surfaces without any light.
    BaseColor,
    /// Which way the surfaces face, normal maps included: x, y and z as red, green, blue.
    Normals,
    /// White is matte, black a mirror.
    Roughness,
    /// White is metal.
    Metallic,
    /// How strongly non-metals reflect; mid-grey is the usual 4%.
    Specular,
    /// The occlusion the materials bring along: black where ambient light does not reach.
    Occlusion,
    /// The light the surfaces give off themselves.
    Emissive,
    /// How far the surfaces are from the camera: near is white.
    Depth,
    /// How many point and spot lights each pixel has to ask: dark where none reaches,
    /// blue for one, through green to red for eight and more.
    LightCount,
    /// Which cascade of the sun's shadow each pixel is looked up in: red, green, blue
    /// and yellow from the nearest on; grey where the sun casts no shadow.
    ShadowCascades,
    /// How much of the light from all around gets to each surface past what stands
    /// around it, as worked out from the screen: white is all of it. (Occlusion, above,
    /// is what the materials say about themselves.)
    AmbientOcclusion,
    /// How much of each pixel is fog: white where nothing shows through it. The sky
    /// and the background are in it too; translucent surfaces are left out.
    Fog,
    /// How far, and which way, what each pixel shows moved since the view was last drawn
    /// (MotionVectors.h): black where nothing moved; the color says which way - red to
    /// the right, yellow-green up, cyan to the left, blue-purple down - and the
    /// brightness how far, all of it at 16 pixels. The sky moves as the camera turns.
    Motion,

    /// The light alone: every surface a plain matte grey. Translucent surfaces are left out.
    LightingOnly,
    /// What the surroundings give the surfaces to reflect: every surface a perfect
    /// mirror, lit by the environment and the ambient light alone. Translucent surfaces
    /// are left out.
    Reflections,
    /// The edges of every triangle.
    Wireframe,
    /// How often each pixel is drawn, hidden surfaces included: the brighter, the more.
    Overdraw,
    /// How blurred each pixel is by the depth of field (DepthOfFieldPass): black where it
    /// is sharp, red in front of that and blue behind, as much as it is blurred. Drawn by
    /// the depth of field over the lit picture, also where the scene has none.
    Focus,
    /// The light the shafts of the sun add to each pixel, alone (LightShaftPass): black
    /// where they add none, and everywhere in a scene without them.
    LightShafts,
    /// What the lights throw towards the viewer from the fog in front of each pixel
    /// (VolumetricFog.h), alone: black where they throw nothing, and everywhere in a
    /// scene whose fog no light lights. Drawn by the lighting; translucent surfaces are
    /// left out.
    FogLight,
    /// Which environment probes light each pixel (EnvironmentProbes.h): a color for each
    /// probe of the view, by its place among them, mixed by their shares; grey for what
    /// the environment lights. Drawn by the lighting; translucent surfaces are left out.
    Probes,
    /// The level of detail each mesh is drawn at (LevelOfDetail.h): the mesh itself grey,
    /// its coarser levels green, yellow, orange and red, shaded by the way they face.
    Lod,
};

inline constexpr std::array<RenderDebugView, 23> kRenderDebugViews = {
    RenderDebugView::None,
    RenderDebugView::BaseColor,
    RenderDebugView::Normals,
    RenderDebugView::Roughness,
    RenderDebugView::Metallic,
    RenderDebugView::Specular,
    RenderDebugView::Occlusion,
    RenderDebugView::Emissive,
    RenderDebugView::Depth,
    RenderDebugView::LightCount,
    RenderDebugView::ShadowCascades,
    RenderDebugView::AmbientOcclusion,
    RenderDebugView::Fog,
    RenderDebugView::Motion,
    RenderDebugView::LightingOnly,
    RenderDebugView::Reflections,
    RenderDebugView::Wireframe,
    RenderDebugView::Overdraw,
    RenderDebugView::Focus,
    RenderDebugView::LightShafts,
    RenderDebugView::FogLight,
    RenderDebugView::Probes,
    RenderDebugView::Lod,
};

/// The name used in files and by tools: "none", "baseColor", "lightingOnly", "reflections", ...
std::string_view nameOf(RenderDebugView view);

/// The name to show a person: "Lit", "Base color", "Lighting only", "Reflections", ...
std::string_view displayNameOf(RenderDebugView view);

/// The view with the given nameOf(), or nothing. Upper and lower case are not told apart.
[[nodiscard]] std::optional<RenderDebugView> renderDebugViewFromName(std::string_view name);

/// True for the views that are drawn from the G-buffer and the depth, in place of the
/// lighting: BaseColor to Motion.
[[nodiscard]] constexpr bool isDrawnFromGBuffer(RenderDebugView view) {
    return view >= RenderDebugView::BaseColor && view <= RenderDebugView::Motion;
}

} // namespace cordite
