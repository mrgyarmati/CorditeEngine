#include "cordite/core/Reflection.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// How many levels of arrays wrap the numbers in a value of the given type: none for a
/// scalar, one for a vector. Returns -1 for types that hold no numbers a range applies to.
int numberNestingOf(PropertyType type) {
    if (type == PropertyType::Int || type == PropertyType::Float) {
        return 0;
    }
    if (type == PropertyType::Vec2 || type == PropertyType::Vec3 || type == PropertyType::Vec4) {
        return 1;
    }
    return -1;
}

/// The same for a property, where a list adds one level to that of its elements.
int numberNestingOf(PropertyType type, PropertyType elementType) {
    if (type != PropertyType::List) {
        return numberNestingOf(type);
    }
    const int elementNesting = numberNestingOf(elementType);
    return elementNesting < 0 ? -1 : elementNesting + 1;
}

/// Checks a number against a property's declared range.
Status checkNumberInRange(double number, const PropertyAttributes& attributes) {
    const bool tooSmall = attributes.minimum && number < *attributes.minimum;
    const bool tooLarge = attributes.maximum && number > *attributes.maximum;
    if (!tooSmall && !tooLarge) {
        return {};
    }
    if (attributes.minimum && attributes.maximum) {
        return makeError(ErrorCode::OutOfRange, "{} is outside the allowed range [{}, {}]", number, *attributes.minimum,
                         *attributes.maximum);
    }
    if (tooSmall) {
        return makeError(ErrorCode::OutOfRange, "{} is below the minimum {}", number, *attributes.minimum);
    }
    return makeError(ErrorCode::OutOfRange, "{} is above the maximum {}", number, *attributes.maximum);
}

/// Checks every number of `value` against the range. `arrayLevels` is how many levels of
/// arrays wrap the numbers (see numberNestingOf). A value shaped differently is
/// malformed, which the codec reports with a better message; not descending into it also
/// keeps hostile nesting from exhausting the stack.
Status checkRange(const Json& value, const PropertyAttributes& attributes, int arrayLevels) {
    if (value.is_number()) {
        return checkNumberInRange(value.get<double>(), attributes);
    }
    if (value.is_array() && arrayLevels > 0) {
        for (const Json& element : value) {
            CORDITE_TRY(checkRange(element, attributes, arrayLevels - 1));
        }
    }
    return {};
}

std::string joinOptionNames(std::span<const EnumOption> options) {
    std::string names;
    for (const EnumOption& option : options) {
        names += names.empty() ? "" : ", ";
        names += option.name;
    }
    return names;
}

/// Converts a JSON number that was written with a fraction part ("2.0") to an integer.
Status wholeNumberFromFloat(double number, int64_t& out) {
    // 2^63: the first magnitude that no longer converts to int64_t.
    constexpr double kInt64Limit = 9223372036854775808.0;

    if (!std::isfinite(number) || std::trunc(number) != number) {
        return makeError(ErrorCode::InvalidArgument, "expected a whole number, found {}", number);
    }
    if (number < -kInt64Limit || number >= kInt64Limit) {
        return makeError(ErrorCode::OutOfRange, "{} does not fit the property's integer type", number);
    }
    out = static_cast<int64_t>(number);
    return {};
}

} // namespace

std::string_view nameOf(PropertyType type) {
    switch (type) {
        case PropertyType::Bool:
            return "bool";
        case PropertyType::Int:
            return "int";
        case PropertyType::Float:
            return "float";
        case PropertyType::String:
            return "string";
        case PropertyType::Vec2:
            return "vec2";
        case PropertyType::Vec3:
            return "vec3";
        case PropertyType::Vec4:
            return "vec4";
        case PropertyType::Quat:
            return "quat";
        case PropertyType::Uuid:
            return "uuid";
        case PropertyType::Enum:
            return "enum";
        case PropertyType::Object:
            return "object";
        case PropertyType::List:
            return "list";
    }
    return "unknown";
}

namespace detail {

Status integerFromJson(const Json& json, int64_t minimum, int64_t maximum, int64_t& out) {
    int64_t value = 0;
    if (json.is_number_unsigned()) {
        // Reading a value above INT64_MAX as signed would wrap around.
        const uint64_t magnitude = json.get<uint64_t>();
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return makeError(ErrorCode::OutOfRange, "{} does not fit the property's integer type", magnitude);
        }
        value = static_cast<int64_t>(magnitude);
    } else if (json.is_number_integer()) {
        value = json.get<int64_t>();
    } else if (json.is_number_float()) {
        CORDITE_TRY(wholeNumberFromFloat(json.get<double>(), value));
    } else {
        return makeError(ErrorCode::InvalidArgument, "expected an integer, found {}", jsonTypeName(json));
    }

    if (value < minimum || value > maximum) {
        return makeError(ErrorCode::OutOfRange, "{} does not fit the property's integer type", value);
    }
    out = value;
    return {};
}

