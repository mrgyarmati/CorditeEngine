#include "cordite/editor/AssetBrowserPanel.h"

#include "cordite/assets/Sound.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/AssetFiles.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorIcons.h"
#include "cordite/editor/PropertyEditor.h"
#include "cordite/ui/UiSystem.h"
#include "cordite/ui/UiWidgets.h"

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

namespace cordite {
namespace {

constexpr float kTypeFilterWidth = 130.0f;
constexpr float kSearchWidth = 180.0f;
constexpr float kTypeColumnWidth = 90.0f;
constexpr float kMoveFieldWidth = 420.0f;
/// What a dragged directory of the assets carries: its path.
constexpr const char* kDirectoryPayload = "cordite.assetDirectory";
/// The most of the width of the panel that the picture beside the rows takes, and the
/// least it is drawn at, in units of the interface.
constexpr float kPictureShare = 0.4f;
constexpr float kSmallestPicture = 32.0f;
/// How wide the tree of folders is until it is dragged wider or narrower - which the
/// layout then remembers - in units of the interface.
constexpr float kFoldersWidth = 170.0f;
/// How wide a tile of the grid is, and how large the icon on it, in units of the
/// interface.
constexpr float kTileWidth = 84.0f;
constexpr float kTileIconSize = 34.0f;
/// In how many frames the contents scroll to an asset chosen from outside: the first may
/// lay out a pane that has not been drawn before.
constexpr uint32_t kRevealFrames = 2;

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

/// The menu of the row that was drawn last, for its asset. Returns whether it is open;
/// `moveAsked` says that its file is to be renamed or moved, `deleteAsked` deleted.
bool drawAssetMenu(const EditorContext& context, const AssetInfo& asset, std::vector<std::string>& submittedLines,
                   bool& moveAsked, bool& deleteAsked) {
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
    // What is built in, or inside a file, has no file of its own.
    if (!asset.isBuiltin() && asset.subPath.empty()) {
        ImGui::Separator();
        if (ImGui::MenuItem("Rename or move...")) {
            moveAsked = true;
        }
        if (ImGui::MenuItem("Delete...", nullptr, false, !context.isPlaying())) {
            deleteAsked = true;
        }
    }
    ImGui::EndPopup();
    return true;
}

/// Takes an asset that is dragged onto the directory of `row`, and asks for its file to
/// move there. What could not go there is not taken.
void acceptMoveDrop(const AssetDatabase& database, const AssetBrowserRow& row,
                    std::vector<std::string>& submittedLines) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
    if (dragged != nullptr && dragged->IsDataType(PropertyEditor::kAssetPayload) &&
        dragged->DataSize == static_cast<int>(sizeof(Uuid))) {
        Uuid id;
        std::memcpy(&id, dragged->Data, sizeof(Uuid));
        const AssetInfo* asset = database.find(id);
        const std::string to = asset != nullptr ? dropTarget(*asset, row) : std::string();
        if (!to.empty() && ImGui::AcceptDragDropPayload(PropertyEditor::kAssetPayload) != nullptr) {
            submittedLines.push_back(moveAssetCommand(id, to));
        }
    } else if (dragged != nullptr && dragged->IsDataType(kDirectoryPayload) && dragged->DataSize > 0) {
        const std::string directory(static_cast<const char*>(dragged->Data), static_cast<size_t>(dragged->DataSize));
        const std::string to = directoryDropTarget(directory, row);
        if (!to.empty() && ImGui::AcceptDragDropPayload(kDirectoryPayload) != nullptr) {
            submittedLines.push_back(moveDirectoryCommand(directory, to));
        }
    }
    ImGui::EndDragDropTarget();
}

/// What a row of the contents says it is where its type would be.
std::string kindOf(const AssetBrowserRow& row) {
    return row.kind == AssetBrowserRow::Kind::Asset ? std::string(displayNameOf(row.type)) : std::string("Folder");
}

/// Where an asset that a search found is: the directory of its file - the top's name for
/// the top - or "builtin" for what the engine has built in, which is in no file.
std::string_view whereOf(const AssetBrowserRow& row) {
    const std::string_view path = row.path;
    const std::string_view file = path.substr(0, path.find('#'));
    if (file == kBuiltinAssetPath) {
        return kBuiltinAssetPath;
    }
    const size_t slash = file.find_last_of('/');
    return slash == std::string_view::npos ? kTopFolderLabel : file.substr(0, slash);
}

/// What a row is known by among the others, for ImGui: the key of a directory - which
/// tells what the engine has built in from a directory of the same name - or the path
/// of an asset. It stays with the row when rows before it come and go, and with it its
/// menu.
const char* idOf(const AssetBrowserRow& row) {
    return row.kind == AssetBrowserRow::Kind::Directory ? row.key.c_str() : row.path.c_str();
}

/// The face of the tile of `row`, whose top left corner is `start`: the icon of what it
/// is, large, and its name under it - cut short with an ellipsis where it is longer than
/// the tile; the tooltip has all of it.
void drawTileFace(const AssetBrowserRow& row, ImVec2 start) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const UiIcon icon = row.kind == AssetBrowserRow::Kind::Asset ? iconOf(row.type) : directoryIcon(false);
    const ImVec2 iconExtent = font->CalcTextSizeA(kTileIconSize, FLT_MAX, 0.0f, icon.text());
    drawList->AddText(font, kTileIconSize,
                      ImVec2(std::floor(start.x + (kTileWidth - iconExtent.x) * 0.5f), start.y + style.FramePadding.y),
                      ImGui::GetColorU32(ImGuiCol_Text), icon.text());

