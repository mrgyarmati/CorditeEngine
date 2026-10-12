#pragma once

/// @file
/// The editor panel that shows what a frame costs.
///
/// Time on the CPU and on the GPU, what the scene renderer drew, and what it keeps on the
/// GPU: the numbers of Engine::statistics(), with the times averaged over a short while so
/// that they can be read.
///
/// @code
/// StatisticsPanel panel;
/// bool open = true;
/// panel.draw(engine.statistics(), open);                           // once per frame
///
/// const Json numbers = describeFrameStatistics(engine.statistics());  // for scripts
/// @endcode
///
/// The `editor.statistics` command returns describeFrameStatistics():
///
/// @code{.json}
/// {
///     "frame": 412,
///     "cpuMilliseconds": 0.61,
///     "gpuMilliseconds": 0.24,          // null where the GPU cannot time its work
///     // The animators that were posed, and the joints of their skeletons.
///     "animation": {"animators": 1, "joints": 6},
///     "scene": {"views": 1, "meshRenderers": 21, "culled": 1,
///               // The mesh renderers drawn in a pose, and the joint matrices sent to
///               // the GPU for them.
///               "skinned": {"meshes": 2, "jointMatrices": 48},
///               "drawCalls": 115, "instances": 137, "triangles": 17470,   // all passes together
///               // Passes over the whole picture: the lighting, and what finishes it.
///               "screenPasses": 12,
///               // The lights in view, the entries of the cluster lists they are in, and
///               // the lights left out because a view holds only so many.
///               "lights": {"count": 4, "listEntries": 1106, "dropped": 0},
///               // The shadow maps drawn, the lights that are to cast shadows but found
///               // no room for a map, and the memory of the atlas the maps share.
///               "shadows": {"maps": 10, "lightsWithout": 0, "atlasBytes": 67108864},
///               "passes": {"shadow":  {"drawCalls": 78, "instances": 94, "triangles": 12034},
///                          "depth":   {"drawCalls": 18, "instances": 21, "triangles": 2712},
///                          "base":    {"drawCalls": 18, "instances": 21, "triangles": 2712},
///                          "forward": {"drawCalls": 1, "instances": 1, "triangles": 12}}},
///     "resources": {"meshes": 11, "textures": 24, "materials": 14,
///                   "meshBytes": 118348, "textureBytes": 7689540}
/// }
/// @endcode

#include "cordite/core/Json.h"
#include "cordite/engine/Engine.h"

#include <optional>

namespace cordite {

/// The numbers of a frame as JSON; see the file comment for the layout.
[[nodiscard]] Json describeFrameStatistics(const FrameStatistics& statistics);

class StatisticsPanel {
public:
    /// Draws the panel as its own window. `open` is set to false when the user closes
    /// it. Only during a frame.
    void draw(const FrameStatistics& statistics, bool& open);

private:
    /// Times as shown: each new frame moves them a little towards its own.
    double m_cpuMilliseconds = 0.0;
    std::optional<double> m_gpuMilliseconds;
};

} // namespace cordite
