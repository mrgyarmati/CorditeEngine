#pragma once

/// @file
/// The editor panel that shows the scene as a tree of entities.
///
/// Clicking selects (Ctrl-click adds to or removes from the selection), dragging an
/// entity onto another makes it a child, dragging it onto the empty space below makes it
/// a root, and the context menu creates, duplicates and deletes.
///
/// @code
/// HierarchyPanel hierarchy;
/// hierarchy.draw(context);        // once per frame, between the interface's begin and end
/// @endcode

#include "cordite/editor/EditorContext.h"

#include <functional>

namespace cordite {

class HierarchyPanel {
public:
    /// Draws the panel as its own window.
    void draw(EditorContext& context);

private:
    void drawEntity(EditorContext& context, Entity entity);

    /// An edit requested while the tree was being drawn. It runs after the tree is
    /// finished, because it changes what the drawing code is walking over.
    std::function<void()> m_deferredEdit;
};

} // namespace cordite
