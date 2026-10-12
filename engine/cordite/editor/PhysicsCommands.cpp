#include "cordite/editor/PhysicsCommands.h"

#include "cordite/core/Json.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/MathJson.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

using Type = CommandParameterType;

/// How far a ray is followed when nobody says: across any scene.
constexpr double kDefaultRayLength = 10000.0;
/// The longest a ray may be, and the largest a ball: beyond any scene, and well within
/// what the numbers of the simulation can tell apart.
constexpr double kLongestRay = 1000000.0;

/// The least a length may be.
enum class Least : uint8_t {
    Nothing,
    MoreThanNothing,
};

/// A parameter that is a length: no less than it may be, and no more than the longest
/// there is. Checked as it was given, before it is made the number of fewer digits
/// that the simulation works with.
Result<float> lengthParameter(const Json& parameters, const char* name, double fallback, Least least) {
    const auto length = jsonGetOr<double>(parameters, name, fallback);
    const bool isEnough = least == Least::Nothing ? length >= 0.0 : length > 0.0;
    if (!isEnough || length > kLongestRay) {
        return makeError(ErrorCode::OutOfRange, "'{}' must be {} and no more than {}, got {}", name,
                         least == Least::Nothing ? "nothing or more" : "more than nothing", kLongestRay, length);
    }
    return static_cast<float>(length);
}

/// A parameter that is a point or a direction: three numbers.
Result<Vec3> vectorParameter(const Json& parameters, const char* name) {
    Vec3 value(0.0f);
    if (!fromJson(parameters.at(name), value)) {
        return makeError(ErrorCode::InvalidArgument, "'{}' must be an array of 3 numbers", name);
    }
    return value;
}

/// The entity a body belongs to, as an answer names it.
Json describeBody(const EditorContext& context, const PhysicsSystem& physics, BodyId body) {
    Json described = Json::object();
    const Entity entity = physics.entityOf(body);
    // A body that the system has not made - somebody else's, in the same simulation -
    // has no entity to name.
    if (context.world().isValid(entity)) {
        described["entity"] = context.entityPath(entity);
        described["id"] = context.world().id(entity).toString();
    } else {
        described["entity"] = nullptr;
        described["id"] = nullptr;
    }
    return described;
}

Result<Json> castRay(EditorContext& context, PhysicsSystem& physics, const Json& parameters) {
    CORDITE_TRY_ASSIGN(const Vec3 origin, vectorParameter(parameters, "origin"));
    CORDITE_TRY_ASSIGN(const Vec3 direction, vectorParameter(parameters, "direction"));
    if (glm::length(direction) <= kEpsilon) {
        return makeError(ErrorCode::InvalidArgument, "'direction' has no length, and so points nowhere");
    }
    CORDITE_TRY_ASSIGN(const float maxDistance,
                       lengthParameter(parameters, "maxDistance", kDefaultRayLength, Least::MoreThanNothing));
    CORDITE_TRY_ASSIGN(const float radius, lengthParameter(parameters, "radius", 0.0, Least::Nothing));

    physics.synchronize(context.world(), context.assets());
    const Ray ray{origin, direction};
    const std::optional<RayHit> hit = radius > 0.0f ? physics.simulation().sphereCast(ray, radius, maxDistance)
                                                    : physics.simulation().raycast(ray, maxDistance);
    if (!hit) {
        Json missed = Json::object();
        missed["hit"] = false;
        return missed;
    }
    Json found = describeBody(context, physics, hit->body);
    found["hit"] = true;
    found["distance"] = jsonFromFloat(hit->distance);
    found["position"] = toJson(hit->position);
    found["normal"] = toJson(hit->normal);
    return found;
}

Result<Json> findOverlaps(EditorContext& context, PhysicsSystem& physics, const Json& parameters) {
    CORDITE_TRY_ASSIGN(const Vec3 center, vectorParameter(parameters, "center"));
    const bool isBall = parameters.contains("radius");
    if (isBall == parameters.contains("size")) {
        return makeError(ErrorCode::InvalidArgument,
                         "give either 'radius', for a ball, or 'size', for a box - one of them, and not both");
    }
    if (isBall && parameters.contains("rotation")) {
        return makeError(ErrorCode::InvalidArgument, "'rotation' turns a box; a ball is the same however it is turned");
    }

    std::vector<BodyId> bodies;
    if (isBall) {
        CORDITE_TRY_ASSIGN(const float radius, lengthParameter(parameters, "radius", 0.0, Least::MoreThanNothing));
        physics.synchronize(context.world(), context.assets());
        bodies = physics.simulation().overlapSphere(center, radius);
    } else {
        CORDITE_TRY_ASSIGN(const Vec3 size, vectorParameter(parameters, "size"));
        if (!(std::min({size.x, size.y, size.z}) > 0.0f) || std::max({size.x, size.y, size.z}) > kLongestRay) {
            return makeError(ErrorCode::OutOfRange,
                             "'size' must be more than nothing and no more than {} along every axis, got {} {} {}",
                             kLongestRay, size.x, size.y, size.z);
        }
        Quat rotation = kIdentityQuat;
        if (parameters.contains("rotation") && !fromJson(parameters.at("rotation"), rotation)) {
            return makeError(ErrorCode::InvalidArgument,
                             "'rotation' must be a quaternion: an array of 4 numbers [x, y, z, w] that are not all 0");
        }
        physics.synchronize(context.world(), context.assets());
        bodies = physics.simulation().overlapBox(center, size, rotation);
    }

    Json entities = Json::array();
    for (const BodyId body : bodies) {
        entities.push_back(describeBody(context, physics, body));
    }
    Json found = Json::object();
    found["entities"] = std::move(entities);
    return found;
}

} // namespace

Status registerPhysicsCommands(CommandRegistry& commands, EditorContext& context, PhysicsSystem& physics) {
    CORDITE_TRY(commands.add({
        .name = "physics.raycast",
        .description = "Says what a ray hits first: the entity whose body it is, how far along the ray, the point "
                       "and the direction the surface faces there. With a `radius`, what a ball of that size "
                       "touches first when its middle is moved along the ray. Returns {\"hit\": false} for "
                       "nothing. The bodies are those of the scene as it is; no time passes.",
        .parameters = {{"origin", Type::Array, "Where the ray starts: [x, y, z] in metres."},
                       {"direction", Type::Array, "The way it goes: [x, y, z], of any length but none."},
                       {"maxDistance", Type::Number,
                        "How far it is followed, in metres: at most 1000000. Defaults to 10000.", false},
                       {"radius", Type::Number,
                        "The radius of a ball to move along the ray instead, in metres. Defaults to 0: a ray.", false}},
        .handler = [&context, &physics](const Json& parameters) { return castRay(context, physics, parameters); },
    }));

    return commands.add({
        .name = "physics.overlap",
        .description = "Lists the entities whose bodies reach into a ball (`radius`) or a box (`size`, and "
                       "`rotation`) around `center`, each once. Of a collider that is the triangles of a mesh, "
                       "the triangles count, not what they enclose. The bodies are those of the scene as it is; "
                       "no time passes.",
        .parameters = {{"center", Type::Array, "The middle of the ball or the box: [x, y, z] in metres."},
                       {"radius", Type::Number, "The radius of a ball, in metres.", false},
                       {"size", Type::Array, "The edge lengths of a box: [x, y, z] in metres.", false},
                       {"rotation", Type::Array,
                        "How the box is turned about its middle: a quaternion [x, y, z, w]. Defaults to not at all.",
                        false}},
        .handler = [&context, &physics](const Json& parameters) { return findOverlaps(context, physics, parameters); },
    });
}

} // namespace cordite
