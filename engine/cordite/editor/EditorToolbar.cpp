#include "cordite/editor/EditorToolbar.h"

#include "cordite/editor/ConsolePanel.h"
#include "cordite/ui/UiSystem.h"

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string_view>

namespace cordite {
namespace {

/// Between the parts of the summary at the right of the status bar.
constexpr std::string_view kSeparator = "   ·   ";

constexpr ImGuiWindowFlags kBarFlags =
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;

/// How wide a row of buttons is, as drawButtons() lays it out.
float rowWidth(std::span<const ToolbarButton> buttons) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = 0.0f;
    for (const ToolbarButton& button : buttons) {
        width += ImGui::CalcTextSize(button.icon.text()).x + style.FramePadding.x * 2.0f;
    }
    return buttons.empty() ? 0.0f : width + style.ItemSpacing.x * static_cast<float>(buttons.size() - 1);
}

void drawButton(const ToolbarButton& button, std::vector<std::string>& submittedLines) {
    const UiPalette& palette = UiSystem::palette();
    if (button.active) {
        ImGui::PushStyleColor(ImGuiCol_Button, palette.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.accentStrong);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.accentStrong);
        ImGui::PushStyleColor(ImGuiCol_Text, palette.onAccent);
    }
    ImGui::BeginDisabled(!button.enabled);
    if (ImGui::Button(button.icon.text())) {
        submittedLines.push_back(button.command);
    }
    ImGui::EndDisabled();
    if (button.active) {
        ImGui::PopStyleColor(4);
    }
    // Said also of a button that cannot be clicked now, which is when it is wondered about.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", button.tooltip.c_str());
    }
}

void drawButtons(std::span<const ToolbarButton> buttons, std::vector<std::string>& submittedLines) {
    for (size_t index = 0; index < buttons.size(); ++index) {
        if (index > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(index));
        drawButton(buttons[index], submittedLines);
        ImGui::PopID();
    }
}

/// The icon of a message of the log: an error, a warning, or anything else.
UiIcon messageIcon(LogLevel level) {
    if (level >= LogLevel::Error) {
        return icons::kError;
    }
    return level == LogLevel::Warn ? icons::kWarning : icons::kInfo;
}

/// A bar along an edge of the main viewport, a frame high and in the color of the menu.
bool beginBar(const char* name, ImGuiDir edge) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UiSystem::palette().base);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), edge, height, kBarFlags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return open;
}

} // namespace

std::vector<ToolbarButton> documentButtons(const EditorContext& context) {
    const UndoStack& history = context.history();
    const bool playing = context.isPlaying();
    std::vector<ToolbarButton> buttons;
    buttons.push_back({
        .icon = icons::kSave,
        .tooltip = context.scenePath().empty() ? "Save: the scene has no file yet - File > Save as" : "Save (Ctrl+S)",
        .command = "scene.save",
        .enabled = !context.scenePath().empty() && !playing,
    });
    buttons.push_back({
        .icon = icons::kUndo,
        .tooltip = history.canUndo() ? fmt::format("Undo {} (Ctrl+Z)", history.undoLabel()) : "Undo (Ctrl+Z)",
        .command = "editor.undo",
        .enabled = history.canUndo(),
    });
    buttons.push_back({
        .icon = icons::kRedo,
        .tooltip = history.canRedo() ? fmt::format("Redo {} (Ctrl+Y)", history.redoLabel()) : "Redo (Ctrl+Y)",
        .command = "editor.redo",
        .enabled = history.canRedo(),
    });
    return buttons;
}

std::vector<ToolbarButton> gizmoButtons(GizmoMode mode, GizmoSpace space) {
    const auto modeButton = [mode](GizmoMode button, UiIcon icon, std::string_view tooltip) {
        return ToolbarButton{
            .icon = icon,
            .tooltip = std::string(tooltip),
            .command = fmt::format(R"(viewport.gizmo {{"mode": "{}"}})", nameOf(button)),
            .active = mode == button,
        };
    };
    const bool world = space == GizmoSpace::World;
    return {
        modeButton(GizmoMode::Translate, icons::kMove, "Move (W)"),
        modeButton(GizmoMode::Rotate, icons::kRotate, "Rotate (E)"),
        modeButton(GizmoMode::Scale, icons::kScale, "Scale (R)"),
        ToolbarButton{
            .icon = world ? icons::kEnvironment : icons::kEntity,
            .tooltip = world ? "Along the axes of the world - click for the entity's own (X)"
                             : "Along the axes of the entity - click for the world's (X)",
            .command = fmt::format(R"(viewport.gizmo {{"space": "{}"}})",
                                   nameOf(world ? GizmoSpace::Local : GizmoSpace::World)),
        },
    };
}

