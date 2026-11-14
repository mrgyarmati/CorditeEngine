#include "cordite/editor/InspectorPanel.h"

#include "cordite/core/StringUtils.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/EditorIcons.h"
#include "cordite/editor/PrefabLinks.h"
#include "cordite/ui/UiSystem.h"
#include "cordite/ui/UiTheme.h"
#include "cordite/ui/UiWidgets.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The names of properties get as much width as the longest of them needs, up to this
/// share of the panel; beyond it they are cut short and the tooltip shows them in full.
constexpr float kMaxLabelColumnShare = 0.55f;

/// The width that fits the name of every property of a type.
float labelColumnWidth(const TypeInfo& type) {
    float widest = 0.0f;
    for (const PropertyInfo& property : type.properties()) {
        widest = std::max(widest, ImGui::CalcTextSize(property.name().c_str()).x);
    }
    const float padded = widest + ImGui::GetStyle().CellPadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    return std::min(padded, ImGui::GetContentRegionAvail().x * kMaxLabelColumnShare);
}

/// Whether a component has a section of its own in the inspector. The link of an entity of an
/// instance of a prefab has none: what it says is shown above the components, in words, and
/// it is the editor's to change - added or taken away by hand it would cut an entity off its
/// prefab, or tie it to one by mistake.
bool isShown(const ComponentDescriptor& descriptor) {
    return descriptor.type.name() != PrefabLinkComponent::kName;
}

/// How an asset is shown in a field: its name, and where it is found.
std::string assetLabel(const AssetInfo& asset) {
    return fmt::format("{}  ({})", asset.name, asset.reference());
}

/// What the entity is to a prefab, with what can be done about what it keeps of its own.
void drawPrefab(EditorContext& context, Entity entity, const PrefabView& view,
                std::vector<std::string>& submittedLines) {
    if (view.role == PrefabView::Role::None || !context.world().isValid(entity)) {
        return;
    }
    const World& world = context.world();
    const UiPalette& palette = UiSystem::palette();
    const auto prefabName = [&context](const Uuid& prefab) {
        const AssetInfo* asset = context.assets().database().find(prefab);
        return asset != nullptr ? asset->reference() : prefab.toString();
    };
    const std::string prefab = prefabName(view.prefab);
    // Wrapped: the path of a prefab is longer than a narrow panel.
    ImGui::PushStyleColor(ImGuiCol_Text, palette.prefab);
    switch (view.role) {
        case PrefabView::Role::Root:
            ImGui::TextWrapped("%s  Instance of %s", icons::kPrefab.text(), prefab.c_str());
            break;
        case PrefabView::Role::Part:
            ImGui::TextWrapped("%s  Part of '%s', an instance of %s", icons::kPrefab.text(),
                               world.name(view.root).c_str(), prefab.c_str());
            break;
        case PrefabView::Role::Added:
            ImGui::TextWrapped("%s  Added to '%s', an instance of %s", icons::kPrefab.text(),
                               world.name(view.root).c_str(), prefab.c_str());
            break;
        case PrefabView::Role::None:
            break;
    }
    ImGui::PopStyleColor();
    if (!view.innerPrefab.isNil()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.textDisabled);
        ImGui::TextWrapped("It is %s, inside that prefab.", prefabName(view.innerPrefab).c_str());
        ImGui::PopStyleColor();
    }
    if (view.role == PrefabView::Role::Added) {
        ImGui::TextWrapped("The prefab does not have it: apply '%s' to put it there.", world.name(view.root).c_str());
    }
    if (view.role != PrefabView::Role::Added) {
        if (view.overrides.empty()) {
            ImGui::TextDisabled("It keeps nothing of its own.");
        } else {
            ImGui::TextWrapped("It keeps of its own: %s", view.describeOverrides().c_str());
        }
        // From the root, the whole instance; from an entity of it, what that one keeps.
        const bool whole = view.role == PrefabView::Role::Root;
        ImGui::BeginDisabled(!whole && view.overrides.empty());
        if (ImGui::Button(whole ? "Revert instance" : "Revert")) {
            submittedLines.push_back(prefabCommandLine("prefab.revert", world, entity));
        }
        ImGui::SetItemTooltip("%s", whole ? "Gives every entity of the instance back what its prefab has."
                                          : "Gives the entity back what its prefab has.");
        ImGui::SameLine();
        if (ImGui::Button(whole ? "Apply instance" : "Apply")) {
            submittedLines.push_back(prefabCommandLine("prefab.apply", world, entity));
        }
        ImGui::SetItemTooltip("%s", whole ? "Writes what the instance keeps of its own, and what was added to it, "
                                            "into its prefab, which every instance then takes."
                                          : "Writes what the entity keeps of its own into its prefab, which every "
                                            "instance then takes.");
        ImGui::EndDisabled();
    }
    if (view.role != PrefabView::Role::Root) {
        if (view.role != PrefabView::Role::Added) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Select root")) {
            context.setSelection(std::span(&view.root, 1));
        }
    }
    ImGui::Separator();
}

} // namespace