Status unsignedFromJson(const Json& json, uint64_t maximum, uint64_t& out) {
    uint64_t value = 0;
    if (json.is_number_unsigned()) {
        value = json.get<uint64_t>();
    } else if (json.is_number_integer() || json.is_number_float()) {
        int64_t signedValue = 0;
        if (json.is_number_float()) {
            CORDITE_TRY(wholeNumberFromFloat(json.get<double>(), signedValue));
        } else {
            signedValue = json.get<int64_t>();
        }
        if (signedValue < 0) {
            return makeError(ErrorCode::OutOfRange, "{} must not be negative", signedValue);
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        return makeError(ErrorCode::InvalidArgument, "expected an integer, found {}", jsonTypeName(json));
    }

    if (value > maximum) {
        return makeError(ErrorCode::OutOfRange, "{} does not fit the property's integer type", value);
    }
    out = value;
    return {};
}

Status floatFromJson(const Json& json, double limit, double& out) {
    if (!json.is_number()) {
        return makeError(ErrorCode::InvalidArgument, "expected a number, found {}", jsonTypeName(json));
    }
    const double value = json.get<double>();
    if (!std::isfinite(value) || std::abs(value) > limit) {
        return makeError(ErrorCode::OutOfRange, "the number is not finite or is too large for the property");
    }
    out = value;
    return {};
}

Status expectArray(const Json& json) {
    if (!json.is_array()) {
        return makeError(ErrorCode::InvalidArgument, "expected an array, found {}", jsonTypeName(json));
    }
    return {};
}

} // namespace detail

Json PropertyCodec<bool>::toJson(const bool& value) {
    return Json(value);
}

Status PropertyCodec<bool>::fromJson(const Json& json, bool& out) {
    if (!json.is_boolean()) {
        return makeError(ErrorCode::InvalidArgument, "expected true or false, found {}", jsonTypeName(json));
    }
    out = json.get<bool>();
    return {};
}

Json PropertyCodec<std::string>::toJson(const std::string& value) {
    return Json(value);
}

Status PropertyCodec<std::string>::fromJson(const Json& json, std::string& out) {
    if (!json.is_string()) {
        return makeError(ErrorCode::InvalidArgument, "expected a string, found {}", jsonTypeName(json));
    }
    out = json.get<std::string>();
    return {};
}

Json PropertyCodec<Uuid>::toJson(const Uuid& value) {
    return value.isNil() ? Json(nullptr) : Json(value.toString());
}

Status PropertyCodec<Uuid>::fromJson(const Json& json, Uuid& out) {
    if (json.is_null()) {
        out = Uuid();
        return {};
    }
    if (!json.is_string()) {
        return makeError(ErrorCode::InvalidArgument, "expected an identifier string or null, found {}",
                         jsonTypeName(json));
    }
    const std::optional<Uuid> parsed = Uuid::parse(json.get<std::string>());
    if (!parsed) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is not a valid identifier", json.get<std::string>());
    }
    out = *parsed;
    return {};
}

void PropertyPatch::applyTo(void* object) const {
    for (const detail::PropertyWrite& write : m_writes) {
        write(object);
    }
}

Json PropertyInfo::get(const void* object) const {
    Json value = m_getter(object);
    if (m_type != PropertyType::Enum) {
        return value;
    }

    const int64_t current = value.get<int64_t>();
    const auto option = std::ranges::find(m_enumOptions, current, &EnumOption::value);
    // A value without a name means the type description is missing an option. Showing the
    // number makes that visible instead of presenting a wrong name.
    return option != m_enumOptions.end() ? Json(option->name) : value;
}

Status PropertyInfo::set(void* object, const Json& value) const {
    CORDITE_TRY_ASSIGN(const detail::PropertyWrite write,
                       parse(value, {.unknownMembers = UnknownMembers::Reject, .editing = true}));
    write(object);
    return {};
}

Result<detail::PropertyWrite> PropertyInfo::parse(const Json& value, const detail::ParseContext& context) const {
    if (context.editing && m_attributes.readOnly) {
        return makeError(ErrorCode::InvalidArgument, "property '{}' is read-only", m_name);
    }

    Result<detail::PropertyWrite> write = parseValue(value, context);
    if (!write) {
        return tl::unexpected(write.error().withContext(fmt::format("property '{}'", m_name)));
    }
    return write;
}

Result<detail::PropertyWrite> PropertyInfo::parseValue(const Json& value, const detail::ParseContext& context) const {
    if (m_type == PropertyType::Enum) {
        if (!value.is_string()) {
            return makeError(ErrorCode::InvalidArgument, "expected one of [{}], found {}",
                             joinOptionNames(m_enumOptions), jsonTypeName(value));
        }
        const std::string name = value.get<std::string>();
        const auto option = std::ranges::find(m_enumOptions, name, &EnumOption::name);
        if (option == m_enumOptions.end()) {
            return makeError(ErrorCode::InvalidArgument, "expected one of [{}], found '{}'",
                             joinOptionNames(m_enumOptions), name);
        }
        return m_parser(Json(option->value), context);
    }

    if (m_attributes.minimum || m_attributes.maximum) {
        if (const int arrayLevels = numberNestingOf(m_type, m_elementType); arrayLevels >= 0) {
            CORDITE_TRY(checkRange(value, m_attributes, arrayLevels));
        }
    }
    return m_parser(value, context);
}

