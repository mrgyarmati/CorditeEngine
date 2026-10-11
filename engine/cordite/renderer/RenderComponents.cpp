#include "cordite/renderer/RenderComponents.h"

#include "cordite/core/Reflection.h"

namespace cordite {
namespace {

TypeInfo describeMeshRenderer() {
    return TypeBuilder<MeshRendererComponent>("MeshRenderer")
        .property(
            "mesh", &MeshRendererComponent::mesh,
            {.description = "The mesh to draw", .semantic = PropertySemantic::AssetReference, .referenceType = "Mesh"})
        .property("materials", &MeshRendererComponent::materials,
                  {.description = "One material per submesh of the mesh; missing or empty entries use the default "
                                  "material",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Material"})
        .build();
}

} // namespace

Status registerRendererComponents(ComponentRegistry& registry) {
    return registry.add<MeshRendererComponent>(describeMeshRenderer(),
                                               {.description = "Draws a mesh at the entity's place in the world."});
}

} // namespace cordite
