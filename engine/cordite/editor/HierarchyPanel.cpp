#include "cordite/editor/HierarchyPanel.h"

#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/PropertyEditor.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The entity carried by a drag in progress, if it is one of ours.
Entity acceptEntityDrop(const World& world) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PropertyEditor::kEntityPayload);
    if (payload == nullptr || payload->DataSize != static_cast<int>(sizeof(Uuid))) {
        return Entity{};
    }
    Uuid id;
    std::memcpy(&id, payload->Data, sizeof(Uuid));
    return world.findById(id);
}

/// The asset carried by a drag in progress that is something to place in a scene - a
/// model or a prefab. Nil for any other, which is then not taken: no entity is a place
/// to drop a texture.
Uuid acceptPlaceableDrop(const AssetDatabase& database) {
    const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
    if (dragged == nullptr || !dragged->IsDataType(PropertyEditor::kAssetPayload) ||
        dragged->DataSize != static_cast<int>(sizeof(Uuid))) {
        return {};
    }
    Uuid id;
    std::memcpy(&id, dragged->Data, sizeof(Uuid));
    const AssetInfo* asset = database.find(id);
    if (asset == nullptr || !canBePlaced(asset->type)) {
        return {};
    }
    return ImGui::AcceptDragDropPayload(PropertyEditor::kAssetPayload) != nullptr ? id : Uuid();
}

} // namespace

void HierarchyPanel::draw(EditorContext& context) {
    if (ImGui::Begin("Hierarchy")) {
        const World& world = context.world();

        if (ImGui::Button("Create entity")) {
            m_deferredEdit = [&context] {
                const Result<Entity> created =
                    EditorActions(context).createEntity({.parent = context.primarySelection()});
                if (reportIfFailed(created)) {
                    context.setSelection(std::span(&*created, 1));
                }
            };
        }
        ImGui::Separator();

        if (ImGui::BeginChild("tree")) {
            for (const Entity root : world.roots()) {
                drawEntity(context, root);
            }

            // The space below the tree: click to deselect, drop to make a root.
            const ImVec2 remaining = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("background", ImVec2(std::max(remaining.x, 1.0f), std::max(remaining.y, 32.0f)));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                context.clearSelection();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const Entity dropped = acceptEntityDrop(world); !dropped.isNull()) {
                    m_deferredEdit = [&context, dropped] {
                        reportIfFailed(EditorActions(context).setParent(dropped, Entity{}));
                    };
                } else if (const Uuid asset = acceptPlaceableDrop(context.assets().database()); !asset.isNil()) {
                    // A model or a prefab from the asset browser, placed as a root.
                    m_deferredEdit = [&context, asset] {
                        reportIfFailed(EditorActions(context).instantiateAsset(asset));
                    };
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (m_deferredEdit) {
        // Cleared before it runs, so that an edit is free to queue another one.
        const std::function<void()> edit = std::exchange(m_deferredEdit, nullptr);
        edit();
    }
}

void HierarchyPanel::drawEntity(EditorContext& context, Entity entity) {
    const World& world = context.world();
    // Copied: an entry of the context menu below may be the last thing to see them.
    const std::vector<Entity> children = world.children(entity);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (context.isSelected(entity)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::PushID(static_cast<int>(entt::to_integral(entity.handle)));
    // An entity that is switched off, itself or through an ancestor, is shown dimmed.
    const bool dimmed = !world.isEnabledInHierarchy(entity);
    if (dimmed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const bool open = ImGui::TreeNodeEx("##entity", flags, "%s", world.name(entity).c_str());
    if (dimmed) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) {
            context.toggleSelection(entity);
        } else {
            context.setSelection(std::span(&entity, 1));
        }
    }

    if (ImGui::BeginPopupContextItem("entity_menu")) {
        // The menu acts on the selection when the entity is part of it, otherwise on
        // the entity alone.
        if (!context.isSelected(entity)) {
            context.setSelection(std::span(&entity, 1));
        }
        if (ImGui::MenuItem("Create child")) {
            m_deferredEdit = [&context, entity] {
                const Result<Entity> created = EditorActions(context).createEntity({.parent = entity});
                if (reportIfFailed(created)) {
                    context.setSelection(std::span(&*created, 1));
                }
            };
        }
        if (ImGui::MenuItem("Duplicate")) {
            m_deferredEdit = [&context] {
                reportIfFailed(EditorActions(context).duplicateEntities(context.selection()));
            };
        }
        if (ImGui::MenuItem("Delete")) {
            m_deferredEdit = [&context] { reportIfFailed(EditorActions(context).deleteEntities(context.selection())); };
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginDragDropSource()) {
        const Uuid id = world.id(entity);
        ImGui::SetDragDropPayload(PropertyEditor::kEntityPayload, &id, sizeof(Uuid));
        ImGui::TextUnformatted(world.name(entity).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const Entity dropped = acceptEntityDrop(world); !dropped.isNull() && dropped != entity) {
            m_deferredEdit = [&context, child = dropped, newParent = entity] {
                reportIfFailed(EditorActions(context).setParent(child, newParent));
            };
        } else if (const Uuid asset = acceptPlaceableDrop(context.assets().database()); !asset.isNil()) {
            // A model or a prefab from the asset browser, placed under this entity.
            m_deferredEdit = [&context, asset, parent = entity] {
                reportIfFailed(EditorActions(context).instantiateAsset(asset, parent));
            };
        }
        ImGui::EndDragDropTarget();
    }

    if (open) {
        for (const Entity child : children) {
            drawEntity(context, child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace cordite
