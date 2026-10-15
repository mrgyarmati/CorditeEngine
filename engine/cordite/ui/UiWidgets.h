#pragma once

/// @file
/// Widgets of the interface beyond ImGui's own, drawn the same way wherever they are used.
///
/// @code
/// if (sectionHeader("Transform", nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
///     drawTransform();
/// }
/// overlayText(*ImGui::GetWindowDrawList(), corner, "6.0, 4.0, 8.0");
/// @endcode

#include <imgui.h>

#include <string_view>

namespace cordite {

/// Begins a section that can be collapsed, with a header in the strong typeface and the
/// shades of a field. ImGui's own CollapsingHeader is drawn in its header colors, which a
/// theme gives to the rows that are selected: a section would look chosen. Takes what
/// ImGui::CollapsingHeader takes - `visible`, when given, adds a button that sets it to
/// false - and returns whether the section is open.
bool sectionHeader(const char* label, bool* visible = nullptr, ImGuiTreeNodeFlags flags = 0);

/// Draws text over the picture of a scene, which can be of any color: light, on a dark
/// backdrop that lets the picture through, in every theme (kOverlayText,
/// kOverlayBackdrop). `position` is the top-left corner on the screen. Returns the size of
/// what was drawn, backdrop and all. Only during a frame.
ImVec2 overlayText(ImDrawList& drawList, ImVec2 position, std::string_view text);

} // namespace cordite
