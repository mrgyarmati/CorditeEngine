#include "cordite/assets/Prefab.h"

#include "cordite/core/Document.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

constexpr DocumentFormat kPrefabFormat{.name = "cordite.prefab", .version = kPrefabFormatVersion, .kind = "prefab"};
constexpr std::array<std::string_view, 4> kPrefabMembers = {"format", "version", "name", "entities"};

} // namespace

Result<PrefabData> parsePrefab(const Json& document) {
    CORDITE_TRY(readDocumentHeader(document, kPrefabFormat));
    CORDITE_TRY(rejectUnknownMembers(document, kPrefabMembers));
    CORDITE_TRY_ASSIGN(std::optional<std::string> name, jsonGetOptional<std::string>(document, "name"));

    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array()) {
        return makeError(ErrorCode::ParseError, "the prefab has no 'entities' array");
    }
    if (entities->empty()) {
        return makeError(ErrorCode::ParseError, "the prefab has no entities; it needs one at least");
    }
    if (entities->size() > kMaxPrefabEntities) {
        return makeError(ErrorCode::ParseError, "the prefab has {} entities; it may have {} at most", entities->size(),
                         kMaxPrefabEntities);
    }
    for (size_t index = 0; index < entities->size(); ++index) {
        if (!(*entities)[index].is_object()) {
            return makeError(ErrorCode::ParseError, "entity #{} must be a JSON object, found {}", index,
                             jsonTypeName((*entities)[index]));
        }
    }
    return PrefabData{.name = std::move(name).value_or(std::string()), .entities = *entities};
}

Json prefabDocument(const PrefabData& prefab) {
    Json document = makeDocument(kPrefabFormat);
    document["name"] = prefab.name;
    document["entities"] = prefab.entities;
    return document;
}

} // namespace cordite
