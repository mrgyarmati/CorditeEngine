#include "cordite/renderer/RenderDebugView.h"

#include "support/TestSupport.h"

#include <cctype>
#include <set>
#include <string>

using namespace cordite;

TEST_SUITE("RenderDebugView") {
    TEST_CASE("every view has a name for tools and a name for people, all different") {
        std::set<std::string> names;
        std::set<std::string> displayNames;
        for (const RenderDebugView view : kRenderDebugViews) {
            const std::string name(nameOf(view));
            CAPTURE(name);
            CHECK(name != "unknown");
            CHECK(displayNameOf(view) != "Unknown");
            // Names for tools are single words in camelCase.
            CHECK(name.find(' ') == std::string::npos);
            CHECK(std::islower(static_cast<unsigned char>(name.front())) != 0);
            names.insert(name);
            displayNames.insert(std::string(displayNameOf(view)));
        }
        CHECK(names.size() == kRenderDebugViews.size());
        CHECK(displayNames.size() == kRenderDebugViews.size());
    }

    TEST_CASE("the names are the ones scripts and settings files use") {
        // Changing one breaks every script that names it.
        CHECK(nameOf(RenderDebugView::None) == "none");
        CHECK(nameOf(RenderDebugView::BaseColor) == "baseColor");
        CHECK(nameOf(RenderDebugView::Normals) == "normals");
        CHECK(nameOf(RenderDebugView::Roughness) == "roughness");
        CHECK(nameOf(RenderDebugView::Metallic) == "metallic");
        CHECK(nameOf(RenderDebugView::Specular) == "specular");
        CHECK(nameOf(RenderDebugView::Occlusion) == "occlusion");
        CHECK(nameOf(RenderDebugView::Emissive) == "emissive");
        CHECK(nameOf(RenderDebugView::Depth) == "depth");
        CHECK(nameOf(RenderDebugView::LightingOnly) == "lightingOnly");
        CHECK(nameOf(RenderDebugView::Wireframe) == "wireframe");
        CHECK(nameOf(RenderDebugView::Overdraw) == "overdraw");

        CHECK(displayNameOf(RenderDebugView::None) == "Lit");
        CHECK(displayNameOf(RenderDebugView::LightingOnly) == "Lighting only");
    }

    TEST_CASE("a view is found by its name, whatever the case, and nothing else is") {
        for (const RenderDebugView view : kRenderDebugViews) {
            CHECK(renderDebugViewFromName(nameOf(view)) == view);
        }
        CHECK(renderDebugViewFromName("BASECOLOR") == RenderDebugView::BaseColor);
        CHECK(renderDebugViewFromName("lightingonly") == RenderDebugView::LightingOnly);

        CHECK_FALSE(renderDebugViewFromName("").has_value());
        CHECK_FALSE(renderDebugViewFromName("base color").has_value());
        CHECK_FALSE(renderDebugViewFromName("Lit").has_value());
        CHECK_FALSE(renderDebugViewFromName("xray").has_value());
    }

    TEST_CASE("the views of the G-buffer are the ones from base color to depth") {
        CHECK_FALSE(showsGBufferChannel(RenderDebugView::None));
        for (const RenderDebugView view :
             {RenderDebugView::BaseColor, RenderDebugView::Normals, RenderDebugView::Roughness,
              RenderDebugView::Metallic, RenderDebugView::Specular, RenderDebugView::Occlusion,
              RenderDebugView::Emissive, RenderDebugView::Depth}) {
            CAPTURE(nameOf(view));
            CHECK(showsGBufferChannel(view));
        }
        CHECK_FALSE(showsGBufferChannel(RenderDebugView::LightingOnly));
        CHECK_FALSE(showsGBufferChannel(RenderDebugView::Wireframe));
        CHECK_FALSE(showsGBufferChannel(RenderDebugView::Overdraw));

        // The shader that draws them tells them apart by these numbers.
        CHECK(static_cast<int>(RenderDebugView::BaseColor) == 1);
        CHECK(static_cast<int>(RenderDebugView::Normals) == 2);
        CHECK(static_cast<int>(RenderDebugView::Roughness) == 3);
        CHECK(static_cast<int>(RenderDebugView::Metallic) == 4);
        CHECK(static_cast<int>(RenderDebugView::Specular) == 5);
        CHECK(static_cast<int>(RenderDebugView::Occlusion) == 6);
        CHECK(static_cast<int>(RenderDebugView::Emissive) == 7);
        CHECK(static_cast<int>(RenderDebugView::Depth) == 8);
    }
}
