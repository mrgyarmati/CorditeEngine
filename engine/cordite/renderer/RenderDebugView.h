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

    /// The light alone: every surface a plain matte grey. Translucent surfaces are left out.
    LightingOnly,
    /// The edges of every triangle.
    Wireframe,
    /// How often each pixel is drawn, hidden surfaces included: the brighter, the more.
    Overdraw,
};

inline constexpr std::array<RenderDebugView, 13> kRenderDebugViews = {
    RenderDebugView::None,     RenderDebugView::BaseColor,  RenderDebugView::Normals,      RenderDebugView::Roughness,
    RenderDebugView::Metallic, RenderDebugView::Specular,   RenderDebugView::Occlusion,    RenderDebugView::Emissive,
    RenderDebugView::Depth,    RenderDebugView::LightCount, RenderDebugView::LightingOnly, RenderDebugView::Wireframe,
    RenderDebugView::Overdraw,
};

/// The name used in files and by tools: "none", "baseColor", "lightingOnly", ...
std::string_view nameOf(RenderDebugView view);

/// The name to show a person: "Lit", "Base color", "Lighting only", ...
std::string_view displayNameOf(RenderDebugView view);

/// The view with the given nameOf(), or nothing. Upper and lower case are not told apart.
[[nodiscard]] std::optional<RenderDebugView> renderDebugViewFromName(std::string_view name);

/// True for the views that are drawn from the G-buffer and the depth, in place of the
/// lighting: BaseColor to LightCount.
[[nodiscard]] constexpr bool isDrawnFromGBuffer(RenderDebugView view) {
    return view >= RenderDebugView::BaseColor && view <= RenderDebugView::LightCount;
}

} // namespace cordite
