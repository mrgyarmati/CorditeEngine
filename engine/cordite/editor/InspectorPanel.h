#pragma once

/// @file
/// The editor panel that shows and edits the selected entity: its name, whether it is
/// enabled, and every component with all its properties.
///
/// It is generated from the reflected description of the components, so a new component
/// or property shows up without any change here.
///
/// @code
/// InspectorPanel inspector;
/// std::vector<std::string> submitted;
/// inspector.draw(context, submitted);   // once per frame, between the interface's begin and end
/// @endcode
///
/// While a control is being dragged or typed into, the change is applied to the component
/// directly, frame by frame. When the control is let go, the whole interaction becomes a
/// single entry in the undo history.

#include "cordite/core/Json.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/PrefabLinks.h"
#include "cordite/editor/PropertyEditor.h"

#include <optional>
#include <string>
#include <vector>

namespace cordite {

class InspectorPanel {
public:
    /// What the window is known by: its place in a layout is remembered by it, whatever its
    /// tab shows (windowName() in UiWidgets.h).
    static constexpr const char* kWindowId = "###Inspector";

    /// Draws the panel as its own window. What is to be run between frames - the revert and
    /// the apply of what an instance of a prefab keeps of its own (prefabCommandLine) - is
    /// added to `submittedLines`; running it is up to the caller.
    void draw(EditorContext& context, std::vector<std::string>& submittedLines);

private:
    void drawEntityHeader(EditorContext& context, Entity entity);
    void drawComponent(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                       const PrefabView& view, std::vector<std::string>& submittedLines);
    void drawAddComponent(EditorContext& context, Entity entity);

    /// Applies a property the user just changed and remembers what the component looked
    /// like before the interaction began.
    void applyPropertyChange(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                             void* instance, const PropertyInfo& property, const Json& value);

    /// Turns the interaction in progress, if any, into one undoable edit.
    void commitInteraction(EditorContext& context);

    /// A component that a control still held by the user is changing.
    struct Interaction {
        Uuid entity;
        const ComponentDescriptor* descriptor = nullptr;
        /// The component before the first change (TypeInfo::toJson).
        Json before;
    };

    PropertyEditor m_properties;
    std::optional<Interaction> m_interaction;
    /// The text in the name field, which differs from the name while it is being typed.
    std::string m_nameText;
};

} // namespace cordite
