#include "cordite/engine/SceneAssets.h"

#include "cordite/assets/Prefab.h"
#include "cordite/core/Document.h"
#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/StringUtils.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include <optional>
#include <unordered_set>
#include <utility>

namespace cordite {
namespace {

/// Finds the asset references in the JSON form of a reflected object and hands each to
/// `visit` with the name of the property it is in.
template <typename Visit>
void visitAssetReferences(const TypeInfo& type, const Json& value, const std::string& prefix, const Visit& visit) {
    if (!value.is_object()) {
        return;
    }
    for (const PropertyInfo& property : type.properties()) {
        const auto member = value.find(property.name());
        if (member == value.end()) {
            continue;
        }
        const std::string name = prefix + property.name();

        if (property.type() == PropertyType::Object && property.objectType() != nullptr) {
            visitAssetReferences(*property.objectType(), *member, name + ".", visit);
            continue;
        }
        if (property.attributes().semantic != PropertySemantic::AssetReference) {
            continue;
        }

        const auto report = [&](const Json& element, const std::string& elementName) {
            if (!element.is_string()) {
                return;
            }
            if (const std::optional<Uuid> id = Uuid::parse(element.get<std::string>()); id && !id->isNil()) {
                visit(elementName, *id, property.attributes().referenceType);
            }
        };
        if (member->is_array()) {
            for (size_t index = 0; index < member->size(); ++index) {
                report((*member)[index], fmt::format("{}[{}]", name, index));
            }
        } else {
            report(*member, name);
        }
    }
}

} // namespace

std::vector<AssetReferenceUse> collectAssetReferences(const World& world, const ComponentRegistry& components) {
    std::vector<AssetReferenceUse> uses;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        for (const ComponentDescriptor* descriptor : components.components()) {
            const void* instance = descriptor->getConst(world.registry(), entity.handle);
            if (instance == nullptr) {
                continue;
            }
            // Through JSON, because that is the one form every property can take.
            visitAssetReferences(descriptor->type, descriptor->type.toJson(instance), "",
                                 [&](const std::string& property, const Uuid& asset, const std::string& expected) {
                                     uses.push_back({entity, descriptor->type.name(), property, asset, expected});
                                 });
        }
    });
    return uses;
}

Result<std::vector<Uuid>> assetsReferencedBy(AssetType type, const Json& document,
                                             const ComponentRegistry& components) {
    // Both hold their entities as serializeEntities() writes them (SceneSerializer.h).
    Json prefabEntities;
    const Json* entities = nullptr;
    if (type == AssetType::Scene) {
        CORDITE_TRY(readDocumentHeader(document, kSceneFormat));
        const auto found = document.find("entities");
        if (found == document.end() || !found->is_array()) {
            return makeError(ErrorCode::ParseError, "the scene has no 'entities' array");
        }
        entities = &*found;
    } else if (type == AssetType::Prefab) {
        CORDITE_TRY_ASSIGN(PrefabData prefab, parsePrefab(document));
        prefabEntities = std::move(prefab.entities);
        entities = &prefabEntities;
    } else {
        return makeError(ErrorCode::InvalidArgument, "{} has no entities to refer to assets with", withArticle(type));
    }

    std::vector<Uuid> used;
    std::unordered_set<Uuid> seen;
    const auto use = [&](const std::string& /*property*/, const Uuid& asset, const std::string& /*expected*/) {
        if (seen.insert(asset).second) {
            used.push_back(asset);
        }
    };
    for (size_t index = 0; index < entities->size(); ++index) {
        const Json& entity = (*entities)[index];
        // What is no object finds nothing.
        const auto held = entity.find("components");
        if (held == entity.end() || !held->is_object()) {
            continue;
        }
        for (const auto& component : held->items()) {
            // The root of a prefab is the first of its entities; once placed, it is marked
            // as an instance of the prefab, whatever its file says.
            if (type == AssetType::Prefab && index == 0 && component.key() == PrefabInstanceComponent::kName) {
                continue;
            }
            if (const ComponentDescriptor* descriptor = components.find(component.key())) {
                visitAssetReferences(descriptor->type, component.value(), "", use);
            }
        }
    }
    return used;
}

AssetReferenceReader assetReferenceReader(const ComponentRegistry& components) {
    return
        [&components](AssetType type, const Json& document) { return assetsReferencedBy(type, document, components); };
}

std::vector<SceneProblem> findSceneProblems(const World& world, const ComponentRegistry& components,
                                            const AssetDatabase& assets) {
    std::vector<SceneProblem> problems;
    for (const AssetReferenceUse& use : collectAssetReferences(world, components)) {
        const AssetInfo* asset = assets.find(use.asset);
        if (asset == nullptr) {
            problems.push_back({use.entity, fmt::format("{}.{} refers to the asset {}, which does not exist",
                                                        use.component, use.property, use.asset.toString())});
        } else if (!use.expectedType.empty() && !equalsIgnoreCase(use.expectedType, nameOf(asset->type))) {
            // A kind that no asset has - a component that was described wrongly - is named
            // as the component spells it.
            const std::optional<AssetType> expected = assetTypeFromName(use.expectedType);
            const std::string wanted =
                expected ? std::string(withArticle(*expected)) : fmt::format("a {}", toLowerAscii(use.expectedType));
            problems.push_back(
                {use.entity, fmt::format("{}.{} refers to {}, which is {}, not {}", use.component, use.property,
                                         asset->reference(), withArticle(asset->type), wanted)});
        }
    }
    for (PhysicsProblem& problem : findPhysicsProblems(world)) {
        problems.push_back({problem.entity, std::move(problem.message)});
    }
    return problems;
}

} // namespace cordite
