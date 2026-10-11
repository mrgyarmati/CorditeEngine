#include "cordite/math/MathReflection.h"

#include "cordite/math/MathJson.h"

namespace cordite {
namespace {

/// Reads a math type with the functions of MathJson.h. Those report malformed input as
/// a ParseError, which is right for files; for a property value the caller passed the
/// wrong thing, so the code becomes InvalidArgument like for every other codec.
template <typename T>
Status readProperty(const Json& json, T& out) {
    if (const Status status = cordite::fromJson(json, out); !status) {
        return makeError(ErrorCode::InvalidArgument, "{}", status.error().message);
    }
    return {};
}

} // namespace

// The qualified calls select the cordite::toJson overloads from MathJson.h instead of
// recursing into these member functions.

Json PropertyCodec<Vec2>::toJson(const Vec2& value) {
    return cordite::toJson(value);
}

Status PropertyCodec<Vec2>::fromJson(const Json& json, Vec2& out) {
    return readProperty(json, out);
}

Json PropertyCodec<Vec3>::toJson(const Vec3& value) {
    return cordite::toJson(value);
}

Status PropertyCodec<Vec3>::fromJson(const Json& json, Vec3& out) {
    return readProperty(json, out);
}

Json PropertyCodec<Vec4>::toJson(const Vec4& value) {
    return cordite::toJson(value);
}

Status PropertyCodec<Vec4>::fromJson(const Json& json, Vec4& out) {
    return readProperty(json, out);
}

Json PropertyCodec<Quat>::toJson(const Quat& value) {
    return cordite::toJson(value);
}

Status PropertyCodec<Quat>::fromJson(const Json& json, Quat& out) {
    return readProperty(json, out);
}

} // namespace cordite