void InspectorPanel::draw(EditorContext& context, std::vector<std::string>& submittedLines) {
    static const std::string kName = windowName(icons::kInspector, "Inspector");
    if (ImGui::Begin(kName.c_str())) {
        m_properties.setEntityNameLookup([&context](const Uuid& id) {
            const Entity entity = context.world().findById(id);
            return entity.isNull() ? fmt::format("(missing {})", id.toString()) : context.entityPath(entity);
        });
        m_properties.setAssetLookup(
            [&context](const Uuid& id) {
                const AssetInfo* asset = context.assets().database().find(id);
                return asset != nullptr ? assetLabel(*asset) : fmt::format("(missing {})", id.toString());
            },
            [&context](std::string_view referenceType) {
                std::vector<PropertyEditor::AssetChoice> choices;
                for (const AssetInfo& asset : context.assets().database().assets()) {
                    if (referenceType.empty() || equalsIgnoreCase(referenceType, nameOf(asset.type))) {
                        choices.push_back({asset.id, assetLabel(asset)});
                    }
                }
                return choices;
            });

        const std::vector<Entity> selection = context.selection();
        if (selection.empty()) {
            ImGui::TextDisabled("Nothing is selected.");
        } else if (selection.size() > 1) {
            ImGui::TextDisabled("%zu entities are selected.", selection.size());
            ImGui::TextWrapped("Select a single entity to see and edit its components.");
        } else {
            const Entity entity = selection.front();
            drawEntityHeader(context, entity);
            const PrefabView view = prefabViewOf(context.world(), entity);
            drawPrefab(context, entity, view, submittedLines);
            for (const ComponentDescriptor* descriptor : context.components().components()) {
                // The entity may be gone if one of the controls above deleted it.
                if (isShown(*descriptor) && context.world().isValid(entity) &&
                    descriptor->has(context.world().registry(), entity.handle)) {
                    drawComponent(context, entity, *descriptor, view, submittedLines);
                }
            }
            if (context.world().isValid(entity)) {
                drawAddComponent(context, entity);
            }
        }
    }
    ImGui::End();

    // An interaction ends when the control that started it is released. Until then its
    // changes are not in the history, so that a drag is one edit rather than hundreds.
    if (m_interaction && !ImGui::IsAnyItemActive()) {
        commitInteraction(context);
    }
}

void InspectorPanel::drawEntityHeader(EditorContext& context, Entity entity) {
    const World& world = context.world();

    bool enabled = world.isEnabled(entity);
    if (ImGui::Checkbox("##enabled", &enabled)) {
        reportIfFailed(EditorActions(context).setEntityEnabled(entity, enabled));
    }
    ImGui::SetItemTooltip("Enabled. A disabled entity, and everything below it, is ignored.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
    ImGui::InputText("##name", &m_nameText);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        // Enter or leaving the field ends the edit. An unusable name is reported and
        // the field goes back to the name the entity has.
        reportIfFailed(EditorActions(context).renameEntity(entity, m_nameText));
    }
    if (!ImGui::IsItemActive()) {
        m_nameText = world.name(entity);
    }

    const std::string id = world.id(entity).toString();
    ImGui::TextDisabled("%s", id.c_str());
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(id.c_str());
    }
    ImGui::SetItemTooltip("The entity's permanent id. Click to copy it.");
    ImGui::Separator();
}

