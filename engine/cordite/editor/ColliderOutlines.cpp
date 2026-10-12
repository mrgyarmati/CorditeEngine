#include "cordite/editor/ColliderOutlines.h"

namespace cordite {

const ColliderOutline* ColliderOutlines::find(const PhysicsSystem& physics, const Uuid& mesh, ShapeType shape) {
    const PhysicsWorld& simulation = physics.simulation();
    const GeometryId geometry = physics.geometryOf(mesh, shape);
    if (geometry.isNull()) {
        return nullptr;
    }
    if (const auto found = m_outlines.find(geometry); found != m_outlines.end()) {
        return &found->second;
    }
    // New geometry is what the physics makes when it has forgotten the old - the assets
    // were refreshed. The outlines of that are asked for by nobody again: an id is
    // never another's.
    std::erase_if(m_outlines, [&](const auto& entry) { return !simulation.isValid(entry.first); });
    ColliderOutline& outline = m_outlines[geometry];
    outline.edges = simulation.geometryEdges(geometry, kMaxColliderOutlineEdges);
    outline.bounds = simulation.geometryBounds(geometry);
    return &outline;
}

} // namespace cordite
