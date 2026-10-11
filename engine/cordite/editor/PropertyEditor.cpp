#include "cordite/editor/PropertyEditor.h"

#include "cordite/math/Transform.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cordite {
namespace {

/// How far a drag field moves per pixel when the property does not say.
constexpr float kDefaultDragSpeed = 0.01f;
constexpr float kRotationDragSpeed = 0.5f;

/// More than any panel shows at once; beyond it the remembered angles are stale anyway.
constexpr size_t kMaxRememberedRotations = 512;

/// Two quaternions closer than this describe the same rotation for display purposes.
constexpr float kSameRotationTolerance = 1.0e-6f;

/// The printf format for a number with the property's unit after it.
std::string numberFormat(const PropertyAttributes& attributes, const char* number) {
    std::string format = number;
    if (!attributes.unit.empty()) {
        // A literal percent sign in a unit must not be read as a format directive.
        std::string unit = attributes.unit;
        for (size_t position = unit.find('%'); position != std::string::npos; position = unit.find('%', position + 2)) {
            unit.insert(position, "%");
        }
        format += " " + unit;
    }
    return format;
}

float dragSpeed(const PropertyAttributes& attributes) {
    return attributes.step > 0.0 ? static_cast<float>(attributes.step) : kDefaultDragSpeed;
}

/// The value a new element of a list starts with.
Json defaultValue(PropertyType type) {
    switch (type) {
        case PropertyType::Bool:
            return false;
        case PropertyType::Int:
            return 0;
        case PropertyType::Float:
            return 0.0;
        case PropertyType::String:
            return "";
        case PropertyType::Vec2:
            return Json::array({0.0, 0.0});
        case PropertyType::Vec3:
            return Json::array({0.0, 0.0, 0.0});
        case PropertyType::Vec4:
            return Json::array({0.0, 0.0, 0.0, 0.0});
        case PropertyType::Quat:
            return Json::array({0.0, 0.0, 0.0, 1.0});
        case PropertyType::Uuid:
        case PropertyType::Enum:
        case PropertyType::Object:
        case PropertyType::List:
            return nullptr;
    }
    return nullptr;
}

/// Describes a property for the tooltip of its label. The name is repeated because the
/// label may be cut short in a narrow panel.
std::string tooltipText(const PropertyInfo& property) {
    const PropertyAttributes& attributes = property.attributes();
    std::string text = property.name();
    if (property.type() == PropertyType::Quat) {
        text += "\nShown as rotations about X, Y and Z in degrees.";
    } else if (!attributes.unit.empty() && !(attributes.minimum || attributes.maximum)) {
        text += fmt::format("\nUnit: {}", attributes.unit);
    }
    if (!attributes.description.empty()) {
        text += "\n" + attributes.description;
    }
    if (attributes.minimum || attributes.maximum) {
        text += text.empty() ? "" : "\n";
        if (attributes.minimum && attributes.maximum) {
            text += fmt::format("Range: {} to {}", *attributes.minimum, *attributes.maximum);
        } else if (attributes.minimum) {
            text += fmt::format("Minimum: {}", *attributes.minimum);
        } else {
            text += fmt::format("Maximum: {}", *attributes.maximum);
        }
        text += attributes.unit.empty() ? "" : " " + attributes.unit;
    }
    if (attributes.readOnly) {
        text += text.empty() ? "" : "\n";
        text += "Read-only.";
    }
    return text;
}

void drawLabel(const PropertyInfo& property) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(property.name().c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (const std::string tooltip = tooltipText(property); !tooltip.empty()) {
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
    }
}

bool drawNumber(const PropertyInfo& property, Json& value, bool isInteger) {
    const PropertyAttributes& attributes = property.attributes();
    if (!value.is_number()) {
        ImGui::TextDisabled("(not a number)");
        return false;
    }

    if (isInteger) {
        auto number = value.get<int64_t>();
        // A double holds every bound an integer property can declare closely enough.
        const auto minimum = attributes.minimum ? static_cast<int64_t>(std::ceil(*attributes.minimum))
                                                : std::numeric_limits<int64_t>::min();
        const auto maximum = attributes.maximum ? static_cast<int64_t>(std::floor(*attributes.maximum))
                                                : std::numeric_limits<int64_t>::max();
        const std::string format = numberFormat(attributes, "%lld");
        const float speed = attributes.step > 0.0 ? static_cast<float>(attributes.step) : 1.0f;
        if (ImGui::DragScalar("##value", ImGuiDataType_S64, &number, speed, &minimum, &maximum, format.c_str(),
                              ImGuiSliderFlags_AlwaysClamp)) {
            value = number;
            return true;
        }
        return false;
    }

    auto number = value.get<double>();
    const double minimum = attributes.minimum.value_or(-std::numeric_limits<double>::max());
    const double maximum = attributes.maximum.value_or(std::numeric_limits<double>::max());
    const std::string format = numberFormat(attributes, "%.3f");
    if (ImGui::DragScalar("##value", ImGuiDataType_Double, &number, dragSpeed(attributes), &minimum, &maximum,
                          format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
        value = number;
        return true;
    }
    return false;
}

bool drawVector(const PropertyInfo& property, Json& value, int componentCount) {
    const PropertyAttributes& attributes = property.attributes();
    const auto count = static_cast<size_t>(componentCount);
    if (!value.is_array() || value.size() != count) {
        ImGui::TextDisabled("(not a vector)");
        return false;
    }

    std::array<float, 4> components{};
    for (size_t index = 0; index < count; ++index) {
        components[index] = value[index].is_number() ? value[index].get<float>() : 0.0f;
    }

    bool changed = false;
    if (attributes.semantic == PropertySemantic::Color && componentCount >= 3) {
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
        changed = componentCount == 3 ? ImGui::ColorEdit3("##value", components.data(), flags)
                                      : ImGui::ColorEdit4("##value", components.data(), flags);
    } else {
        const float minimum =
            attributes.minimum ? static_cast<float>(*attributes.minimum) : -std::numeric_limits<float>::max();
        const float maximum =
            attributes.maximum ? static_cast<float>(*attributes.maximum) : std::numeric_limits<float>::max();
        // No unit here: with several fields side by side there is no room for it. The
        // tooltip of the label states it.
        changed = ImGui::DragScalarN("##value", ImGuiDataType_Float, components.data(), componentCount,
                                     dragSpeed(attributes), &minimum, &maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    }

    if (changed) {
        for (size_t index = 0; index < count; ++index) {
            value[index] = jsonFromFloat(components[index]);
        }
    }
    return changed;
}

bool drawEnum(const PropertyInfo& property, Json& value) {
    const std::string current = value.is_string() ? value.get<std::string>() : dumpJson(value, -1);
    if (!ImGui::BeginCombo("##value", current.c_str())) {
        return false;
    }

    bool changed = false;
    for (const EnumOption& option : property.enumOptions()) {
        const bool isSelected = option.name == current;
        if (ImGui::Selectable(option.name.c_str(), isSelected) && !isSelected) {
            value = option.name;
            changed = true;
        }
        if (isSelected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
    return changed;
}

} // namespace

bool PropertyEditor::draw(const PropertyInfo& property, Json& value) {
    ImGui::PushID(property.name().c_str());
    ImGui::BeginDisabled(property.attributes().readOnly);

    bool changed = false;
    if (property.type() == PropertyType::Object) {
        changed = drawObject(property, value);
    } else if (property.type() == PropertyType::List) {
        changed = drawList(property, value);
    } else {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        drawLabel(property);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
        changed = drawValue(property.type(), property, value);
    }

    ImGui::EndDisabled();
    ImGui::PopID();
    return changed && !property.attributes().readOnly;
}

bool PropertyEditor::drawValue(PropertyType type, const PropertyInfo& property, Json& value) {
    switch (type) {
        case PropertyType::Bool: {
            bool checked = value.is_boolean() && value.get<bool>();
            if (ImGui::Checkbox("##value", &checked)) {
                value = checked;
                return true;
            }
            return false;
        }
        case PropertyType::Int:
            return drawNumber(property, value, true);
        case PropertyType::Float:
            return drawNumber(property, value, false);
        case PropertyType::String: {
            std::string text = value.is_string() ? value.get<std::string>() : std::string();
            const bool edited = property.attributes().semantic == PropertySemantic::Multiline
                                    ? ImGui::InputTextMultiline(
                                          "##value", &text,
                                          ImVec2(-std::numeric_limits<float>::min(), ImGui::GetTextLineHeight() * 4.5f))
                                    : ImGui::InputText("##value", &text);
            if (edited) {
                value = text;
            }
            return edited;
        }
        case PropertyType::Vec2:
            return drawVector(property, value, 2);
        case PropertyType::Vec3:
            return drawVector(property, value, 3);
        case PropertyType::Vec4:
            return drawVector(property, value, 4);
        case PropertyType::Quat:
            return drawRotation(value);
        case PropertyType::Uuid:
            return drawReference(property, value);
        case PropertyType::Enum:
            return drawEnum(property, value);
        case PropertyType::Object:
        case PropertyType::List:
            // Composite values get rows of their own in draw(); nested in a list they
            // have no editor yet.
            ImGui::TextDisabled("(not editable here)");
            return false;
    }
    return false;
}

bool PropertyEditor::drawRotation(Json& value) {
    if (!value.is_array() || value.size() != 4) {
        ImGui::TextDisabled("(not a rotation)");
        return false;
    }
    // Stored as [x, y, z, w]; glm's constructor takes w first.
    const Quat rotation(value[3].get<float>(), value[0].get<float>(), value[1].get<float>(), value[2].get<float>());

    if (m_rotations.size() > kMaxRememberedRotations) {
        m_rotations.clear();
    }
    RotationState& state = m_rotations[ImGui::GetID("##value")];
    // The angles are recomputed only when the rotation was changed by something else.
    if (std::abs(std::abs(glm::dot(state.rotation, rotation)) - 1.0f) > kSameRotationTolerance) {
        state.rotation = rotation;
        state.eulerDegrees = eulerDegreesFromQuat(rotation);
    }

    if (!ImGui::DragFloat3("##value", &state.eulerDegrees.x, kRotationDragSpeed, 0.0f, 0.0f, "%.1f")) {
        return false;
    }
    state.rotation = quatFromEulerDegrees(state.eulerDegrees);
    value = Json::array({jsonFromFloat(state.rotation.x), jsonFromFloat(state.rotation.y),
                         jsonFromFloat(state.rotation.z), jsonFromFloat(state.rotation.w)});
    return true;
}

bool PropertyEditor::drawReference(const PropertyInfo& property, Json& value) {
    const bool isEntity = property.attributes().semantic == PropertySemantic::EntityReference;
    const std::optional<Uuid> id = value.is_string() ? Uuid::parse(value.get<std::string>()) : std::nullopt;

    std::string text = "(none)";
    if (id) {
        text = isEntity && m_entityName ? m_entityName(*id) : id->toString();
    }

    const float clearButtonWidth = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(clearButtonWidth + ImGui::GetStyle().ItemInnerSpacing.x));
    ImGui::InputText("##value", &text, ImGuiInputTextFlags_ReadOnly);

    bool changed = false;
    if (isEntity && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload);
            payload != nullptr && payload->DataSize == static_cast<int>(sizeof(Uuid))) {
            Uuid dropped;
            std::memcpy(&dropped, payload->Data, sizeof(Uuid));
            value = dropped.toString();
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!id.has_value());
    if (ImGui::Button("x", ImVec2(clearButtonWidth, 0.0f))) {
        value = nullptr;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool PropertyEditor::drawObject(const PropertyInfo& property, Json& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const bool open = ImGui::TreeNodeEx("##object", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns,
                                        "%s", property.name().c_str());
    ImGui::TableNextColumn();
    if (!open) {
        return false;
    }

    bool changed = false;
    if (property.objectType() != nullptr && value.is_object()) {
        for (const PropertyInfo& member : property.objectType()->properties()) {
            // Runtime-only members are not part of the JSON.
            if (const auto memberValue = value.find(member.name()); memberValue != value.end()) {
                changed = draw(member, *memberValue) || changed;
            }
        }
    }
    ImGui::TreePop();
    return changed;
}

bool PropertyEditor::drawList(const PropertyInfo& property, Json& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const size_t count = value.is_array() ? value.size() : 0;
    const bool open =
        ImGui::TreeNodeEx("##list", ImGuiTreeNodeFlags_SpanAllColumns, "%s (%zu)", property.name().c_str(), count);
    ImGui::TableNextColumn();
    if (!open) {
        return false;
    }

    bool changed = false;
    if (value.is_array()) {
        size_t removeIndex = count;
        for (size_t index = 0; index < count; ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("[%zu]", index);
            ImGui::TableNextColumn();

            const float removeButtonWidth = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(removeButtonWidth + ImGui::GetStyle().ItemInnerSpacing.x));
            changed = drawValue(property.elementType(), property, value[index]) || changed;
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button("-", ImVec2(removeButtonWidth, 0.0f))) {
                removeIndex = index;
            }
            ImGui::PopID();
        }
        if (removeIndex < count) {
            value.erase(removeIndex);
            changed = true;
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        Json element = defaultValue(property.elementType());
        // Only lists of plain values can grow here; the rest have no default to add.
        const bool canAdd = !element.is_null() || property.elementType() == PropertyType::Uuid;
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button("Add element")) {
            value.push_back(std::move(element));
            changed = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::TreePop();
    return changed;
}

} // namespace cordite
