#include "cordite/editor/AssetBrowserPanel.h"

#include "cordite/core/StringUtils.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorIcons.h"
#include "cordite/editor/PropertyEditor.h"
#include "cordite/ui/UiWidgets.h"

#include <fmt/format.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace cordite {
namespace {

constexpr float kTypeFilterWidth = 130.0f;
constexpr float kSearchWidth = 220.0f;
constexpr float kTypeColumnWidth = 90.0f;

/// Rows of assets to go to: a click chooses one in the browser. An id that is no asset is
/// said to be missing, and is nowhere to go.
void drawAssetLinks(const AssetDatabase& database, std::span<const Uuid> assets, std::optional<Uuid>& chosen) {
    if (!ImGui::BeginTable("links", 2, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    for (const Uuid& id : assets) {
        ImGui::PushID(id.toString().c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (const AssetInfo* asset = database.find(id)) {
            const std::string label = fmt::format("{}  {}", iconOf(asset->type).text(), asset->name);
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                chosen = id;
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", asset->reference().c_str());
        } else {
            ImGui::TextDisabled("%s  missing", icons::kWarning.text());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", id.toString().c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/// The menu of the row that was drawn last, for its asset. Returns whether it is open.
bool drawAssetMenu(const EditorContext& context, const AssetInfo& asset, std::vector<std::string>& submittedLines) {
    if (!ImGui::BeginPopupContextItem("asset_menu")) {
        return false;
    }
    if (canBePlaced(asset.type) && ImGui::MenuItem("Place in the scene")) {
        submittedLines.push_back(placeAssetCommand(context, asset.id));
    }
    if (asset.type == AssetType::Scene && ImGui::MenuItem("Open scene", nullptr, false, !context.isPlaying())) {
        submittedLines.push_back(openSceneCommand(asset));
    }
    if (ImGui::MenuItem("Copy id")) {
        ImGui::SetClipboardText(asset.id.toString().c_str());
    }
    if (ImGui::MenuItem("Copy path")) {
        ImGui::SetClipboardText(asset.reference().c_str());
    }
    ImGui::EndPopup();
    return true;
}

/// What a click did to a row.
struct RowClick {
    /// Its arrow - or a directory, anywhere: it is to be opened or closed.
    bool toggled = false;
    /// Its asset was clicked, or its menu opened: that is the asset chosen.
    bool chosen = false;
};

/// Draws one row: its name, as far in as the row is, and for an asset its type in the
/// second column, its menu and what a drag of it carries. A double click on what can be
/// placed asks for that. `scrollHere` scrolls the rows to this one.
RowClick drawRow(const EditorContext& context, const AssetBrowserRow& row, bool scrollHere,
                 std::vector<std::string>& submittedLines) {
    RowClick click;
    const bool isAsset = row.kind == AssetBrowserRow::Kind::Asset;
    // By what the row is, not by where: a menu that is open stays with its asset when rows
    // above it come and go.
    ImGui::PushID(row.path.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);

    // A row of a tree whose state is the browser's: opened and closed there, and drawn
    // here as it is. A directory opens wherever it is clicked; a file by its arrow, since
    // a click on it chooses it.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns;
    if (!row.hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    } else if (isAsset) {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow;
    }
    if (row.selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    // A file that is listed for what is inside it, and not for itself.
    const bool dimmed = isAsset && !row.matches;
    if (dimmed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
    const UiIcon icon = isAsset ? iconOf(row.type) : directoryIcon(row.open);
    ImGui::TreeNodeEx("##row", flags, "%s  %s", icon.text(), row.label.c_str());
    if (dimmed) {
        ImGui::PopStyleColor();
    }
    if (scrollHere) {
        ImGui::SetScrollHereY();
    }

    if (ImGui::IsItemToggledOpen()) {
        click.toggled = true;
    } else if (isAsset && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        click.chosen = true;
        if (canBePlaced(row.type) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            submittedLines.push_back(placeAssetCommand(context, row.asset));
        }
    }
    if (isAsset) {
        if (const AssetInfo* asset = context.assets().database().find(row.asset);
            asset != nullptr && drawAssetMenu(context, *asset, submittedLines)) {
            click.chosen = true;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(PropertyEditor::kAssetPayload, &row.asset, sizeof(Uuid));
            ImGui::TextUnformatted(row.path.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::TableNextColumn();
        const std::string type(displayNameOf(row.type));
        ImGui::TextDisabled("%s", type.c_str());
    }
    ImGui::PopID();
    return click;
}

} // namespace

void AssetBrowserPanel::draw(EditorContext& context, std::vector<std::string>& submittedLines) {
    // The rows scroll, in a region of their own; the window around them does not.
    static const std::string kName = windowName(icons::kFolder, "Assets");
    if (ImGui::Begin(kName.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const AssetDatabase& database = context.assets().database();
        drawToolbar(database, submittedLines);
        ImGui::Separator();

        if (!m_hasRows || m_rowsOfDatabase != database.revision() || m_rowsOfBrowser != m_browser.revision()) {
            m_rows = m_browser.rows(database);
            m_rowsOfDatabase = database.revision();
            m_rowsOfBrowser = m_browser.revision();
            m_hasRows = true;
        }

        // The rows, and below them - under a separator - a line for the asset that is
        // chosen, which stays where it is however many rows there are.
        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
        if (ImGui::BeginChild("rows", ImVec2(0.0f, -footerHeight))) {
            drawRows(context, submittedLines);
        }
        ImGui::EndChild();
        ImGui::Separator();
        drawSelection(context, submittedLines);
    }
    ImGui::End();
}

void AssetBrowserPanel::drawToolbar(const AssetDatabase& database, std::vector<std::string>& submittedLines) {
    AssetBrowserFilter filter = m_browser.filter();
    // A filter that a command has set shows in the field; what is being typed stays as
    // it is typed.
    if (trim(m_search) != filter.search) {
        m_search = filter.search;
    }

    ImGui::SetNextItemWidth(kTypeFilterWidth);
    const std::string current(filter.type ? displayNameOf(*filter.type) : std::string_view("All types"));
    if (ImGui::BeginCombo("##type", current.c_str())) {
        if (ImGui::Selectable("All types", !filter.type.has_value())) {
            filter.type.reset();
        }
        for (const AssetType type : kAssetTypes) {
            const std::string name(displayNameOf(type));
            if (ImGui::Selectable(name.c_str(), filter.type == type)) {
                filter.type = type;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(kSearchWidth);
    if (ImGui::InputTextWithHint("##search", "name or path", &m_search)) {
        filter.search = m_search;
    }
    m_browser.setFilter(std::move(filter));

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        submittedLines.emplace_back("asset.refresh");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads the asset directory again, after files were added, changed or removed.");
    }

    // What is wrong with the asset directory, where it will be seen.
    const std::span<const std::string> problems = database.problems();
    if (!problems.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu problem(s)", problems.size());
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            for (const std::string& problem : problems) {
                ImGui::TextUnformatted(problem.c_str());
            }
            ImGui::EndTooltip();
        }
    }
}

void AssetBrowserPanel::drawRows(const EditorContext& context, std::vector<std::string>& submittedLines) {
    if (m_rows.empty()) {
        ImGui::TextDisabled("%s", m_browser.filter().isActive() ? "No asset is of that type and has that text."
                                                                : "There are no assets.");
        return;
    }
    if (!ImGui::BeginTable("assets", 2, ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, kTypeColumnWidth);

    // An asset that was chosen from outside the panel is scrolled to, where it is listed.
    std::optional<size_t> reveal;
    if (m_browser.selection() != m_shownSelection) {
        m_shownSelection = m_browser.selection();
        const auto selected = std::ranges::find_if(m_rows, &AssetBrowserRow::selected);
        if (selected != m_rows.end()) {
            reveal = static_cast<size_t>(selected - m_rows.begin());
        }
    }

    // What a click asks for changes the rows, and is done once they have been drawn.
    std::optional<size_t> toggled;
    std::optional<Uuid> chosen;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_rows.size()));
    if (reveal) {
        // Drawn wherever it is, so that there is something to scroll to.
        clipper.IncludeItemByIndex(static_cast<int>(*reveal));
    }
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto position = static_cast<size_t>(index);
            const RowClick click = drawRow(context, m_rows[position], reveal == position, submittedLines);
            if (click.toggled) {
                toggled = position;
            }
            if (click.chosen) {
                chosen = m_rows[position].asset;
            }
        }
    }
    ImGui::EndTable();

    // While a filter leaves something out every row is open and stays so: a click that
    // closed one would show nothing now, and hide it once the filter is gone.
    if (toggled && !m_browser.filter().isActive()) {
        const AssetBrowserRow& row = m_rows[*toggled];
        m_browser.setOpen(row, !row.open);
    }
    if (chosen) {
        // Chosen where it is seen: nothing to scroll to.
        m_browser.select(*chosen);
        m_shownSelection = *chosen;
    }
}

void AssetBrowserPanel::drawSelection(const EditorContext& context, std::vector<std::string>& submittedLines) {
    const AssetInfo* asset = context.assets().database().find(m_browser.selection());
    ImGui::AlignTextToFramePadding();
    if (asset == nullptr) {
        ImGui::TextDisabled("No asset is chosen.");
        return;
    }
    const std::string type(displayNameOf(asset->type));
    ImGui::TextUnformatted(asset->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s   %s   %s", type.c_str(), asset->reference().c_str(), asset->id.toString().c_str());

    if (canBePlaced(asset->type)) {
        ImGui::SameLine();
        if (ImGui::Button("Place")) {
            submittedLines.push_back(placeAssetCommand(context, asset->id));
        }
    }
    if (asset->type == AssetType::Scene) {
        ImGui::SameLine();
        ImGui::BeginDisabled(context.isPlaying());
        if (ImGui::Button("Open")) {
            submittedLines.push_back(openSceneCommand(*asset));
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy id")) {
        ImGui::SetClipboardText(asset->id.toString().c_str());
    }
    drawUses(context, *asset, submittedLines);
}

void AssetBrowserPanel::drawUses(const EditorContext& context, const AssetInfo& asset,
                                 std::vector<std::string>& submittedLines) {
    const AssetDatabase& database = context.assets().database();
    const std::span<const Uuid> users = database.dependents(asset.id);

    // The counts change; what the buttons are known by does not.
    ImGui::SameLine();
    ImGui::BeginDisabled(asset.dependencies.empty());
    if (ImGui::Button(fmt::format("Uses {}###uses", asset.dependencies.size()).c_str())) {
        ImGui::OpenPopup("uses");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("The assets it needs.");
    }
    ImGui::SameLine();
    if (ImGui::Button(fmt::format("Used by {}###usedBy", users.size()).c_str())) {
        ImGui::OpenPopup("usedBy");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The assets that need it, as their files were last read, and what in the open scene "
                          "refers to it.");
    }

    std::optional<Uuid> chosen;
    if (ImGui::BeginPopup("uses")) {
        drawAssetLinks(database, asset.dependencies, chosen);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("usedBy")) {
        if (ImGui::IsWindowAppearing()) {
            m_sceneUsers.clear();
            for (const Uuid& id : entitiesUsing(context, asset.id)) {
                m_sceneUsers.emplace_back(id, context.entityPath(context.world().findById(id)));
            }
        }
        if (users.empty()) {
            ImGui::TextDisabled("No asset needs it.");
        } else {
            drawAssetLinks(database, users, chosen);
        }
        ImGui::Separator();
        drawSceneUsers(submittedLines);
        ImGui::EndPopup();
    }
    if (chosen) {
        m_browser.select(*chosen);
        m_browser.reveal(database, *chosen);
    }
}

void AssetBrowserPanel::drawSceneUsers(std::vector<std::string>& submittedLines) const {
    if (m_sceneUsers.empty()) {
        ImGui::TextDisabled("Nothing in the open scene refers to it.");
        return;
    }
    ImGui::TextDisabled("In the open scene");
    for (const auto& [id, path] : m_sceneUsers) {
        ImGui::PushID(id.toString().c_str());
        if (ImGui::Selectable(fmt::format("{}  {}", icons::kEntity.text(), path).c_str())) {
            const std::array<Uuid, 1> one = {id};
            submittedLines.push_back(selectEntitiesCommand(one));
        }
        ImGui::PopID();
    }
    if (m_sceneUsers.size() > 1 && ImGui::Selectable(fmt::format("Select all {}", m_sceneUsers.size()).c_str())) {
        std::vector<Uuid> all;
        all.reserve(m_sceneUsers.size());
        for (const auto& user : m_sceneUsers) {
            all.push_back(user.first);
        }
        submittedLines.push_back(selectEntitiesCommand(all));
    }
}

} // namespace cordite
