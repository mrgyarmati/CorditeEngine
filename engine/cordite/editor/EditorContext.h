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
///
/// A scene can be played where it is edited (PlayMode). beginPlay() remembers the scene
/// and sets its history aside; endPlay() puts both back. In between, the world is a
/// game that runs: whatever changes it then - the game itself, or an edit - is gone when
/// playing ends, and such edits have a history of their own that goes with them.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/UndoStack.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

class EditorContext {
public:
    /// All three must outlive the context.
    EditorContext(World& world, const ComponentRegistry& components, AssetManager& assets);
    ~EditorContext() = default;
    CORDITE_NON_MOVABLE(EditorContext);

    [[nodiscard]] World& world() { return m_world; }

    [[nodiscard]] const World& world() const { return m_world; }

    [[nodiscard]] const ComponentRegistry& components() const { return m_components; }

    /// The assets the scene can use.
    [[nodiscard]] AssetManager& assets() { return m_assets; }

    [[nodiscard]] const AssetManager& assets() const { return m_assets; }

    [[nodiscard]] UndoStack& history() { return m_history; }

    [[nodiscard]] const UndoStack& history() const { return m_history; }

    /// The selected entities in the order they were selected. Entities that have been
    /// destroyed since are left out.
    [[nodiscard]] std::vector<Entity> selection() const;

    /// The entity selected last: the one the inspector shows. Null when nothing is.
    [[nodiscard]] Entity primarySelection() const;
    [[nodiscard]] bool isSelected(Entity entity) const;

    /// What was copied last (EditorActions::copyEntities): the entities, with everything
    /// below them, as serializeEntities writes them - or null when nothing was. A copy, not
    /// a reference: it outlives what it was taken from, and the scene, and playing.
    [[nodiscard]] const Json& clipboard() const { return m_clipboard; }

    void setClipboard(Json entities) { m_clipboard = std::move(entities); }

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

    /// Finds an asset from the text a person or an agent uses to refer to it: its id, or
    /// its reference - the path of its file within the asset directory, followed by `#`
    /// and its place in the file for an asset inside another, as in
    /// "models/crate.gltf#mesh/0" (see AssetInfo::reference). Fails with NotFound when
    /// nothing matches.
    [[nodiscard]] Result<const AssetInfo*> resolveAsset(std::string_view reference) const;

    /// The file the scene was loaded from or last saved to; empty for a new scene.
    [[nodiscard]] const std::filesystem::path& scenePath() const { return m_scenePath; }

    /// Replaces the scene with an empty one. The history and the selection are cleared.
    /// Fails with InvalidState while the scene is being played, and changes nothing then.
    [[nodiscard]] Status newScene();

    /// Replaces the scene with the one in a file. On failure nothing changes. Fails with
    /// InvalidState while the scene is being played.
    [[nodiscard]] Status openScene(const std::filesystem::path& path);

    /// Saves to `path`, which becomes the scene's file; an empty path saves to the
    /// current file. Fails with InvalidState when the scene has no file yet, and while
    /// it is being played: what would be written is a game half-way through. A file
    /// among the assets is then read again for what it uses
    /// (AssetManager::rereadDependencies()), and one that is new to them - a scene saved
    /// under another name - makes them refresh, so that it is listed at once.
    [[nodiscard]] Status saveScene(const std::filesystem::path& path = {});

    /// Says that the scene's file is `path` from now on - where it went when it was moved
    /// (moveAsset()). Nothing is read or written, and what counts as saved stays.
    void setScenePath(std::filesystem::path path) { m_scenePath = std::move(path); }

    /// The scene as saving it would write it (serializeScene): the world, or - while it is
    /// played - the scene as beginPlay() found it, which is what stays when playing ends.
    [[nodiscard]] Json documentScene() const;

    /// Replaces the scene with `scene`, a document as documentScene() gives one, whose file
    /// is `path` (empty for none): what the editor kept of a scene it could not go on with
    /// (SceneRecovery.h). Unlike openScene() it is changes that are not saved - the very
    /// changes that were kept - until it is saved. The history and the selection are
    /// cleared. Fails, changing nothing, while the scene is played, and as
    /// deserializeScene() fails.
    [[nodiscard]] Status restoreScene(const Json& scene, std::filesystem::path path);

    /// True when edits were made, or undone, since the scene was last saved or loaded.
    /// What is edited while the scene is played does not count: it does not stay.
    [[nodiscard]] bool hasUnsavedChanges() const;

    /// How many times the scene was written to a file (saveScene()): what watches the files
    /// of the assets takes those writes for no change of them.
    [[nodiscard]] uint64_t saveCount() const { return m_saveCount; }

    /// Whether the scene is being played: between beginPlay() and endPlay().
    [[nodiscard]] bool isPlaying() const { return m_play.has_value(); }

    /// Begins to play the scene: remembers it as it is, to the byte, and sets its
    /// history aside. From here on history() is another one, empty, for what is edited
    /// while the game runs. The selection stays. Fails with InvalidState when the scene
    /// is being played already.
    [[nodiscard]] Status beginPlay();

    /// Ends playing: the world holds the scene that beginPlay() remembered, and the
    /// history and what counts as saved are what they were. Of the selection, what is
    /// still there stays selected. Whoever ran the scene ends what runs first, while the
    /// world it ran in is still there (Engine::endScene). Fails with InvalidState when
    /// the scene is not being played.
    [[nodiscard]] Status endPlay();

private:
    /// What is kept of the document while its scene is played. What counts as saved
    /// needs no keeping: nothing is saved while a scene plays.
    struct PlayedDocument {
        /// The scene as beginPlay() found it (serializeScene).
        Json scene;
        UndoStack history;
    };

    void resetDocumentState();
    /// Tells the assets that a scene file was written: see saveScene().
    [[nodiscard]] Status noteSavedScene(const std::filesystem::path& file);

    World& m_world;
    const ComponentRegistry& m_components;
    AssetManager& m_assets;
    UndoStack m_history;
    /// By id, so that the selection survives entities being destroyed and restored.
    std::vector<Uuid> m_selection;
    Json m_clipboard;
    std::filesystem::path m_scenePath;
    /// What m_savedStateId is when no state of the history is saved: what was restored or
    /// brought up to date has changes that were not. No history gets there - its states are
    /// counted up from nothing, one an edit.
    static constexpr uint64_t kNotSaved = std::numeric_limits<uint64_t>::max();
    uint64_t m_savedStateId = 0;
    uint64_t m_saveCount = 0;
    std::optional<PlayedDocument> m_play;
};

} // namespace cordite
