#pragma once

/// @file
/// Widgets for editing reflected properties.
///
/// Given the description of a property and its current value as JSON, a PropertyEditor
/// draws the fitting ImGui widget - a checkbox, a drag field, a color picker, a list of
/// options, a box of text for JSON of no fixed shape - and reports whether the user
/// changed the value. The inspector is built from
/// it, and so is any other panel that edits reflected data, which is why a new component
/// needs no editor code of its own.
///
/// @code
/// PropertyEditor editor;
/// for (const PropertyInfo& property : type.properties()) {
///     Json value = property.get(instance);
///     if (editor.draw(property, value)) {
///         CORDITE_TRY(property.set(instance, value));    // or parse it into an undoable edit
///     }
/// }
/// @endcode
///
/// It only edits the JSON it is handed. Validating the result and storing it is the
/// caller's business, which keeps undo and error handling in one place.

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/Math.h"

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cordite {

class PropertyEditor {
public:
    /// Turns the id of an entity into the text to show for it. Without one, entity
    /// references are shown as ids.
    using EntityNameLookup = std::function<std::string(const Uuid& id)>;

    void setEntityNameLookup(EntityNameLookup lookup) { m_entityName = std::move(lookup); }

    /// One of the assets offered for an asset reference.
    struct AssetChoice {
        Uuid id;
        std::string label;
    };

    /// Turns the id of an asset into the text to show for it, and lists the assets of a
    /// kind (PropertyAttributes::referenceType, such as "Mesh"; empty for any kind) for
    /// the user to choose from. Without them, asset references are shown as ids and can
    /// only be cleared.
    using AssetNameLookup = std::function<std::string(const Uuid& id)>;
    using AssetChoices = std::function<std::vector<AssetChoice>(std::string_view referenceType)>;

    void setAssetLookup(AssetNameLookup name, AssetChoices choices) {
        m_assetName = std::move(name);
        m_assetChoices = std::move(choices);
    }

    /// How the name of a property is shown, beyond the name: for a value that is marked, such
    /// as one that an instance of a prefab keeps of its own.
    struct LabelStyle {
        /// The color of the name; none for the text of the theme.
        std::optional<ImVec4> color;
        /// Said in the tooltip of the name, after what the property is.
        std::string note;
        /// What a right click on the name offers: draws the items of a popup menu. None for
        /// no menu.
        std::function<void()> menu;
    };

    /// Draws a label and a widget for `property` on the current line of a two-column
    /// table and returns true when the user changed `value` this frame. `value` must be
    /// what PropertyInfo::get returns for the property. `label` is how its name is shown;
    /// the members of an object are shown plainly.
    bool draw(const PropertyInfo& property, Json& value, const LabelStyle& label = {});

    /// The name of the drag-and-drop payload that carries an entity: a Uuid. The
    /// hierarchy panel is its source; entity reference fields accept it.
    static constexpr const char* kEntityPayload = "CORDITE_ENTITY";

    /// The name of the drag-and-drop payload that carries an asset: a Uuid. The asset
    /// browser is its source; a field for an asset accepts it when the asset is one of
    /// those it lists, and the hierarchy when it is something to place in a scene.
    static constexpr const char* kAssetPayload = "CORDITE_ASSET";

private:
    bool drawValue(PropertyType type, const PropertyInfo& property, Json& value);
    bool drawRotation(Json& value);
    bool drawJson(Json& value);
    bool drawReference(const PropertyInfo& property, Json& value);
    bool drawEntityReference(const std::optional<Uuid>& id, Json& value);
    bool drawAssetReference(const PropertyInfo& property, const std::optional<Uuid>& id, Json& value);
    bool drawObject(const PropertyInfo& property, Json& value, const LabelStyle& label);
    bool drawList(const PropertyInfo& property, Json& value, const LabelStyle& label);

    /// The Euler angles last shown for a rotation, with the quaternion they stand for.
    struct RotationState {
        Vec3 eulerDegrees{0.0f};
        Quat rotation = kIdentityQuat;
    };

    /// JSON as someone is typing it: text that is not JSON for most of the time it
    /// takes to write, and so cannot be the value yet.
    struct JsonDraft {
        std::string text;
        /// The value the text was made of, written out: when the value is another, the
        /// text is about something that is no longer there.
        std::string source;
        /// What is wrong with the text, if it could not become the value.
        std::string problem;
        bool editing = false;
    };

    EntityNameLookup m_entityName;
    AssetNameLookup m_assetName;
    AssetChoices m_assetChoices;
    /// Per widget. Angles are remembered rather than recomputed from the quaternion every
    /// frame, because that conversion jumps between equivalent sets of angles and would
    /// make a field fight the user who is dragging it.
    std::unordered_map<ImGuiID, RotationState> m_rotations;
    /// Per widget, for as long as it is shown.
    std::unordered_map<ImGuiID, JsonDraft> m_jsonDrafts;
};

} // namespace cordite