void InspectorPanel::drawComponent(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                                   const PrefabView& view, std::vector<std::string>& submittedLines) {
    ImGui::PushID(descriptor.type.name().c_str());
    const std::string& name = descriptor.type.name();
    const UiPalette& palette = UiSystem::palette();

    // Components every entity must have get no close button. One that was added to an
    // instance of a prefab, which its prefab does not have, is in the color of prefabs.
    bool keep = true;
    const bool added = view.addedComponent(name);
    if (added) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.prefab);
    }
    const bool open =
        sectionHeader(name.c_str(), descriptor.traits.optional ? &keep : nullptr, ImGuiTreeNodeFlags_DefaultOpen);
    if (added) {
        ImGui::PopStyleColor();
    }
    if (!descriptor.traits.description.empty() || added) {
        ImGui::SetItemTooltip("%s%s", descriptor.traits.description.c_str(),
                              added ? "\nAdded to this entity: its prefab does not have it." : "");
    }

    if (!keep) {
        commitInteraction(context);
        reportIfFailed(EditorActions(context).removeComponent(entity, descriptor.type.name()));
    } else if (const float labelWidth = labelColumnWidth(descriptor.type);
               open && ImGui::BeginTable("properties", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        void* instance = descriptor.get(context.world().registry(), entity.handle);
        for (const PropertyInfo& property : descriptor.type.properties()) {
            Json value = property.get(instance);
            // What an instance of a prefab keeps of its own is marked, with what can be done
            // about it in the menu of its name.
            PropertyEditor::LabelStyle label;
            if (view.keepsProperty(name, property.name())) {
                // Shared by the copies of the menu, which are made as it is handed on.
                const auto entry = std::make_shared<const std::string>(
                    added ? fmt::format("+{}", name) : fmt::format("{}.{}", name, property.name()));
                label.color = palette.prefab;
                label.note = added ? "Its own: the component was added to this entity."
                                   : "Its own: the prefab's value does not reach it.";
                const World& world = context.world();
                label.menu = [&submittedLines, &world, entity, entry] {
                    if (ImGui::MenuItem("Revert to the prefab")) {
                        submittedLines.push_back(prefabCommandLine("prefab.revert", world, entity, *entry));
                    }
                    if (ImGui::MenuItem("Apply to the prefab")) {
                        submittedLines.push_back(prefabCommandLine("prefab.apply", world, entity, *entry));
                    }
                };
            }
            if (m_properties.draw(property, value, label)) {
                applyPropertyChange(context, entity, descriptor, instance, property, value);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void InspectorPanel::drawAddComponent(EditorContext& context, Entity entity) {
    ImGui::Spacing();
    if (ImGui::Button("Add component", ImVec2(-std::numeric_limits<float>::min(), 0.0f))) {
        ImGui::OpenPopup("add_component");
    }
    if (!ImGui::BeginPopup("add_component")) {
        return;
    }

    bool anyAvailable = false;
    for (const ComponentDescriptor* descriptor : context.components().components()) {
        if (!descriptor->traits.optional || !isShown(*descriptor) ||
            descriptor->has(context.world().registry(), entity.handle)) {
            continue;
        }
        anyAvailable = true;
        if (ImGui::MenuItem(descriptor->type.name().c_str())) {
            commitInteraction(context);
            reportIfFailed(EditorActions(context).addComponent(entity, descriptor->type.name()));
        }
        if (!descriptor->traits.description.empty()) {
            ImGui::SetItemTooltip("%s", descriptor->traits.description.c_str());
        }
    }
    if (!anyAvailable) {
        ImGui::TextDisabled("The entity has every component there is.");
    }
    ImGui::EndPopup();
}

void InspectorPanel::applyPropertyChange(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                                         void* instance, const PropertyInfo& property, const Json& value) {
    Json edit = Json::object();
    edit[property.name()] = value;
    const Result<PropertyPatch> patch = descriptor.type.parseEdit(edit);
    if (!reportIfFailed(patch)) {
        return;
    }

    const Uuid& id = context.world().id(entity);
    const bool continuesInteraction =
        m_interaction && m_interaction->entity == id && m_interaction->descriptor == &descriptor;
    if (!continuesInteraction) {
        commitInteraction(context);
        m_interaction = Interaction{id, &descriptor, descriptor.type.toJson(instance)};
    }
    patch->applyTo(instance);
}

void InspectorPanel::commitInteraction(EditorContext& context) {
    if (!m_interaction) {
        return;
    }
    const Interaction interaction = std::move(*m_interaction);
    m_interaction.reset();

    // The entity may have been deleted while the control was held; then there is
    // nothing left to record.
    const Entity entity = context.world().findById(interaction.entity);
    if (!entity.isNull() && interaction.descriptor->has(context.world().registry(), entity.handle)) {
        reportIfFailed(EditorActions(context).recordComponentEdit(entity, interaction.descriptor->type.name(),
                                                                  interaction.before));
    }
}

} // namespace cordite
