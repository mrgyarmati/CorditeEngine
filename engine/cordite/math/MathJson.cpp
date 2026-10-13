#include "cordite/math/MathJson.h"

#include "cordite/core/Json.h"

#include <array>
#include <cmath>
#include <limits>

namespace cordite {
namespace {

template <size_t N>
Status readNumbers(const Json& json, std::array<float, N>& out) {
    if (!json.is_array() || json.size() != N) {
        return makeError(ErrorCode::ParseError, "expected an array of {} numbers, found {}", N, jsonTypeName(json));
    }

    std::array<float, N> values{};
    for (size_t index = 0; index < N; ++index) {
        const Json& element = json[index];
        if (!element.is_number()) {
            return makeError(ErrorCode::ParseError, "element {} is {}, expected a number", index,
                             jsonTypeName(element));
        }
        // Range-check as double first: narrowing an out-of-range double to float is
        // undefined behavior.
        const double number = element.get<double>();
        if (!std::isfinite(number) || std::abs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
            return makeError(ErrorCode::ParseError, "element {} is not a finite single-precision number", index);
        }
        values[index] = static_cast<float>(number);
    }
    out = values;
    return {};
}

} // namespace

Json toJson(const Vec2& value) {
    return Json::array({jsonFromFloat(value.x), jsonFromFloat(value.y)});
}

Json toJson(const Vec3& value) {
    return Json::array({jsonFromFloat(value.x), jsonFromFloat(value.y), jsonFromFloat(value.z)});
}

Json toJson(const Vec4& value) {
    return Json::array(
        {jsonFromFloat(value.x), jsonFromFloat(value.y), jsonFromFloat(value.z), jsonFromFloat(value.w)});
}

Json toJson(const Quat& value) {
    return Json::array(
        {jsonFromFloat(value.x), jsonFromFloat(value.y), jsonFromFloat(value.z), jsonFromFloat(value.w)});
}

Json toJson(const Transform& value) {
    Json json = Json::object();
    json["position"] = toJson(value.position);
    json["rotation"] = toJson(value.rotation);
    json["scale"] = toJson(value.scale);
    return json;
}

Json toJson(const Aabb& value) {
    Json json = Json::object();
    json["min"] = toJson(value.min);
    json["max"] = toJson(value.max);
    return json;
}

Status fromJson(const Json& json, Vec2& out) {
    std::array<float, 2> values{};
    CORDITE_TRY(readNumbers(json, values));
    out = Vec2(values[0], values[1]);
    return {};
}

Status fromJson(const Json& json, Vec3& out) {
    std::array<float, 3> values{};
    CORDITE_TRY(readNumbers(json, values));
    out = Vec3(values[0], values[1], values[2]);
    return {};
}

Status fromJson(const Json& json, Vec4& out) {
    std::array<float, 4> values{};
    CORDITE_TRY(readNumbers(json, values));
    out = Vec4(values[0], values[1], values[2], values[3]);
    return {};
}

Status fromJson(const Json& json, Quat& out) {
    std::array<float, 4> values{};
    CORDITE_TRY(readNumbers(json, values));

    const Quat rotation(values[3], values[0], values[1], values[2]);
    if (!isRotation(rotation)) {
        // Finite numbers, so what is wrong is their size: none to speak of, or one whose
        // square is no number any more.
        return makeError(ErrorCode::ParseError, "a rotation quaternion must not be zero, and its numbers must not "
                                                "be too large to have a length");
    }

    // A quaternion that is already of unit length, as every saved one is, is kept bit for
    // bit: files must not change every time they are loaded and saved again.
    out = normalizeRotation(rotation);
    return {};
}

Status fromJson(const Json& json, Transform& out) {
    if (!json.is_object()) {
        return makeError(ErrorCode::ParseError, "expected a transform object, found {}", jsonTypeName(json));
    }

    Transform transform;
    if (const auto member = json.find("position"); member != json.end()) {
        CORDITE_TRY(fromJson(*member, transform.position));
    }
    if (const auto member = json.find("rotation"); member != json.end()) {
        CORDITE_TRY(fromJson(*member, transform.rotation));
    }
    if (const auto member = json.find("scale"); member != json.end()) {
        CORDITE_TRY(fromJson(*member, transform.scale));
    }
    out = transform;
    return {};
}

Status fromJson(const Json& json, Aabb& out) {
    if (!json.is_object() || !json.contains("min") || !json.contains("max")) {
        return makeError(ErrorCode::ParseError, "expected an object with 'min' and 'max'");
    }

    Aabb box;
    CORDITE_TRY(fromJson(json["min"], box.min));
    CORDITE_TRY(fromJson(json["max"], box.max));
    out = box;
    return {};
}

} // namespace cordite
