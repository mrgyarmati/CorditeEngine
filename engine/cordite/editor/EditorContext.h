#pragma once

/// @file
/// The document the editor is working on: a world, the file it belongs to, what is
/// selected in it and the history of edits made to it.
///
/// It is the part of the editor that has no user interface. Panels draw it, commands
/// change it, and tests exercise it without a window or a GPU.
///
/// @code
/// World world;
/// ComponentRegistry components;
/// CORDITE_TRY(registerWorldComponents(components));
/// EditorContext context(world, components);
///
/// CORDITE_TRY(context.openScene("scenes/depot.scene.json"));
/// CORDITE_TRY_ASSIGN(const Entity lamp, context.resolveEntity("Room/Table/Lamp"));
/// const Entity selected[] = {lamp};
/// context.setSelection(selected);
/// ...
/// if (context.hasUnsavedChanges()) {
///     CORDITE_TRY(context.saveScene());
/// }
/// @endcode
///
/// Changes that should be undoable go through EditorActions, which records them in
/// history(); only those count as unsaved changes.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/UndoStack.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

class EditorContext {
public:
    /// Both must outlive the context.
    EditorContext(World& world, const ComponentRegistry& components);
    ~EditorContext() = default;
    CORDITE_NON_MOVABLE(EditorContext);

    [[nodiscard]] World& world() { return m_world; }

    [[nodiscard]] const World& world() const { return m_world; }

    [[nodiscard]] const ComponentRegistry& components() const { return m_components; }

    [[nodiscard]] UndoStack& history() { return m_history; }

    [[nodiscard]] const UndoStack& history() const { return m_history; }

    /// The selected entities in the order they were selected. Entities that have been
    /// destroyed since are left out.
    [[nodiscard]] std::vector<Entity> selection() const;

    /// The entity selected last: the one the inspector shows. Null when nothing is.
    [[nodiscard]] Entity primarySelection() const;
    [[nodiscard]] bool isSelected(Entity entity) const;

    /// Replaces the selection. Invalid handles and repetitions are dropped.
    void setSelection(std::span<const Entity> entities);

    void clearSelection() { m_selection.clear(); }

    /// Adds an entity to the selection, or removes it if it is selected already.
    void toggleSelection(Entity entity);

    /// Finds an entity from the text a person or an agent uses to refer to it:
    /// - an id: "5f0c7c1e-...";
    /// - a path of names from a root, separated by slashes: "Room/Table/Lamp", or
    ///   "/Room" for a root;
    /// - a name: "Lamp".
    /// Fails with NotFound when nothing matches, and with InvalidArgument when a name or a
    /// path matches more than one entity, since guessing would edit the wrong one.
    [[nodiscard]] Result<Entity> resolveEntity(std::string_view reference) const;

    /// The path of names leading to an entity: "Room/Table/Lamp". The entity must be valid.
    [[nodiscard]] std::string entityPath(Entity entity) const;

    /// The file the scene was loaded from or last saved to; empty for a new scene.
    [[nodiscard]] const std::filesystem::path& scenePath() const { return m_scenePath; }

    /// Replaces the scene with an empty one. The history and the selection are cleared.
    void newScene();

    /// Replaces the scene with the one in a file. On failure nothing changes.
    [[nodiscard]] Status openScene(const std::filesystem::path& path);

    /// Saves to `path`, which becomes the scene's file; an empty path saves to the
    /// current file. Fails with InvalidState when the scene has no file yet.
    [[nodiscard]] Status saveScene(const std::filesystem::path& path = {});

    /// True when edits were made, or undone, since the scene was last saved or loaded.
    [[nodiscard]] bool hasUnsavedChanges() const { return m_history.stateId() != m_savedStateId; }

private:
    void resetDocumentState();

    World& m_world;
    const ComponentRegistry& m_components;
    UndoStack m_history;
    /// By id, so that the selection survives entities being destroyed and restored.
    std::vector<Uuid> m_selection;
    std::filesystem::path m_scenePath;
    uint64_t m_savedStateId = 0;
};

} // namespace cordite
