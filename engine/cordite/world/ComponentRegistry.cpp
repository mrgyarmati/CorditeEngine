#include "cordite/world/ComponentRegistry.h"

#include <algorithm>

namespace cordite {

const ComponentDescriptor* ComponentRegistry::find(std::string_view name) const {
    const auto found = std::ranges::find_if(
        m_ordered, [name](const ComponentDescriptor* descriptor) { return descriptor->type.name() == name; });
    return found != m_ordered.end() ? *found : nullptr;
}

Status ComponentRegistry::insert(std::unique_ptr<ComponentDescriptor> descriptor) {
    const std::string& name = descriptor->type.name();
    if (name.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a component type needs a name");
    }
    if (find(name) != nullptr) {
        return makeError(ErrorCode::AlreadyExists, "a component named '{}' is already registered", name);
    }
    m_ordered.push_back(descriptor.get());
    m_owned.push_back(std::move(descriptor));
    return {};
}

} // namespace cordite
