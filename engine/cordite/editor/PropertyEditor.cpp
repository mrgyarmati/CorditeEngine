#include "cordite/editor/PropertyEditor.h"

#include "cordite/core/Result.h"
#include "cordite/math/Transform.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cordite {
namespace {

/// How far a drag field moves per pixel when the property does not say.
constexpr float kDefaultDragSpeed = 0.01f;
constexpr float kRotationDragSpeed = 0.5f;

/// The heading of a nested object or a list: it takes the whole row, label included,
/// rather than being cut off at the width of the column of names.
constexpr ImGuiTreeNodeFlags kHeaderFlags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns;

/// More than any panel shows at once; beyond it the remembered angles are stale anyway.
constexpr size_t kMaxRememberedRotations = 512;

/// The same for text of JSON that someone is in the middle of typing.
constexpr size_t kMaxRememberedDrafts = 64;

/// How JSON is written out for someone to read and change.
constexpr int kJsonIndent = 2;

/// How many lines of JSON are shown at once.
constexpr float kJsonLines = 6.5f;

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
        case PropertyType::Json:
            return Json::object();
        case PropertyType::Uuid:
        case PropertyType::Enum:
        case PropertyType::Object:
        case PropertyType::List:
            return nullptr;
    }
    return nullptr;
}

/// What `text` is as the value of a Json property - or what keeps it from being one,
/// said before anything is asked to store it.
Result<Json> jsonFromText(const std::string& text) {
    CORDITE_TRY_ASSIGN(const Json parsed, parseJson(text, "the text"));
    Json value;
    if (const Status valid = PropertyCodec<Json>::fromJson(parsed, value); !valid) {
        return tl::unexpected(valid.error().withContext("the text"));
    }
    return value;
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

/// An integer whose bits are switches: one check box per bit, the lowest first, as
/// many as the property's maximum has bits.
bool drawBitMask(const PropertyInfo& property, Json& value) {
    if (!value.is_number_integer()) {
        ImGui::TextDisabled("(not a number)");
        return false;
    }
    // Without a maximum, the 32 bits of the widest integer a component declares.
    constexpr uint64_t kAllBits = 0xFFFFFFFFu;
    const PropertyAttributes& attributes = property.attributes();
    const uint64_t allowed =
        attributes.maximum ? static_cast<uint64_t>(std::clamp(*attributes.maximum, 0.0, 4294967295.0)) : kAllBits;
    const int bitCount = std::bit_width(allowed);
    auto bits = static_cast<uint64_t>(std::max<int64_t>(value.get<int64_t>(), 0));

    bool changed = false;
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float boxWidth = ImGui::GetFrameHeight();
    for (int bit = 0; bit < bitCount; ++bit) {
        // On the same line as long as there is room, so that a narrow panel wraps the
        // row instead of cutting it off.
        if (bit > 0) {
            ImGui::SameLine(0.0f, spacing);
            if (ImGui::GetContentRegionAvail().x < boxWidth) {
                ImGui::NewLine();
            }
        }
        ImGui::PushID(bit);
        bool isSet = ((bits >> bit) & 1u) != 0;
        if (ImGui::Checkbox("##bit", &isSet)) {
            bits ^= uint64_t{1} << bit;
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%d", bit + 1);
        }
        ImGui::PopID();
    }
    if (changed) {
        value = static_cast<int64_t>(bits);
    }
    return changed;
}

/// A color: a swatch as wide as the row, which opens a picker when it is clicked. The
/// numbers are in the picker; three or four of them do not fit a narrow panel side by
/// side. Colors are not limited to one: a light may be brighter than white.
bool drawColor(std::array<float, 4>& components, int componentCount) {
    const bool hasAlpha = componentCount == 4;
    const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                      (hasAlpha ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha);
    const ImVec4 color(components[0], components[1], components[2], hasAlpha ? components[3] : 1.0f);
    if (ImGui::ColorButton("##value", color, flags, ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight()))) {
        ImGui::OpenPopup("##picker");
    }

    bool changed = false;
    if (ImGui::BeginPopup("##picker")) {
        changed = hasAlpha ? ImGui::ColorPicker4("##color", components.data(), flags)
                           : ImGui::ColorPicker3("##color", components.data(), flags);
        ImGui::EndPopup();
    }
    return changed;
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
        changed = drawColor(components, componentCount);
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
            return property.attributes().semantic == PropertySemantic::BitMask ? drawBitMask(property, value)
                                                                               : drawNumber(property, value, true);
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
        case PropertyType::Json:
            return drawJson(value);
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
        // Adding zero turns a negative zero into a plain one, which would otherwise be
        // shown as "-0.0".
        state.eulerDegrees = eulerDegreesFromQuat(rotation) + Vec3(0.0f);
    }

    if (!ImGui::DragFloat3("##value", &state.eulerDegrees.x, kRotationDragSpeed, 0.0f, 0.0f, "%.1f")) {
        return false;
    }
    state.rotation = quatFromEulerDegrees(state.eulerDegrees);
    value = Json::array({jsonFromFloat(state.rotation.x), jsonFromFloat(state.rotation.y),
                         jsonFromFloat(state.rotation.z), jsonFromFloat(state.rotation.w)});
    return true;
}

