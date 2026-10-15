#include "cordite/ui/UiWidgets.h"

#include "cordite/ui/UiSystem.h"
#include "cordite/ui/UiTheme.h"

namespace cordite {

bool sectionHeader(const char* label, bool* visible, ImGuiTreeNodeFlags flags) {
    const UiPalette& palette = UiSystem::palette();
    ImGui::PushStyleColor(ImGuiCol_Header, palette.field);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, palette.fieldHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, palette.fieldActive);
    ImGui::PushFont(UiSystem::font(UiFont::Strong), 0.0f);
    const bool open = ImGui::CollapsingHeader(label, visible, flags);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    return open;
}

ImVec2 overlayText(ImDrawList& drawList, ImVec2 position, std::string_view text) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 padding(style.FramePadding.x * 0.75f, style.FramePadding.y * 0.5f);
    const char* begin = text.data();
    const char* end = begin + text.size();
    const ImVec2 textSize = ImGui::CalcTextSize(begin, end);
    const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
    drawList.AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y),
                           ImGui::GetColorU32(kOverlayBackdrop), style.FrameRounding);
    drawList.AddText(ImVec2(position.x + padding.x, position.y + padding.y), ImGui::GetColorU32(kOverlayText), begin,
                     end);
    return size;
}

} // namespace cordite
