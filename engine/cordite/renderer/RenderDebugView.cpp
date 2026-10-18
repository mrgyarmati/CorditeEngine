#include "cordite/renderer/RenderDebugView.h"

#include "cordite/core/StringUtils.h"

namespace cordite {

std::string_view nameOf(RenderDebugView view) {
    switch (view) {
        case RenderDebugView::None:
            return "none";
        case RenderDebugView::BaseColor:
            return "baseColor";
        case RenderDebugView::Normals:
            return "normals";
        case RenderDebugView::Roughness:
            return "roughness";
        case RenderDebugView::Metallic:
            return "metallic";
        case RenderDebugView::Specular:
            return "specular";
        case RenderDebugView::Occlusion:
            return "occlusion";
        case RenderDebugView::Emissive:
            return "emissive";
        case RenderDebugView::Depth:
            return "depth";
        case RenderDebugView::LightCount:
            return "lightCount";
        case RenderDebugView::ShadowCascades:
            return "shadowCascades";
        case RenderDebugView::AmbientOcclusion:
            return "ambientOcclusion";
        case RenderDebugView::Fog:
            return "fog";
        case RenderDebugView::Motion:
            return "motion";
        case RenderDebugView::LightingOnly:
            return "lightingOnly";
        case RenderDebugView::Reflections:
            return "reflections";
        case RenderDebugView::Wireframe:
            return "wireframe";
        case RenderDebugView::Overdraw:
            return "overdraw";
        case RenderDebugView::Focus:
            return "focus";
        case RenderDebugView::LightShafts:
            return "lightShafts";
        case RenderDebugView::FogLight:
            return "fogLight";
        case RenderDebugView::Probes:
            return "probes";
        case RenderDebugView::Lod:
            return "lod";
    }
    return "unknown";
}

std::string_view displayNameOf(RenderDebugView view) {
    switch (view) {
        case RenderDebugView::None:
            return "Lit";
        case RenderDebugView::BaseColor:
            return "Base color";
        case RenderDebugView::Normals:
            return "Normals";
        case RenderDebugView::Roughness:
            return "Roughness";
        case RenderDebugView::Metallic:
            return "Metallic";
        case RenderDebugView::Specular:
            return "Specular";
        case RenderDebugView::Occlusion:
            return "Occlusion";
        case RenderDebugView::Emissive:
            return "Emissive";
        case RenderDebugView::Depth:
            return "Depth";
        case RenderDebugView::LightCount:
            return "Light count";
        case RenderDebugView::ShadowCascades:
            return "Shadow cascades";
        case RenderDebugView::AmbientOcclusion:
            return "Ambient occlusion";
        case RenderDebugView::Fog:
            return "Fog";
        case RenderDebugView::Motion:
            return "Motion";
        case RenderDebugView::LightingOnly:
            return "Lighting only";
        case RenderDebugView::Reflections:
            return "Reflections";
        case RenderDebugView::Wireframe:
            return "Wireframe";
        case RenderDebugView::Overdraw:
            return "Overdraw";
        case RenderDebugView::Focus:
            return "Depth of field";
        case RenderDebugView::LightShafts:
            return "Light shafts";
        case RenderDebugView::FogLight:
            return "Fog light";
        case RenderDebugView::Probes:
            return "Probes";
        case RenderDebugView::Lod:
            return "Levels of detail";
    }
    return "Unknown";
}

std::optional<RenderDebugView> renderDebugViewFromName(std::string_view name) {
    for (const RenderDebugView view : kRenderDebugViews) {
        if (equalsIgnoreCase(name, nameOf(view))) {
            return view;
        }
    }
    return std::nullopt;
}

} // namespace cordite