    const float labelTop = start.y + style.FramePadding.y + kTileIconSize + style.ItemInnerSpacing.y;
    const ImVec2 labelExtent = ImGui::CalcTextSize(row.label.c_str());
    const float labelEnd = start.x + kTileWidth - style.FramePadding.x;
    const float labelLeft =
        std::max(std::floor(start.x + (kTileWidth - labelExtent.x) * 0.5f), start.x + style.FramePadding.x);
    ImGui::RenderTextEllipsis(drawList, ImVec2(labelLeft, labelTop),
                              ImVec2(labelEnd, labelTop + ImGui::GetTextLineHeight()), labelEnd, row.label.c_str(),
                              nullptr, &labelExtent);
}

/// The arrow at the side of the tile of a file with something inside it, whose top left
/// corner is `start`: it shows that, after the file, or hides it again. Returns whether it
/// was clicked.
bool drawOpenArrow(const AssetBrowserRow& row, ImVec2 start) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const UiIcon arrow = row.open ? icons::kChevronLeft : icons::kChevronRight;
    const ImVec2 glyph = ImGui::CalcTextSize(arrow.text());
    const ImVec2 size(glyph.x + style.FramePadding.x, ImGui::GetFrameHeight());
    const ImVec2 corner(start.x + kTileWidth - size.x,
                        start.y + style.FramePadding.y + (kTileIconSize - size.y) * 0.5f);
    ImGui::SetCursorScreenPos(corner);
    const bool clicked = ImGui::InvisibleButton("open", size);
    // Without a ground of its own until the pointer is on it: the tile has one.
    if (ImGui::IsItemHovered()) {
        drawList->AddRectFilled(corner, ImVec2(corner.x + size.x, corner.y + size.y),
                                ImGui::GetColorU32(ImGuiCol_ButtonHovered), style.FrameRounding);
    }
    drawList->AddText(
        ImVec2(std::floor(corner.x + (size.x - glyph.x) * 0.5f), std::floor(corner.y + (size.y - glyph.y) * 0.5f)),
        ImGui::GetColorU32(ImGuiCol_Text), arrow.text());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", row.open ? "Hide what is inside" : "Show what is inside");
    }
    return clicked;
}

} // namespace

