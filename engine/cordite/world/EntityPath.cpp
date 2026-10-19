#include "cordite/world/EntityPath.h"

#include "cordite/core/StringUtils.h"

#include <fmt/format.h>

#include <algorithm>
#include <ranges>
#include <vector>

namespace cordite {
namespace {

/// How many entities a message lists before it says "and N more".
constexpr size_t kMaxListedEntities = 4;

} // namespace

std::string entityPath(const World& world, Entity entity) {
    std::vector<std::string_view> names;
    for (Entity current = entity; !current.isNull(); current = world.parent(current)) {
        names.push_back(world.name(current));
    }

    std::string path;
    for (const std::string_view name : names | std::views::reverse) {
        path += path.empty() ? "" : "/";
        path += name;
    }
    return path;
}

std::string entityNameFrom(std::string_view text, std::string_view fallback) {
    std::string name(text.empty() ? fallback : text);
    std::ranges::replace(name, '/', '_');
    return name;
}

std::string describeEntities(const World& world, std::span<const Entity> entities) {
    std::string text;
    for (size_t index = 0; index < std::min(entities.size(), kMaxListedEntities); ++index) {
        text += fmt::format("{}{} ({})", index == 0 ? "" : ", ", entityPath(world, entities[index]),
                            world.id(entities[index]).toString());
    }
    if (entities.size() > kMaxListedEntities) {
        text += fmt::format(" and {} more", entities.size() - kMaxListedEntities);
    }
    return text;
}

Result<Entity> findEntityByPath(const World& world, std::string_view path, Entity from) {
    if (!from.isNull() && !world.isValid(from)) {
        return makeError(ErrorCode::InvalidArgument, "the entity the path '{}' sets out from is gone", path);
    }
    Entity current = path.starts_with('/') ? Entity{} : from;
    for (const std::string_view part : split(path, '/')) {
        if (part.empty()) {
            continue;
        }
        std::vector<Entity> matches;
        for (const Entity child : world.children(current)) {
            if (world.name(child) == part) {
                matches.push_back(child);
            }
        }
        if (matches.empty()) {
            return makeError(ErrorCode::NotFound, "'{}' in the path '{}' does not name {}", part, path,
                             current.isNull() ? std::string("a root entity")
                                              : fmt::format("a child of '{}'", world.name(current)));
        }
        if (matches.size() > 1) {
            return makeError(ErrorCode::InvalidArgument, "'{}' in the path '{}' matches {} entities: {}", part, path,
                             matches.size(), describeEntities(world, matches));
        }
        current = matches.front();
    }
    if (current.isNull()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' does not name an entity", path);
    }
    return current;
}

} // namespace cordite
