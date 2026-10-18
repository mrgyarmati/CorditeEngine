#include "cordite/editor/EditorContext.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/world/EntityPath.h"
#include "cordite/world/SceneSerializer.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace cordite {

EditorContext::EditorContext(World& world, const ComponentRegistry& components, AssetManager& assets)
    : m_world(world), m_components(components), m_assets(assets), m_savedStateId(m_history.stateId()) {}

std::vector<Entity> EditorContext::selection() const {
    std::vector<Entity> entities;
    entities.reserve(m_selection.size());
    for (const Uuid& id : m_selection) {
        if (const Entity entity = m_world.findById(id); !entity.isNull()) {
            entities.push_back(entity);
        }
    }
    return entities;
}

Entity EditorContext::primarySelection() const {
    for (const Uuid& id : m_selection | std::views::reverse) {
        if (const Entity entity = m_world.findById(id); !entity.isNull()) {
            return entity;
        }
    }
    return Entity{};
}

bool EditorContext::isSelected(Entity entity) const {
    return m_world.isValid(entity) && std::ranges::find(m_selection, m_world.id(entity)) != m_selection.end();
}

void EditorContext::setSelection(std::span<const Entity> entities) {
    m_selection.clear();
    for (const Entity entity : entities) {
        if (m_world.isValid(entity) && !isSelected(entity)) {
            m_selection.push_back(m_world.id(entity));
        }
    }
}

void EditorContext::toggleSelection(Entity entity) {
    if (!m_world.isValid(entity)) {
        return;
    }
    const Uuid& id = m_world.id(entity);
    if (std::erase(m_selection, id) == 0) {
        m_selection.push_back(id);
    }
}

Result<const AssetInfo*> EditorContext::resolveAsset(std::string_view reference) const {
    const std::string_view trimmed = trim(reference);
    if (trimmed.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an asset reference must not be empty");
    }

    const AssetDatabase& database = m_assets.database();
    if (const std::optional<Uuid> id = Uuid::parse(trimmed)) {
        if (const AssetInfo* asset = database.find(*id)) {
            return asset;
        }
        return makeError(ErrorCode::NotFound, "no asset has the id {}", id->toString());
    }
    if (const AssetInfo* asset = database.findByReference(trimmed)) {
        return asset;
    }
    return makeError(ErrorCode::NotFound,
                     "there is no asset '{}'; asset.list shows the ones that exist and how to refer to them", trimmed);
}

Result<Entity> EditorContext::resolveEntity(std::string_view reference) const {
    const std::string_view trimmed = trim(reference);
    if (trimmed.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an entity reference must not be empty");
    }

    if (const std::optional<Uuid> id = Uuid::parse(trimmed)) {
        const Entity entity = m_world.findById(*id);
        if (entity.isNull()) {
            return makeError(ErrorCode::NotFound, "no entity has the id {}", id->toString());
        }
        return entity;
    }

    if (trimmed.find('/') == std::string_view::npos) {
        std::vector<Entity> matches;
        m_world.traverse([&](Entity entity, uint32_t /*depth*/) {
            if (m_world.name(entity) == trimmed) {
                matches.push_back(entity);
            }
        });
        if (matches.empty()) {
            return makeError(ErrorCode::NotFound, "no entity is named '{}'", trimmed);
        }
        if (matches.size() > 1) {
            return makeError(ErrorCode::InvalidArgument,
                             "{} entities are named '{}': {}. Refer to one of them by path or by id.", matches.size(),
                             trimmed, describeEntities(m_world, matches));
        }
        return matches.front();
    }

    // A path: every part names a child of the entity found for the part before it.
    return findEntityByPath(m_world, trimmed);
}

std::string EditorContext::entityPath(Entity entity) const {
    return cordite::entityPath(m_world, entity);
}

void EditorContext::resetDocumentState() {
    m_history.clear();
    m_selection.clear();
    m_savedStateId = m_history.stateId();
}

bool EditorContext::hasUnsavedChanges() const {
    // Of the history of the scene, which is set aside while the scene is played.
    return (m_play ? m_play->history : m_history).stateId() != m_savedStateId;
}

Status EditorContext::beginPlay() {
    if (m_play) {
        return makeError(ErrorCode::InvalidState, "the scene is being played already");
    }
    m_play = PlayedDocument{.scene = serializeScene(m_world, m_components),
                            .history = std::exchange(m_history, UndoStack())};
    return {};
}

Status EditorContext::endPlay() {
    if (!m_play) {
        return makeError(ErrorCode::InvalidState, "the scene is not being played");
    }
    PlayedDocument played = std::move(*m_play);
    m_play.reset();
    m_history = std::move(played.history);
    // It was this world a moment ago, written by the function that now reads it: a
    // failure here is not expected, and leaves the document as it should be with a
    // world that says what went wrong.
    return deserializeScene(m_world, played.scene, m_components);
}

Status EditorContext::newScene() {
    if (m_play) {
        return makeError(ErrorCode::InvalidState, "the scene is being played: stop it before starting a new one");
    }
    m_world.clear();
    m_world.setSceneName("Untitled");
    m_scenePath.clear();
    resetDocumentState();
    return {};
}

Status EditorContext::openScene(const std::filesystem::path& path) {
    if (m_play) {
        return makeError(ErrorCode::InvalidState, "the scene is being played: stop it before opening another");
    }
    CORDITE_TRY(loadScene(path, m_world, m_components));
    m_scenePath = path;
    resetDocumentState();
    return {};
}

Status EditorContext::saveScene(const std::filesystem::path& path) {
    if (m_play) {
        return makeError(ErrorCode::InvalidState,
                         "the scene is being played: stop it before saving, or a game half-way through is "
                         "what would be saved");
    }
    const std::filesystem::path& target = path.empty() ? m_scenePath : path;
    if (target.empty()) {
        return makeError(ErrorCode::InvalidState, "the scene has no file yet; say where to save it");
    }

    if (const std::filesystem::path directory = target.parent_path(); !directory.empty()) {
        CORDITE_TRY(ensureDirectory(directory));
    }
    CORDITE_TRY(cordite::saveScene(target, m_world, m_components));
    m_scenePath = target;
    m_savedStateId = m_history.stateId();
    ++m_saveCount;

    // The scene is saved, whatever the assets make of it: what went wrong there is said,
    // and is not a failure of the save.
    if (const Status noted = noteSavedScene(target); !noted) {
        CORDITE_LOG_WARN("Editor", "the scene was saved, but the assets do not know what it uses now: {}",
                         noted.error().describe());
    }
    return {};
}

Status EditorContext::noteSavedScene(const std::filesystem::path& file) {
    const AssetDatabase& database = m_assets.database();
    if (database.rootDirectory().empty()) {
        return {};
    }
    const Result<std::string> reference = relativePathWithin(database.rootDirectory(), file);
    if (!reference || assetTypeOfFile(*reference) != AssetType::Scene) {
        // Saved where no asset is, or as what is no scene to the assets.
        return {};
    }
    if (const AssetInfo* asset = database.findByReference(*reference)) {
        return m_assets.rereadDependencies(asset->id);
    }
    return m_assets.refresh();
}

} // namespace cordite
