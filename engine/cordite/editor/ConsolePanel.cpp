#include "cordite/editor/ConsolePanel.h"

#include "cordite/core/StringUtils.h"
#include "cordite/editor/EditorFeedback.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <utility>

namespace cordite {
namespace {

/// Messages kept; the oldest are dropped beyond this.
constexpr size_t kMaxMessages = 4000;

constexpr std::array<LogLevel, 5> kFilterLevels = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                                                   LogLevel::Error};

ImVec4 colorOf(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            return {0.55f, 0.57f, 0.62f, 1.0f};
        case LogLevel::Warn:
            return {0.95f, 0.78f, 0.30f, 1.0f};
        case LogLevel::Error:
        case LogLevel::Critical:
            return {0.95f, 0.38f, 0.33f, 1.0f};
        case LogLevel::Info:
        case LogLevel::Off:
            break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

} // namespace

ConsolePanel::ConsolePanel() : m_inbox(std::make_shared<Inbox>()), m_messages(Log::history()) {
    // The listener shares the inbox instead of pointing at the panel: a message that is
    // being delivered on another thread while the panel is destroyed then still has
    // somewhere valid to go.
    m_listener = Log::addListener([inbox = m_inbox](const LogMessage& message) {
        const std::lock_guard lock(inbox->mutex);
        inbox->messages.push_back(message);
    });
}

ConsolePanel::~ConsolePanel() {
    Log::removeListener(m_listener);
}

void ConsolePanel::collectIncoming() {
    std::vector<LogMessage> incoming;
    {
        const std::lock_guard lock(m_inbox->mutex);
        incoming.swap(m_inbox->messages);
    }
    if (incoming.empty()) {
        return;
    }
    m_failedAction = m_failedAction || std::ranges::any_of(incoming, reportsFailedAction);

    m_messages.insert(m_messages.end(), std::make_move_iterator(incoming.begin()),
                      std::make_move_iterator(incoming.end()));
    if (m_messages.size() > kMaxMessages) {
        m_messages.erase(m_messages.begin(),
                         m_messages.begin() + static_cast<std::ptrdiff_t>(m_messages.size() - kMaxMessages));
    }
    m_scrollToNewest = m_followNewest;
}

bool ConsolePanel::takeFailedAction() {
    collectIncoming();
    return std::exchange(m_failedAction, false);
}

void ConsolePanel::draw(std::vector<std::string>& submittedLines) {
    collectIncoming();

    if (ImGui::Begin(kWindowName)) {
        if (ImGui::Button("Clear")) {
            m_messages.clear();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::BeginCombo("##level", std::string(nameOf(m_minimumLevel)).c_str())) {
            for (const LogLevel level : kFilterLevels) {
                if (ImGui::Selectable(std::string(nameOf(level)).c_str(), level == m_minimumLevel)) {
                    m_minimumLevel = level;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The least severe kind of message to show.");
        ImGui::SameLine();
        ImGui::Checkbox("Follow", &m_followNewest);
        ImGui::SetItemTooltip("Keep the newest message in view.");
        ImGui::Separator();

        // Everything above the input line scrolls.
        const float inputHeight = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("messages", ImVec2(0.0f, -inputHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const LogMessage& message : m_messages) {
                if (message.level < m_minimumLevel) {
                    continue;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, colorOf(message.level));
                ImGui::Text("[%s] %s", message.category.c_str(), message.text.c_str());
                ImGui::PopStyleColor();
            }
            if (m_scrollToNewest) {
                ImGui::SetScrollHereY(1.0f);
                m_scrollToNewest = false;
            }
        }
        ImGui::EndChild();

        ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
        if (ImGui::InputTextWithHint("##command", "command {\"parameter\": value}   -   try: editor.commands", &m_input,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (const std::string_view line = trim(m_input); !line.empty()) {
                submittedLines.emplace_back(line);
            }
            m_input.clear();
            // Enter takes the focus away; give it back for the next command.
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
}

} // namespace cordite