Json PropertyInfo::describe() const {
    Json description = Json::object();
    description["name"] = m_name;
    description["type"] = nameOf(m_type);
    if (m_type == PropertyType::List) {
        description["elementType"] = nameOf(m_elementType);
    }
    if (m_type == PropertyType::Object && m_objectType != nullptr) {
        description["objectType"] = m_objectType->name();
    }
    if (!m_enumOptions.empty()) {
        Json options = Json::array();
        for (const EnumOption& option : m_enumOptions) {
            options.push_back(option.name);
        }
        description["options"] = std::move(options);
    }
    if (!m_attributes.description.empty()) {
        description["description"] = m_attributes.description;
    }
    if (m_attributes.minimum) {
        description["minimum"] = *m_attributes.minimum;
    }
    if (m_attributes.maximum) {
        description["maximum"] = *m_attributes.maximum;
    }
    if (m_attributes.step != 0.0) {
        description["step"] = m_attributes.step;
    }
    if (!m_attributes.unit.empty()) {
        description["unit"] = m_attributes.unit;
    }
    switch (m_attributes.semantic) {
        case PropertySemantic::None:
            break;
        case PropertySemantic::Color:
            description["semantic"] = "color";
            break;
        case PropertySemantic::EntityReference:
            description["semantic"] = "entity";
            break;
        case PropertySemantic::AssetReference:
            description["semantic"] = "asset";
            description["assetType"] = m_attributes.referenceType;
            break;
        case PropertySemantic::Multiline:
            description["semantic"] = "multiline";
            break;
        case PropertySemantic::BitMask:
            description["semantic"] = "bitMask";
            break;
    }
    if (m_attributes.readOnly) {
        description["readOnly"] = true;
    }
    if (m_attributes.transient) {
        description["transient"] = true;
    }
    return description;
}

const PropertyInfo* TypeInfo::findProperty(std::string_view name) const {
    const auto found = std::ranges::find(m_properties, name, &PropertyInfo::name);
    return found != m_properties.end() ? &*found : nullptr;
}

Json TypeInfo::toJson(const void* object) const {
    Json json = Json::object();
    for (const PropertyInfo& property : m_properties) {
        if (!property.attributes().transient) {
            json[property.name()] = property.get(object);
        }
    }
    return json;
}

Result<PropertyPatch> TypeInfo::parse(const Json& json, UnknownMembers unknownMembers) const {
    return parseWith(json, {.unknownMembers = unknownMembers, .editing = false});
}

Result<PropertyPatch> TypeInfo::parseEdit(const Json& json) const {
    return parseWith(json, {.unknownMembers = UnknownMembers::Reject, .editing = true});
}

Status TypeInfo::fromJson(void* object, const Json& json, UnknownMembers unknownMembers) const {
    CORDITE_TRY_ASSIGN(const PropertyPatch patch, parse(json, unknownMembers));
    patch.applyTo(object);
    return {};
}

Result<PropertyPatch> TypeInfo::parseWith(const Json& json, const detail::ParseContext& context) const {
    if (!json.is_object()) {
        return makeError(ErrorCode::InvalidArgument, "{} expects an object, found {}", m_name, jsonTypeName(json));
    }

    PropertyPatch patch;
    patch.m_writes.reserve(json.size());
    for (const auto& [key, value] : json.items()) {
        const PropertyInfo* property = findProperty(key);
        // Saved data never contains runtime state, so there a transient property is as
        // unexpected as a name the type does not have.
        const bool isRuntimeState = property != nullptr && property->attributes().transient && !context.editing;
        if (property == nullptr || isRuntimeState) {
            if (context.unknownMembers == UnknownMembers::Ignore) {
                continue;
            }
            if (isRuntimeState) {
                return makeError(ErrorCode::InvalidArgument, "property '{}' of {} is runtime state and is never loaded",
                                 key, m_name);
            }
            return makeError(ErrorCode::InvalidArgument, "{} has no property '{}'", m_name, key);
        }

        Result<detail::PropertyWrite> write = property->parse(value, context);
        if (!write) {
            return tl::unexpected(write.error().withContext(m_name));
        }
        patch.m_writes.push_back(std::move(*write));
    }
    return patch;
}

Json TypeInfo::describe() const {
    Json description = Json::object();
    description["name"] = m_name;
    Json properties = Json::array();
    for (const PropertyInfo& property : m_properties) {
        properties.push_back(property.describe());
    }
    description["properties"] = std::move(properties);
    return description;
}

} // namespace cordite
