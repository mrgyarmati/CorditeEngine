#include "cordite/editor/InspectorPanel.h"

#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorFeedback.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <limits>
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

} // namespace

void InspectorPanel::draw(EditorContext& context) {
    if (ImGui::Begin("Inspector")) {
        m_properties.setEntityNameLookup([&context](const Uuid& id) {
            const Entity entity = context.world().findById(id);
            return entity.isNull() ? fmt::format("(missing {})", id.toString()) : context.entityPath(entity);
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
            for (const ComponentDescriptor* descriptor : context.components().components()) {
                // The entity may be gone if one of the controls above deleted it.
                if (context.world().isValid(entity) && descriptor->has(context.world().registry(), entity.handle)) {
                    drawComponent(context, entity, *descriptor);
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

void InspectorPanel::drawComponent(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor) {
    ImGui::PushID(descriptor.type.name().c_str());

    // Components every entity must have get no close button.
    bool keep = true;
    const bool open = ImGui::CollapsingHeader(
        descriptor.type.name().c_str(), descriptor.traits.optional ? &keep : nullptr, ImGuiTreeNodeFlags_DefaultOpen);
    if (!descriptor.traits.description.empty()) {
        ImGui::SetItemTooltip("%s", descriptor.traits.description.c_str());
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
            if (m_properties.draw(property, value)) {
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
        if (!descriptor->traits.optional || descriptor->has(context.world().registry(), entity.handle)) {
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
