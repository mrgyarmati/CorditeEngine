#pragma once

/// @file
/// The handles in the viewport that move, turn and scale what is selected: where they
/// are, which one the pointer is over, and what dragging one does - arithmetic alone,
/// tested without an interface. ViewportPanel draws them and applies what they say.
///
/// @code
/// const GizmoFrame frame = gizmoFrame(world.worldMatrix(entity), GizmoSpace::World, view, viewportSize);
/// const GizmoHandle handle = pickHandle(GizmoMode::Translate, frame, view, viewportSize, pointer);
/// if (handle != GizmoHandle::None) {
///     const GizmoDrag drag = beginDrag(GizmoMode::Translate, handle, frame, view, viewportSize, pointer);
///     // ... while the button is held:
///     const Vec3 offset = dragTranslation(drag, view, viewportSize, movedPointer);
/// }
/// @endcode
///
/// A gizmo is as large on the screen wherever it is (kGizmoScreenSize): its frame says
/// how many metres that is where the entity stands. Translation and scale follow the
/// pointer along an axis or across a plane exactly; rotation turns by the angle the
/// pointer swept around the centre of the gizmo, as seen on the screen.

#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/DebugDraw.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

enum class GizmoMode : uint8_t {
    Translate,
    Rotate,
    Scale,
};

inline constexpr std::array<GizmoMode, 3> kGizmoModes = {GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale};

/// The name of a mode in commands: "translate", "rotate", "scale".
[[nodiscard]] std::string_view nameOf(GizmoMode mode);
/// The mode of a name, whatever its case; none for a name no mode has.
[[nodiscard]] std::optional<GizmoMode> gizmoModeFromName(std::string_view name);

/// Whether the axes of a gizmo are those of the world or those of the entity. Scale is
/// always along the entity's own axes, which are the axes its scale is kept in.
enum class GizmoSpace : uint8_t {
    World,
    Local,
};

[[nodiscard]] std::string_view nameOf(GizmoSpace space);
[[nodiscard]] std::optional<GizmoSpace> gizmoSpaceFromName(std::string_view name);

/// The part of a gizmo the pointer takes hold of.
enum class GizmoHandle : uint8_t {
    None,
    /// An axis: an arrow, a ring around it, or an axis that ends in a box.
    X,
    Y,
    Z,
    /// The plane of two axes, as a small square between them; translation only.
    XY,
    YZ,
    XZ,
    /// The centre: scaling the same along all three axes.
    All,
};

/// How long an axis of a gizmo is on the screen, in pixels at 100% scaling.
inline constexpr float kGizmoScreenSize = 96.0f;
/// How near the pointer must be to a handle, in pixels, to take hold of it.
inline constexpr float kGizmoPickPixels = 8.0f;

/// Where a gizmo is and which way its axes point.
struct GizmoFrame {
    Vec3 origin{0.0f};
    /// The axes, of unit length, in world space.
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 z{0.0f, 0.0f, 1.0f};
    /// The length of an axis in metres, so that it is kGizmoScreenSize pixels on the screen.
    float size = 1.0f;

    [[nodiscard]] Vec3 axis(GizmoHandle handle) const;
};

/// The frame of the gizmo of an entity placed by `worldMatrix`, seen in `view` on a
/// viewport `viewportSize` pixels large; `pixelScale` is the scale of the interface. The
/// axes are the world's, or the entity's own with its scale and any shear taken out.
[[nodiscard]] GizmoFrame gizmoFrame(const Mat4& worldMatrix, GizmoSpace space, GizmoMode mode, const CameraView& view,
                                    Vec2 viewportSize, float pixelScale = 1.0f);

/// The handle of a gizmo under the pointer at `pixel`, or None: of those within
/// kGizmoPickPixels (times `pixelScale`), the nearest on the screen.
[[nodiscard]] GizmoHandle pickHandle(GizmoMode mode, const GizmoFrame& frame, const CameraView& view, Vec2 viewportSize,
                                     Vec2 pixel, float pixelScale = 1.0f);

/// A handle that is held, and what it was held at.
struct GizmoDrag {
    GizmoMode mode = GizmoMode::Translate;
    GizmoHandle handle = GizmoHandle::None;
    GizmoFrame frame;
    /// Where on the axis or the plane the pointer took hold, in world space.
    Vec3 grabbed{0.0f};
    /// Where the pointer was on the screen.
    Vec2 grabbedPixel{0.0f};
};

[[nodiscard]] GizmoDrag beginDrag(GizmoMode mode, GizmoHandle handle, const GizmoFrame& frame, const CameraView& view,
                                  Vec2 viewportSize, Vec2 pixel);

/// How far, in world space, a translation handle that is held has been moved by now: along
/// its axis - by how far the pointer moved along the axis as it is seen, so that the handle
/// stays under it - or across its plane. Nothing for an axis seen end on, or while the
/// pointer's ray runs along the plane, where where it is cannot be told.
[[nodiscard]] Vec3 dragTranslation(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel);

/// The world-space rotation a rotation handle that is held has made by now: about its axis,
/// by the angle the pointer swept around the centre of the gizmo on the screen.
[[nodiscard]] Quat dragRotation(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel);

/// What a scale handle that is held multiplies the scale by: along its axis, or along all
/// three for the centre, by how far the pointer moved along the axis on the screen, in
/// lengths of the axis. Never less than kGizmoMinimumScale.
[[nodiscard]] Vec3 dragScale(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel);

/// The least a drag scales by: a scale of nothing cannot be undone by scaling again.
inline constexpr float kGizmoMinimumScale = 0.01f;

/// Draws a gizmo, with the handle that is held or under the pointer in the color of what
/// is selected.
void drawGizmo(DebugDraw& lines, GizmoMode mode, const GizmoFrame& frame, GizmoHandle highlighted);

} // namespace cordite
