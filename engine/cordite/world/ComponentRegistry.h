#pragma once

/// @file
/// The list of component types the engine knows by name.
///
/// Systems written in C++ use components through their types. Everything else - scene
/// files, the editor inspector, scripts, the automation API - refers to a component by
/// name and to its fields as reflected properties. The registry connects the two.
///
/// @code
/// ComponentRegistry components;
/// CORDITE_TRY(registerWorldComponents(components));
///
/// const ComponentDescriptor* camera = components.find("Camera");
/// void* instance = camera->add(world.registry(), entity.handle);
/// CORDITE_TRY(camera->type.fromJson(instance, json));
/// @endcode
///
/// A registry is an ordinary object, not a global: the engine owns one, and a test can
/// build its own with exactly the components it needs.

#include "cordite/core/Reflection.h"
#include "cordite/core/Result.h"
#include "cordite/world/Entt.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {

struct ComponentTraits {
    /// One sentence saying what the component is for. Shown in the editor and published
    /// in the automation schema.
    std::string description;
    /// False for components that every entity has from creation, such as Transform. They
    /// can be edited but neither added nor removed.
    bool optional = true;
};

/// A component type, with type-erased access to its instances. In all the functions,
/// `entity` must be valid.
struct ComponentDescriptor {
    /// The reflected description. `type.name()` is the name used in files and tools.
    TypeInfo type;
    ComponentTraits traits;

    bool (*has)(const entt::registry& registry, entt::entity entity) = nullptr;

    /// Return a pointer to the entity's instance, or nullptr when it has none.
    void* (*get)(entt::registry& registry, entt::entity entity) = nullptr;
    const void* (*getConst)(const entt::registry& registry, entt::entity entity) = nullptr;

    /// Returns the entity's instance, default-constructing it first if necessary.
    void* (*add)(entt::registry& registry, entt::entity entity) = nullptr;

    /// Gives the entity an instance made of default values with `patch` applied, replacing
    /// the one it has, if any. The component is complete before the entity gets it, so
    /// code listening for new components never sees a half-initialized one.
    void* (*emplace)(entt::registry& registry, entt::entity entity, const PropertyPatch& patch) = nullptr;

    /// Removes the instance if there is one.
    void (*remove)(entt::registry& registry, entt::entity entity) = nullptr;
};

class ComponentRegistry {
public:
    /// Registers component type T under the name of its TypeInfo. T must be
    /// default-constructible. Fails with InvalidArgument for an unnamed type and with
    /// AlreadyExists when the name is taken.
    template <typename T>
    Status add(TypeInfo type, ComponentTraits traits = {}) {
        auto descriptor = std::make_unique<ComponentDescriptor>();
        descriptor->type = std::move(type);
        descriptor->traits = std::move(traits);
        descriptor->has = [](const entt::registry& registry, entt::entity entity) {
            return registry.all_of<T>(entity);
        };
        descriptor->get = [](entt::registry& registry, entt::entity entity) -> void* {
            return registry.try_get<T>(entity);
        };
        descriptor->getConst = [](const entt::registry& registry, entt::entity entity) -> const void* {
            return registry.try_get<T>(entity);
        };
        descriptor->add = [](entt::registry& registry, entt::entity entity) -> void* {
            return &registry.get_or_emplace<T>(entity);
        };
        descriptor->emplace = [](entt::registry& registry, entt::entity entity, const PropertyPatch& patch) -> void* {
            T component{};
            patch.applyTo(&component);
            return &registry.emplace_or_replace<T>(entity, std::move(component));
        };
        descriptor->remove = [](entt::registry& registry, entt::entity entity) { registry.remove<T>(entity); };
        return insert(std::move(descriptor));
    }

    /// Returns the descriptor registered under `name`, or nullptr. The pointer stays
    /// valid for the lifetime of the registry.
    [[nodiscard]] const ComponentDescriptor* find(std::string_view name) const;

    /// Every registered component, in registration order.
    [[nodiscard]] std::span<const ComponentDescriptor* const> components() const { return m_ordered; }

private:
    Status insert(std::unique_ptr<ComponentDescriptor> descriptor);

    std::vector<std::unique_ptr<ComponentDescriptor>> m_owned;
    /// The same descriptors as plain pointers, for components().
    std::vector<const ComponentDescriptor*> m_ordered;
};

} // namespace cordite