std::vector<ToolbarButton> playButtons(PlayState state) {
    const bool stopped = state == PlayState::Stopped;
    const bool paused = state == PlayState::Paused;
    std::vector<ToolbarButton> buttons;
    buttons.push_back({
        .icon = icons::kPlay,
        .tooltip = stopped ? "Play (F5)" : "Stop (Shift+F5)",
        .command = stopped ? "play.start" : "play.stop",
        .active = !stopped,
    });
    buttons.push_back({
        .icon = icons::kPause,
        .tooltip = paused ? "Resume (F6)" : "Pause (F6)",
        .command = paused ? "play.resume" : "play.pause",
        .enabled = !stopped,
        .active = paused,
    });
    buttons.push_back({
        .icon = icons::kStepForward,
        .tooltip = "Step a frame (F10)",
        .command = "play.step",
        .enabled = paused,
    });
    return buttons;
}

void drawToolbar(std::span<const ToolbarButton> document, std::span<const ToolbarButton> tools,
                 std::span<const ToolbarButton> playing, std::vector<std::string>& submittedLines) {
    if (beginBar("##Toolbar", ImGuiDir_Up)) {
        drawButtons(document, submittedLines);
        // A gap between the groups, as wide as a button.
        ImGui::SameLine(0.0f, ImGui::GetFrameHeight());
        ImGui::PushID("tools");
        drawButtons(tools, submittedLines);
        ImGui::PopID();
        // Playing is in the middle of the bar, whatever is at its ends.
        const float middle = (ImGui::GetWindowWidth() - rowWidth(playing)) * 0.5f;
        ImGui::SameLine(std::max(middle, ImGui::GetCursorPosX()));
        ImGui::PushID("playing");
        drawButtons(playing, submittedLines);
        ImGui::PopID();
    }
    ImGui::End();
}

std::string statusSummary(const StatusBarState& state) {
    std::string summary;
    if (state.play != PlayState::Stopped) {
        summary = state.play == PlayState::Playing ? "Playing" : "Paused";
        summary += kSeparator;
    }
    summary += fmt::format("{} {}", state.entityCount, state.entityCount == 1 ? "entity" : "entities");
    if (state.selectionCount > 0) {
        summary += fmt::format("{}{} selected", kSeparator, state.selectionCount);
    }
    return summary;
}

bool drawStatusBar(const StatusBarState& state) {
    bool clicked = false;
    if (beginBar("##StatusBar", ImGuiDir_Down)) {
        const std::string summary = statusSummary(state);
        const float summaryWidth = ImGui::CalcTextSize(summary.c_str()).x;
        const float messageEnd = ImGui::GetWindowWidth() - summaryWidth - ImGui::GetStyle().WindowPadding.x -
                                 ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (state.message) {
            const UiPalette& palette = UiSystem::palette();
            const LogMessage& message = *state.message;
            const std::string line = fmt::format("{} {}", messageIcon(message.level).text(), message.text);
            // As much of the message as there is room for; the console has all of it.
            ImGui::PushStyleColor(ImGuiCol_Text, messageColor(message.level, palette));
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const ImVec2 size(std::max(messageEnd - ImGui::GetCursorPosX(), 1.0f), ImGui::GetFrameHeight());
            clicked = ImGui::InvisibleButton("##message", size);
            // On the line of the summary, which is aligned to the padding of a frame.
            const ImVec2 text(start.x, start.y + ImGui::GetStyle().FramePadding.y);
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), text, ImVec2(start.x + size.x, start.y + size.y),
                                      start.x + size.x, line.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s\nClick for the console.", message.text.c_str());
            }
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - summaryWidth - ImGui::GetStyle().WindowPadding.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", summary.c_str());
    }
    ImGui::End();
    return clicked;
}

} // namespace cordite