bool PropertyEditor::drawJson(Json& value) {
    if (m_jsonDrafts.size() > kMaxRememberedDrafts) {
        // Not the one that is being typed, nor one that is waiting to be put right.
        std::erase_if(m_jsonDrafts,
                      [](const auto& entry) { return !entry.second.editing && entry.second.problem.empty(); });
    }
    JsonDraft& draft = m_jsonDrafts[ImGui::GetID("##value")];
    // What is shown is the value, until someone types: from then on it is their text,
    // until it has become the value or the value has become another.
    const std::string current = dumpJson(value, kJsonIndent);
    if (!draft.editing && draft.source != current) {
        draft = {.text = current, .source = current, .problem = {}, .editing = false};
    }

    const ImVec2 size(-std::numeric_limits<float>::min(), ImGui::GetTextLineHeight() * kJsonLines);
    ImGui::InputTextMultiline("##value", &draft.text, size, ImGuiInputTextFlags_AllowTabInput);
    draft.editing = ImGui::IsItemActive();

    // The text becomes the value when its author leaves it - if it is one. If it is not,
    // it stays as it was typed, with what is wrong with it underneath.
    bool changed = false;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (Result<Json> typed = jsonFromText(draft.text)) {
            draft.problem.clear();
            changed = *typed != value;
            value = std::move(*typed);
            // Shown the way the engine writes it from here on, however it was typed.
            draft.source = dumpJson(value, kJsonIndent);
            draft.text = draft.source;
        } else {
            draft.problem = typed.error().message;
        }
    }
    if (!draft.problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", draft.problem.c_str());
        ImGui::PopStyleColor();
    }
    return changed;
}

bool PropertyEditor::drawReference(const PropertyInfo& property, Json& value) {
    const std::optional<Uuid> id = value.is_string() ? Uuid::parse(value.get<std::string>()) : std::nullopt;

    // The field leaves room for the button that clears it.
    const float clearButtonWidth = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(clearButtonWidth + ImGui::GetStyle().ItemInnerSpacing.x));
    bool changed = property.attributes().semantic == PropertySemantic::AssetReference
                       ? drawAssetReference(property, id, value)
                       : drawEntityReference(id, value);

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!id.has_value());
    if (ImGui::Button("x", ImVec2(clearButtonWidth, 0.0f))) {
        value = nullptr;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool PropertyEditor::drawEntityReference(const std::optional<Uuid>& id, Json& value) {
    std::string text = "(none)";
    if (id) {
        text = m_entityName ? m_entityName(*id) : id->toString();
    }
    ImGui::InputText("##value", &text, ImGuiInputTextFlags_ReadOnly);

    // An entity is assigned by dragging it here from the hierarchy.
    bool changed = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload);
            payload != nullptr && payload->DataSize == static_cast<int>(sizeof(Uuid))) {
            Uuid dropped;
            std::memcpy(&dropped, payload->Data, sizeof(Uuid));
            value = dropped.toString();
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

bool PropertyEditor::drawAssetReference(const PropertyInfo& property, const std::optional<Uuid>& id, Json& value) {
    std::string text = "(none)";
    if (id) {
        text = m_assetName ? m_assetName(*id) : id->toString();
    }
    if (!ImGui::BeginCombo("##value", text.c_str(), ImGuiComboFlags_HeightLarge)) {
        // Closed, the field takes an asset that is dragged onto it from the browser - one
        // of the kind it lists, or the drop is not for it.
        bool dropped = false;
        if (m_assetChoices && ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
            if (dragged != nullptr && dragged->IsDataType(kAssetPayload) &&
                dragged->DataSize == static_cast<int>(sizeof(Uuid))) {
                Uuid asset;
                std::memcpy(&asset, dragged->Data, sizeof(Uuid));
                const std::vector<AssetChoice> choices = m_assetChoices(property.attributes().referenceType);
                const bool fits = std::ranges::find(choices, asset, &AssetChoice::id) != choices.end();
                if (fits && ImGui::AcceptDragDropPayload(kAssetPayload) != nullptr) {
                    value = asset.toString();
                    dropped = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        return dropped;
    }

    // The list is only asked for while it is open.
    bool changed = false;
    if (m_assetChoices) {
        for (const AssetChoice& choice : m_assetChoices(property.attributes().referenceType)) {
            const bool isSelected = id && choice.id == *id;
            // Two assets may share a label; their ids keep the entries apart.
            ImGui::PushID(choice.id.toString().c_str());
            if (ImGui::Selectable(choice.label.c_str(), isSelected) && !isSelected) {
                value = choice.id.toString();
                changed = true;
            }
            ImGui::PopID();
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
    }
    ImGui::EndCombo();
    return changed;
}

bool PropertyEditor::drawObject(const PropertyInfo& property, Json& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const bool open =
        ImGui::TreeNodeEx("##object", ImGuiTreeNodeFlags_DefaultOpen | kHeaderFlags, "%s", property.name().c_str());
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
    const bool open = ImGui::TreeNodeEx("##list", kHeaderFlags, "%s (%zu)", property.name().c_str(), count);
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
