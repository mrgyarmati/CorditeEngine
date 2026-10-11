#include "cordite/editor/EditorContext.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/StringUtils.h"
#include "cordite/world/SceneSerializer.h"

#include <algorithm>
#include <ranges>

namespace cordite {
namespace {

/// How many matches an "ambiguous" error lists before it says "and N more".
constexpr size_t kMaxListedMatches = 4;

std::string describeMatches(const EditorContext& context, std::span<const Entity> matches) {
    std::string text;
    for (size_t index = 0; index < std::min(matches.size(), kMaxListedMatches); ++index) {
        text += fmt::format("{}{} ({})", index == 0 ? "" : ", ", context.entityPath(matches[index]),
                            context.world().id(matches[index]).toString());
    }
    if (matches.size() > kMaxListedMatches) {
        text += fmt::format(" and {} more", matches.size() - kMaxListedMatches);
    }
    return text;
}

/// The entities among `candidates` with the given name.
std::vector<Entity> withName(const World& world, std::span<const Entity> candidates, std::string_view name) {
    std::vector<Entity> matches;
    for (const Entity candidate : candidates) {
        if (world.name(candidate) == name) {
            matches.push_back(candidate);
        }
    }
    return matches;
}

} // namespace

EditorContext::EditorContext(World& world, const ComponentRegistry& components)
    : m_world(world), m_components(components), m_savedStateId(m_history.stateId()) {}

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
                             trimmed, describeMatches(*this, matches));
        }
        return matches.front();
    }

    // A path: every part names a child of the entity found for the part before it.
    Entity current;
    for (const std::string_view part : split(trimmed, '/')) {
        if (part.empty()) {
            // A leading slash, or a doubled one, adds nothing.
            continue;
        }
        const std::vector<Entity> candidates = m_world.children(current);
        const std::vector<Entity> matches = withName(m_world, candidates, part);
        if (matches.empty()) {
            return makeError(ErrorCode::NotFound, "'{}' in the path '{}' does not name {}", part, trimmed,
                             current.isNull() ? std::string("a root entity")
                                              : fmt::format("a child of '{}'", m_world.name(current)));
        }
        if (matches.size() > 1) {
            return makeError(ErrorCode::InvalidArgument,
                             "'{}' in the path '{}' matches {} entities: {}. Refer to one of them by id.", part,
                             trimmed, matches.size(), describeMatches(*this, matches));
        }
        current = matches.front();
    }
    if (current.isNull()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' does not name an entity", trimmed);
    }
    return current;
}

std::string EditorContext::entityPath(Entity entity) const {
    std::vector<std::string_view> names;
    for (Entity current = entity; !current.isNull(); current = m_world.parent(current)) {
        names.push_back(m_world.name(current));
    }

    std::string path;
    for (const std::string_view name : names | std::views::reverse) {
        path += path.empty() ? "" : "/";
        path += name;
    }
    return path;
}

void EditorContext::resetDocumentState() {
    m_history.clear();
    m_selection.clear();
    m_savedStateId = m_history.stateId();
}

void EditorContext::newScene() {
    m_world.clear();
    m_world.setSceneName("Untitled");
    m_scenePath.clear();
    resetDocumentState();
}

Status EditorContext::openScene(const std::filesystem::path& path) {
    CORDITE_TRY(loadScene(path, m_world, m_components));
    m_scenePath = path;
    resetDocumentState();
    return {};
}

Status EditorContext::saveScene(const std::filesystem::path& path) {
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
    return {};
}

} // namespace cordite