void AssetBrowserPanel::draw(Engine& engine, EditorContext& context, ShadowQuality shadows,
                             std::vector<std::string>& submittedLines) {
    // The panes scroll, each in a region of its own; the window around them does not.
    static const std::string kName = windowName(icons::kFolder, "Assets");
    if (ImGui::Begin(kName.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const AssetDatabase& database = context.assets().database();
        const auto refresh = [&] {
            if (!m_hasRows || m_rowsOfDatabase != database.revision() || m_rowsOfBrowser != m_browser.revision()) {
                m_folders = m_browser.folders(database);
                m_contents = m_browser.contents(database);
                m_shownFolder = m_browser.shownFolder(database);
                m_steps = folderSteps(m_shownFolder);
                m_rowsOfDatabase = database.revision();
                m_rowsOfBrowser = m_browser.revision();
                m_hasRows = true;
            }
        };
        refresh();
        // What a click asks for changes the rows, and is done once they have all been drawn.
        Click click;
        drawToolbar(database, submittedLines, click);
        // The filter of the toolbar shows at once.
        refresh();

        // The panes, and below them - under a separator - a line for the asset that is
        // chosen, which stays where it is however many rows there are.
        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
        // At the right, a picture of the chosen asset, when it has one: square, as high as
        // the panes are, and no wider than a share of the panel. A sound has what is said of
        // it there, and the button that plays it, which need no square.
        const AssetInfo* chosen = database.find(m_browser.selection());
        const ImVec2 room = ImGui::GetContentRegionAvail();
        float pictureWidth = 0.0f;
        if (chosen != nullptr && AssetPreview::hasPicture(chosen->type)) {
            pictureWidth = std::floor(std::min(room.y - footerHeight, room.x * kPictureShare));
        } else if (chosen != nullptr && chosen->type == AssetType::Sound) {
            pictureWidth = std::floor(room.x * kPictureShare);
        }
        const bool pictured = pictureWidth >= kSmallestPicture;
        const float besidePanes = pictured ? pictureWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f;

        // The tree is as wide as it was dragged to be, which the layout remembers.
        if (ImGui::BeginChild("folders", ImVec2(kFoldersWidth, -footerHeight), ImGuiChildFlags_ResizeX)) {
            drawFolders(context, submittedLines, click);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("contents", ImVec2(-besidePanes, -footerHeight))) {
            drawContents(context, submittedLines, click);
            drawContentsMenu(context);
        }
        ImGui::EndChild();
        if (pictured) {
            ImGui::SameLine();
            // It does not scroll: a scroll bar that came and went would change its size, and ask
            // for a picture of another.
            if (ImGui::BeginChild("picture", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                if (chosen->type == AssetType::Sound) {
                    drawSound(engine, context, *chosen, submittedLines);
                } else {
                    drawPicture(engine, context, shadows, *chosen);
                }
            }
            ImGui::EndChild();
        }
        ImGui::Separator();
        drawSelection(context, submittedLines);
        drawMoveDialog(context, submittedLines);
        drawDeleteDialog(context, submittedLines);
        drawCreateDialog(submittedLines);
        apply(click);
    }
    ImGui::End();
}

void AssetBrowserPanel::apply(const Click& click) {
    if (click.toggled) {
        m_browser.setOpen(*click.toggled, !click.toggled->open);
    }
    if (click.shown) {
        m_browser.showFolder(*click.shown);
    }
    if (click.chosen) {
        // Chosen where it is seen: nothing to scroll to.
        m_browser.select(*click.chosen);
        m_shownSelection = *click.chosen;
        m_revealFrames = 0;
    }
}

std::optional<size_t> AssetBrowserPanel::rowToReveal() {
    if (m_browser.selection() != m_shownSelection) {
        m_shownSelection = m_browser.selection();
        m_revealFrames = kRevealFrames;
    }
    if (m_revealFrames == 0) {
        return std::nullopt;
    }
    --m_revealFrames;
    const auto selected = std::ranges::find_if(m_contents, &AssetBrowserRow::selected);
    if (selected == m_contents.end()) {
        return std::nullopt;
    }
    return static_cast<size_t>(selected - m_contents.begin());
}

void AssetBrowserPanel::drawSound(Engine& engine, EditorContext& context, const AssetInfo& asset,
                                  std::vector<std::string>& submittedLines) {
    // Read once, not in every frame: a file that cannot be read is tried again when the
    // assets are.
    const uint64_t revision = context.assets().database().revision();
    if (m_soundRead != asset.id || m_soundRevision != revision) {
        const Result<std::shared_ptr<const SoundData>> sound = context.assets().loadSound(asset.id);
        m_soundRead = asset.id;
        m_soundRevision = revision;
        m_soundHeard = sound.has_value();
        m_soundText = sound ? describeSound(**sound) : fmt::format("It cannot be heard: {}", sound.error().message);
    }
    ImGui::PushTextWrapPos(0.0f);
    if (m_soundHeard) {
        ImGui::TextUnformatted(m_soundText.c_str());
    } else {
        ImGui::TextDisabled("%s", m_soundText.c_str());
    }
    ImGui::PopTextWrapPos();
    if (!m_soundHeard) {
        return;
    }
    // Heard by whoever is at the editor, also while the scene is not played. Play on a
    // sound while another is heard puts this one in its place.
    if (engine.audio().previewing() == asset.id) {
        if (ImGui::Button(fmt::format("{}  Stop", icons::kStop.text()).c_str())) {
            submittedLines.push_back(listenCommand(std::nullopt));
        }
    } else if (ImGui::Button(fmt::format("{}  Play", icons::kPlay.text()).c_str())) {
        submittedLines.push_back(listenCommand(asset.id));
    }
}

void AssetBrowserPanel::drawDeleteDialog(const EditorContext& context, std::vector<std::string>& submittedLines) {
    if (m_openDeleteDialog) {
        ImGui::OpenPopup("Delete asset");
        m_openDeleteDialog = false;
    }
    if (!ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const AssetDatabase& database = context.assets().database();
    const AssetInfo* asset = database.find(m_deleting);
    const Result<AssetUsers> users = findAssetUsers(context, m_deleting);
    if (asset == nullptr || !users) {
        // Gone while the question was open: the assets were read again.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted(fmt::format("Delete {} and its .meta? This cannot be undone.", asset->path).c_str());
    if (!users->empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("It is used by:");
        for (const Uuid& user : users->assets) {
            const AssetInfo* found = database.find(user);
            ImGui::BulletText("%s", found != nullptr ? found->reference().c_str() : user.toString().c_str());
        }
        if (users->sceneReferences > 0) {
            ImGui::BulletText("%zu propert%s of the open scene", users->sceneReferences,
                              users->sceneReferences == 1 ? "y" : "ies");
        }
        ImGui::TextDisabled("Deleted anyway, those refer to an asset that is not there.");
    }
    ImGui::Spacing();
    if (ImGui::Button(users->empty() ? "Delete" : "Delete anyway")) {
        submittedLines.push_back(deleteAssetCommand(m_deleting, !users->empty()));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::drawContentsMenu(const EditorContext& context) {
    // Only a directory of the project takes something new: not what the engine has built in.
    const std::string& folder = m_browser.folder();
    if (context.assets().database().rootDirectory().empty() || folder == kBuiltinFolder) {
        return;
    }
    if (!ImGui::BeginPopupContextWindow("contents_menu",
                                        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        return;
    }
    if (ImGui::MenuItem("New folder...")) {
        m_creating = Creating::Folder;
        m_createTarget = freeAssetPath(context, folder, "New folder", "");
        m_openCreateDialog = true;
    }
    if (ImGui::MenuItem("New material...")) {
        m_creating = Creating::Material;
        m_createTarget = freeAssetPath(context, folder, "New material", ".material.json");
        m_openCreateDialog = true;
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::drawCreateDialog(std::vector<std::string>& submittedLines) {
    if (m_openCreateDialog) {
        ImGui::OpenPopup("New asset");
        m_openCreateDialog = false;
    }
    if (!ImGui::BeginPopupModal("New asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const bool folder = m_creating == Creating::Folder;
    ImGui::TextUnformatted(folder ? "Where the new folder goes, inside the asset directory:"
                                  : "Where the new material goes, inside the asset directory:");
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(kMoveFieldWidth);
    const bool entered = ImGui::InputText("##path", &m_createTarget, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create") || entered) {
        const std::string path(trim(m_createTarget));
        submittedLines.push_back(folder ? createDirectoryCommand(path) : createMaterialCommand(path));
        m_creating = Creating::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_creating = Creating::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::drawPicture(Engine& engine, EditorContext& context, ShadowQuality shadows,
                                    const AssetInfo& asset) {
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const float side = std::floor(std::min(room.x, room.y));
    // Drawn in the pixels of the display, of which a unit of the interface may be more than one.
    const float pixels =
        std::clamp(side * ImGui::GetIO().DisplayFramebufferScale.y, static_cast<float>(AssetPreview::kMinSize),
                   static_cast<float>(AssetPreview::kMaxSize));
    if (const Status shown =
            m_preview.show(engine.sceneRenderer(), engine.graphics().device(), engine.commandList(), context.assets(),
                           context.components(), asset.id, static_cast<uint32_t>(pixels), shadows);
        !shown) {
        // Asked once: show() answers as it did until something changes.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No picture: %s", shown.error().message.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    nvrhi::ITexture* picture = m_preview.picture();
    ImVec2 extent(side, side);
    if (!AssetPreview::isDrawn(asset.type)) {
        // A texture keeps its proportions.
        const nvrhi::TextureDesc& desc = picture->getDesc();
        const float aspect = static_cast<float>(desc.width) / static_cast<float>(std::max(desc.height, 1u));
        extent = aspect >= 1.0f ? ImVec2(side, std::floor(side / aspect)) : ImVec2(std::floor(side * aspect), side);
    }
    ImGui::Image(UiSystem::textureId(picture), extent);
}

void AssetBrowserPanel::drawToolbar(const AssetDatabase& database, std::vector<std::string>& submittedLines,
                                    Click& click) {
    const ImGuiStyle& style = ImGui::GetStyle();
    AssetBrowserFilter filter = m_browser.filter();
    // A filter that a command has set shows in the field; what is being typed stays as
    // it is typed.
    if (trim(m_search) != filter.search) {
        m_search = filter.search;
    }

    // At the right the filter, the views, Refresh and what is wrong; the way to the folder
    // shown has what is left at the left.
    const std::span<const std::string> problems = database.problems();
    const std::string problemCount = fmt::format("{} problem(s)", problems.size());
    const auto iconButtonWidth = [&style](const UiIcon& icon) {
        return ImGui::CalcTextSize(icon.text()).x + style.FramePadding.x * 2.0f;
    };
    float rightWidth = kTypeFilterWidth + kSearchWidth + iconButtonWidth(icons::kList) + iconButtonWidth(icons::kGrid) +
                       iconButtonWidth(icons::kRefresh) + style.ItemSpacing.x * 4.0f;
    if (!problems.empty()) {
        rightWidth += ImGui::CalcTextSize(problemCount.c_str()).x + style.ItemSpacing.x;
    }
    const float left = ImGui::GetCursorPosX();
    const float room = ImGui::GetContentRegionAvail().x;
    drawSteps(std::max(room - rightWidth - style.ItemSpacing.x, 0.0f), click);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(left + room - rightWidth, ImGui::GetCursorPosX()));

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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("Finds what is in the folder shown, and below it.");
    }
    m_browser.setFilter(std::move(filter));

    // The view in use is drawn in the accent, as the tools of the toolbar are.
    const UiPalette& palette = UiSystem::palette();
    for (const AssetBrowserView view : kAssetBrowserViews) {
        const bool inUse = m_browser.view() == view;
        ImGui::SameLine();
        if (inUse) {
            ImGui::PushStyleColor(ImGuiCol_Button, palette.accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.accentStrong);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.accentStrong);
            ImGui::PushStyleColor(ImGuiCol_Text, palette.onAccent);
        }
        const UiIcon icon = view == AssetBrowserView::List ? icons::kList : icons::kGrid;
        if (ImGui::Button(fmt::format("{}##{}", icon.text(), nameOf(view)).c_str())) {
            m_browser.setView(view);
        }
        if (inUse) {
            ImGui::PopStyleColor(4);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", view == AssetBrowserView::List ? "A list" : "A grid of tiles");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(icons::kRefresh.text())) {
        submittedLines.emplace_back("asset.refresh");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("Reads the asset directory again, after files were added, changed or removed.");
    }

    // What is wrong with the asset directory, where it will be seen.
    if (!problems.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", problemCount.c_str());
        if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
            for (const std::string& problem : problems) {
                ImGui::TextUnformatted(problem.c_str());
            }
            ImGui::EndTooltip();
        }
    }
}

void AssetBrowserPanel::drawSteps(float width, Click& click) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // How wide each step is, and what goes between two: a chevron with room around it.
    std::vector<float> widths;
    widths.reserve(m_steps.size());
    for (const AssetFolderStep& step : m_steps) {
        widths.push_back(ImGui::CalcTextSize(step.label.c_str()).x + style.FramePadding.x * 2.0f);
    }
    const float between = ImGui::CalcTextSize(icons::kChevronRight.text()).x + style.ItemSpacing.x * 2.0f;
    const float leftOut = ImGui::CalcTextSize("...").x + style.ItemSpacing.x;
    // The first steps are left out while the way is longer than the room - never the last,
    // which is the folder shown.
    size_t first = 0;
    const auto widthFrom = [&](size_t from) {
        float total = from > 0 ? leftOut : 0.0f;
        for (size_t index = from; index < widths.size(); ++index) {
            total += widths[index] + (index > from ? between : 0.0f);
        }
        return total;
    };
    while (first + 1 < m_steps.size() && widthFrom(first) > width) {
        ++first;
    }

    // Flat, as a path is - in every theme, though the light one frames its buttons: a step
    // is a button only where the pointer is.
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::AlignTextToFramePadding();
    if (first > 0) {
        ImGui::TextDisabled("...");
        ImGui::SameLine();
    }
    for (size_t index = first; index < m_steps.size(); ++index) {
        const AssetFolderStep& step = m_steps[index];
        if (index > first) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", icons::kChevronRight.text());
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(index));
        if (index + 1 == m_steps.size()) {
            // The folder shown: there is nowhere to go by it.
            ImGui::TextUnformatted(step.label.c_str());
        } else if (ImGui::Button(step.label.c_str())) {
            click.shown = step.key;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void AssetBrowserPanel::drawFolders(const EditorContext& context, std::vector<std::string>& submittedLines,
                                    Click& click) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_folders.size()));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const AssetBrowserRow& row = m_folders[static_cast<size_t>(index)];
            ImGui::PushID(idOf(row));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
            // A row of a tree whose state is the browser's: opened and closed there, and
            // drawn here as it is. A click on the name shows the folder; its arrow, or a
            // double click, opens it.
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!row.hasChildren) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (row.selected) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
            ImGui::TreeNodeEx("##folder", flags, "%s  %s", directoryIcon(row.open).text(), row.label.c_str());
            if (ImGui::IsItemToggledOpen()) {
                click.toggled = row;
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                click.shown = row.key;
            }
            handleRow(context, row, false, submittedLines, click);
            ImGui::PopID();
        }
    }
}

void AssetBrowserPanel::drawContents(const EditorContext& context, std::vector<std::string>& submittedLines,
                                     Click& click) {
    if (m_contents.empty()) {
        const std::string& folder = m_steps.back().label;
        const std::string said =
            m_browser.filter().isActive()
                ? fmt::format("Nothing in {} or below it is of that type and has that text.", folder)
                : fmt::format("There is nothing in {}.", folder);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", said.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    if (m_browser.view() == AssetBrowserView::List) {
        drawList(context, submittedLines, click);
    } else {
        drawGrid(context, submittedLines, click);
    }
}

void AssetBrowserPanel::handleRow(const EditorContext& context, const AssetBrowserRow& row, bool doubleClicked,
                                  std::vector<std::string>& submittedLines, Click& click) {
    const AssetDatabase& database = context.assets().database();
    if (row.kind == AssetBrowserRow::Kind::Asset) {
        if (doubleClicked && canBePlaced(row.type)) {
            submittedLines.push_back(placeAssetCommand(context, row.asset));
        }
        bool moveAsked = false;
        bool deleteAsked = false;
        if (const AssetInfo* asset = database.find(row.asset);
            asset != nullptr && drawAssetMenu(context, *asset, submittedLines, moveAsked, deleteAsked)) {
            click.chosen = row.asset;
        }
        if (moveAsked) {
            m_moving = row.asset;
            m_movingDirectory.clear();
            m_moveTarget = row.path;
            m_openMoveDialog = true;
        }
        if (deleteAsked) {
            m_deleting = row.asset;
            m_openDeleteDialog = true;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(PropertyEditor::kAssetPayload, &row.asset, sizeof(Uuid));
            ImGui::TextUnformatted(row.path.c_str());
            ImGui::EndDragDropSource();
        }
        return;
    }
    if (doubleClicked) {
        click.shown = row.key;
    }
    // The top is the asset directory itself, and what the engine has built in no directory:
    // neither is renamed or carried about.
    if (isMovableDirectory(row)) {
        if (ImGui::BeginPopupContextItem("directory_menu")) {
            if (ImGui::MenuItem("Rename or move...")) {
                m_moving = Uuid();
                m_movingDirectory = row.path;
                m_moveTarget = row.path;
                m_openMoveDialog = true;
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kDirectoryPayload, row.path.data(), row.path.size());
            ImGui::TextUnformatted(row.path.c_str());
            ImGui::EndDragDropSource();
        }
    }
    acceptMoveDrop(database, row, submittedLines);
}

void AssetBrowserPanel::drawList(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click) {
    // What a search found is from anywhere below the folder: where each is, is said too, in
    // a table of its own - of three columns, not two.
    const bool searching = m_browser.filter().isActive();
    if (!ImGui::BeginTable(searching ? "found" : "list", searching ? 3 : 2, ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, kTypeColumnWidth);
    if (searching) {
        ImGui::TableSetupColumn("where", ImGuiTableColumnFlags_WidthStretch);
    }

    // An asset that was chosen from outside the panel is scrolled to, where it is listed.
    const std::optional<size_t> reveal = rowToReveal();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_contents.size()));
    if (reveal) {
        // Drawn wherever it is, so that there is something to scroll to.
        clipper.IncludeItemByIndex(static_cast<int>(*reveal));
    }
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto position = static_cast<size_t>(index);
            const AssetBrowserRow& row = m_contents[position];
            const bool isAsset = row.kind == AssetBrowserRow::Kind::Asset;
            ImGui::PushID(idOf(row));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
            // A file with something inside it opens by its arrow, since a click on it
            // chooses it; a directory is gone into by a double click.
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAllColumns;
            flags |= row.hasChildren ? ImGuiTreeNodeFlags_OpenOnArrow : ImGuiTreeNodeFlags_Leaf;
            if (row.selected) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
            const UiIcon icon = isAsset ? iconOf(row.type) : directoryIcon(false);
            ImGui::TreeNodeEx("##row", flags, "%s  %s", icon.text(), row.label.c_str());
            if (reveal == position) {
                ImGui::SetScrollHereY();
            }
            bool doubleClicked = false;
            if (ImGui::IsItemToggledOpen()) {
                click.toggled = row;
            } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                if (isAsset) {
                    click.chosen = row.asset;
                }
                doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            }
            handleRow(context, row, doubleClicked, submittedLines, click);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kindOf(row).c_str());
            if (searching) {
                ImGui::TableNextColumn();
                const std::string where(whereOf(row));
                ImGui::TextDisabled("%s", where.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void AssetBrowserPanel::drawGrid(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float tileHeight = style.FramePadding.y * 2.0f + kTileIconSize + style.ItemInnerSpacing.y + lineHeight;
    const ImVec2 pitch(kTileWidth + style.ItemSpacing.x, tileHeight + style.ItemSpacing.y);
    // As many tiles side by side as there is room for, and at least one.
    const int columns =
        std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / pitch.x));
    const int count = static_cast<int>(m_contents.size());
    const int lines = (count + columns - 1) / columns;

    // An asset that was chosen from outside the panel is scrolled to, where it is listed:
    // the line it is on.
    std::optional<int> reveal;
    if (const std::optional<size_t> row = rowToReveal()) {
        reveal = static_cast<int>(*row) / columns;
    }

    ImGuiListClipper clipper;
    clipper.Begin(lines, pitch.y);
    if (reveal) {
        clipper.IncludeItemByIndex(*reveal);
    }
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            // Each tile is put in its place: the arrow of a file is drawn after the tile,
            // and the next tile is not placed after the arrow.
            const ImVec2 lineStart = ImGui::GetCursorScreenPos();
            const size_t first = static_cast<size_t>(line) * static_cast<size_t>(columns);
            for (size_t index = first; index < std::min(first + static_cast<size_t>(columns), m_contents.size());
                 ++index) {
                const float left = lineStart.x + static_cast<float>(index - first) * pitch.x;
                drawTile(context, m_contents[index], left, lineStart.y, tileHeight, submittedLines, click);
            }
            // The line takes its room, for the clipper and the scroll bar to count.
            ImGui::SetCursorScreenPos(lineStart);
            ImGui::Dummy(ImVec2(static_cast<float>(columns) * pitch.x - style.ItemSpacing.x, tileHeight));
            if (reveal == line) {
                ImGui::SetScrollHereY();
            }
        }
    }
}

void AssetBrowserPanel::drawTile(const EditorContext& context, const AssetBrowserRow& row, float left, float top,
                                 float height, std::vector<std::string>& submittedLines, Click& click) {
    const ImVec2 start(left, top);
    ImGui::PushID(idOf(row));
    ImGui::SetCursorScreenPos(start);
    // What is inside a file follows the file, on a ground of its own.
    if (row.depth > 0) {
        ImGui::GetWindowDrawList()->AddRectFilled(start, ImVec2(left + kTileWidth, top + height),
                                                  ImGui::GetColorU32(ImGuiCol_FrameBg),
                                                  ImGui::GetStyle().FrameRounding);
    }
    // The arrow of a file is drawn over the tile, and takes the pointer from it.
    const ImGuiSelectableFlags flags =
        ImGuiSelectableFlags_AllowDoubleClick | (row.hasChildren ? ImGuiSelectableFlags_AllowOverlap : 0);
    bool doubleClicked = false;
    if (ImGui::Selectable("##tile", row.selected, flags, ImVec2(kTileWidth, height))) {
        if (row.kind == AssetBrowserRow::Kind::Asset) {
            click.chosen = row.asset;
        }
        doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    }
    handleRow(context, row, doubleClicked, submittedLines, click);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsDragDropActive()) {
        ImGui::SetTooltip("%s\n%s   %s", row.label.c_str(), kindOf(row).c_str(), row.path.c_str());
    }
    drawTileFace(row, start);
    if (row.hasChildren && drawOpenArrow(row, start)) {
        click.toggled = row;
    }
    ImGui::PopID();
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

void AssetBrowserPanel::drawMoveDialog(const EditorContext& context, std::vector<std::string>& submittedLines) {
    if (m_openMoveDialog) {
        ImGui::OpenPopup("Rename or move");
        m_openMoveDialog = false;
    }
    if (!ImGui::BeginPopupModal("Rename or move", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    // A directory, or the file of an asset.
    std::string moving = m_movingDirectory;
    if (moving.empty()) {
        const AssetInfo* asset = context.assets().database().find(m_moving);
        if (asset == nullptr) {
            // Gone while the dialog was open: the assets were read again.
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        moving = asset->path;
    }
    ImGui::TextUnformatted(fmt::format("Where {} goes, inside the asset directory:", moving).c_str());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(kMoveFieldWidth);
    const bool entered = ImGui::InputText("##to", &m_moveTarget, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled(m_movingDirectory.empty() ? "It keeps its id: what refers to it by id follows it."
                                                  : "Every asset in it keeps its id: what refers to them by id "
                                                    "follows them.");
    if (ImGui::Button("Move") || entered) {
        submittedLines.push_back(m_movingDirectory.empty()
                                     ? moveAssetCommand(m_moving, trim(m_moveTarget))
                                     : moveDirectoryCommand(m_movingDirectory, trim(m_moveTarget)));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
