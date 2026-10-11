#pragma once

/// @file
/// Runtime descriptions of data types ("reflection").
///
/// A TypeInfo lists the properties of a struct: their names, types, constraints and how
/// to read and write them as JSON. One description drives everything that has to treat
/// data generically: saving and loading scenes, the editor inspector, the scripting
/// bindings, and the automation API (including the schema it publishes to AI agents).
/// Adding a field to a component therefore means describing it once.
///
/// @code
/// struct LightSettings {
///     Vec3 color{1.0f};
///     float intensity = 1.0f;
///     LightKind kind = LightKind::Point;
/// };
///
/// const TypeInfo type = TypeBuilder<LightSettings>("LightSettings")
///     .property("color", &LightSettings::color, {.description = "Linear RGB", .semantic = PropertySemantic::Color})
///     .property("intensity", &LightSettings::intensity, {.minimum = 0.0, .unit = "cd"})
///     .enumProperty("kind", &LightSettings::kind, {{LightKind::Point, "point"}, {LightKind::Spot, "spot"}})
///     .build();
///
/// LightSettings settings;
/// CORDITE_TRY(type.fromJson(&settings, json));     // validates types, ranges and enum names
/// const Json saved = type.toJson(&settings);
/// @endcode
///
/// JSON never reaches an object unchecked. It is first parsed into a PropertyPatch, which
/// fails as a whole if any member is wrong; applying a patch cannot fail. Code that has
/// to validate a lot of input before changing anything (loading a scene, an editor
/// command) parses everything first and applies afterwards:
///
/// @code
/// CORDITE_TRY_ASSIGN(const PropertyPatch patch, type.parseEdit(json));   // nothing has changed yet
/// patch.applyTo(&settings);
/// @endcode
///
/// A type becomes usable as a property by specializing PropertyCodec for it. The core
/// module provides bool, integers, floats, std::string, Uuid and std::vector of any
/// supported type; cordite/math/MathReflection.h adds vectors and quaternions.

#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cordite {

enum class PropertyType : uint8_t {
    Bool,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Uuid,
    /// One of a fixed set of named values, stored in JSON by name.
    Enum,
    /// A nested struct with its own TypeInfo.
    Object,
    /// A sequence of values that all have the type given by PropertyInfo::elementType().
    List,
};

std::string_view nameOf(PropertyType type);

/// What a value means, beyond its storage type. Tools use it to pick an editor.
enum class PropertySemantic : uint8_t {
    None,
    /// A linear color (Vec3 or Vec4): edited with a color picker.
    Color,
    /// A Uuid, or a list of them, identifying other entities of the same world.
    /// Redirected to the copies when a group of entities is duplicated.
    EntityReference,
    /// A Uuid that identifies an asset; PropertyAttributes::referenceType names its kind.
    AssetReference,
    /// A string holding several lines of text.
    Multiline,
    /// An integer whose bits are switches, such as lighting channels: edited as a row
    /// of check boxes, one per bit up to the highest that PropertyAttributes::maximum
    /// allows. In data it stays a number.
    BitMask,
};

/// Optional information about a property.
struct PropertyAttributes {
    /// One sentence shown as a tooltip and published in the automation schema.
    std::string description;
    PropertySemantic semantic = PropertySemantic::None;
    /// Inclusive bounds, enforced whenever a value is parsed. They apply to Int and Float
    /// properties, to each component of a vector, and to each element of a list of those.
    std::optional<double> minimum;
    std::optional<double> maximum;
    /// Suggested increment when dragging the value in an editor. Zero means "choose one".
    double step = 0.0;
    /// Display unit such as "m", "deg" or "s".
    std::string unit;
    /// For AssetReference properties: the kind of asset expected, e.g. "Mesh".
    std::string referenceType;
    /// Tools show the value but may not change it: edits are rejected. It is still saved
    /// and loaded like any other property.
    bool readOnly = false;
    /// Runtime state: tools may read and change it, but it is never saved or loaded.
    bool transient = false;
};

/// One named value of an Enum property.
struct EnumOption {
    std::string name;
    int64_t value = 0;
};

/// Describes how a C++ type is represented as a property value. Specialize it to make a
/// type usable with TypeBuilder::property:
///
/// @code
/// template <>
/// struct PropertyCodec<Vec3> {
///     static constexpr PropertyType kType = PropertyType::Vec3;
///     static Json toJson(const Vec3& value);
///     static Status fromJson(const Json& json, Vec3& out);
/// };
/// @endcode
///
/// `fromJson` must reject every JSON value that is not a valid representation, because
/// its input comes from files and from tools.
template <typename T>
struct PropertyCodec;

template <typename T>
concept ReflectableValue =
    std::default_initializable<T> && std::copyable<T> && requires(const T& value, T& out, const Json& json) {
        { PropertyCodec<T>::kType } -> std::convertible_to<PropertyType>;
        { PropertyCodec<T>::toJson(value) } -> std::same_as<Json>;
        { PropertyCodec<T>::fromJson(json, out) } -> std::same_as<Status>;
    };

/// What parsing does with JSON members that are not properties of the type.
enum class UnknownMembers : uint8_t {
    /// Fail. An unknown name is almost always a typo, and silently dropping it loses data.
    Reject,
    /// Skip them. For data that may come from a newer version of the engine.
    Ignore,
};

namespace detail {

/// Travels through nested objects while JSON is being validated.
struct ParseContext {
    UnknownMembers unknownMembers = UnknownMembers::Reject;
    /// True when a tool is changing values: read-only properties are refused and
    /// transient ones accepted. False when saved data is restored: the reverse.
    bool editing = false;
};

/// Stores one already validated value in an object.
using PropertyWrite = std::function<void(void* object)>;

Status integerFromJson(const Json& json, int64_t minimum, int64_t maximum, int64_t& out);
Status unsignedFromJson(const Json& json, uint64_t maximum, uint64_t& out);
Status floatFromJson(const Json& json, double limit, double& out);
Status expectArray(const Json& json);

} // namespace detail

template <>
struct PropertyCodec<bool> {
    static constexpr PropertyType kType = PropertyType::Bool;
    static Json toJson(const bool& value);
    static Status fromJson(const Json& json, bool& out);
};

/// Signed and unsigned integers of any width. Values that do not fit are rejected. A JSON
/// number written with a fraction part is accepted when it is whole ("2.0").
template <std::integral T>
    requires(!std::same_as<T, bool>)
struct PropertyCodec<T> {
    static constexpr PropertyType kType = PropertyType::Int;

    static Json toJson(const T& value) { return Json(value); }

    static Status fromJson(const Json& json, T& out) {
        if constexpr (std::is_signed_v<T>) {
            int64_t wide = 0;
            CORDITE_TRY(
                detail::integerFromJson(json, std::numeric_limits<T>::min(), std::numeric_limits<T>::max(), wide));
            out = static_cast<T>(wide);
        } else {
            uint64_t wide = 0;
            CORDITE_TRY(detail::unsignedFromJson(json, std::numeric_limits<T>::max(), wide));
            out = static_cast<T>(wide);
        }
        return {};
    }
};

/// float and double. Non-finite values, and doubles too large for a float, are rejected.
template <std::floating_point T>
struct PropertyCodec<T> {
    static constexpr PropertyType kType = PropertyType::Float;

    static Json toJson(const T& value) {
        if constexpr (std::same_as<T, float>) {
            return jsonFromFloat(value);
        } else {
            return Json(value);
        }
    }

    static Status fromJson(const Json& json, T& out) {
        double wide = 0.0;
        CORDITE_TRY(detail::floatFromJson(json, static_cast<double>(std::numeric_limits<T>::max()), wide));
        out = static_cast<T>(wide);
        return {};
    }
};

template <>
struct PropertyCodec<std::string> {
    static constexpr PropertyType kType = PropertyType::String;
    static Json toJson(const std::string& value);
    static Status fromJson(const Json& json, std::string& out);
};

/// Stored as the canonical string form; the nil Uuid is stored as null.
template <>
struct PropertyCodec<Uuid> {
    static constexpr PropertyType kType = PropertyType::Uuid;
    static Json toJson(const Uuid& value);
    static Status fromJson(const Json& json, Uuid& out);
};

template <ReflectableValue T>
struct PropertyCodec<std::vector<T>> {
    static constexpr PropertyType kType = PropertyType::List;
    static constexpr PropertyType kElementType = PropertyCodec<T>::kType;

    static Json toJson(const std::vector<T>& value) {
        Json array = Json::array();
        for (const T& element : value) {
            array.push_back(PropertyCodec<T>::toJson(element));
        }
        return array;
    }

    static Status fromJson(const Json& json, std::vector<T>& out) {
        CORDITE_TRY(detail::expectArray(json));
        std::vector<T> parsed;
        parsed.reserve(json.size());
        for (size_t index = 0; index < json.size(); ++index) {
            T element{};
            if (const Status status = PropertyCodec<T>::fromJson(json[index], element); !status) {
                return tl::unexpected(status.error().withContext(fmt::format("element {}", index)));
            }
            parsed.push_back(std::move(element));
        }
        out = std::move(parsed);
        return {};
    }
};

/// Property values that have passed validation and are ready to be written. Produced by
/// TypeInfo::parse and TypeInfo::parseEdit.
class PropertyPatch {
public:
    /// Writes the values into `object`, which must point to an instance of the type whose
    /// TypeInfo produced the patch. Properties the patch does not mention keep their
    /// value. This cannot fail, and one patch can be applied to any number of objects.
    void applyTo(void* object) const;

    /// True when the patch changes nothing, as for an empty JSON object.
    [[nodiscard]] bool isEmpty() const { return m_writes.empty(); }

private:
    friend class TypeInfo;

    std::vector<detail::PropertyWrite> m_writes;
};

class TypeInfo;

/// One property of a reflected type.
class PropertyInfo {
public:
    [[nodiscard]] const std::string& name() const { return m_name; }

    [[nodiscard]] PropertyType type() const { return m_type; }

    [[nodiscard]] const PropertyAttributes& attributes() const { return m_attributes; }

    /// The element type of a List property.
    [[nodiscard]] PropertyType elementType() const { return m_elementType; }

    /// The named values of an Enum property; empty for every other type.
    [[nodiscard]] std::span<const EnumOption> enumOptions() const { return m_enumOptions; }

    /// The description of an Object property's struct; null for every other type.
    [[nodiscard]] const TypeInfo* objectType() const { return m_objectType; }

    /// Returns the property of `object`, which must point to an instance of the type the
    /// property belongs to.
    [[nodiscard]] Json get(const void* object) const;

    /// Changes the property of `object` as an edit made by a tool: `value` is validated
    /// first, and on failure `object` is unchanged. Fails with InvalidArgument for a
    /// wrong JSON type, an unknown enum name or a read-only property, and with OutOfRange
    /// for a value outside the declared range.
    [[nodiscard]] Status set(void* object, const Json& value) const;

    /// A short JSON description of the property for the automation schema.
    [[nodiscard]] Json describe() const;

private:
    template <typename T>
    friend class TypeBuilder;
    friend class TypeInfo;

    using Parser = std::function<Result<detail::PropertyWrite>(const Json&, const detail::ParseContext&)>;

    [[nodiscard]] Result<detail::PropertyWrite> parse(const Json& value, const detail::ParseContext& context) const;
    [[nodiscard]] Result<detail::PropertyWrite> parseValue(const Json& value,
                                                           const detail::ParseContext& context) const;

    std::string m_name;
    PropertyType m_type = PropertyType::Bool;
    PropertyType m_elementType = PropertyType::Bool;
    PropertyAttributes m_attributes;
    std::vector<EnumOption> m_enumOptions;
    const TypeInfo* m_objectType = nullptr;
    std::function<Json(const void*)> m_getter;
    Parser m_parser;
};

/// The description of a struct: its name and its properties, in declaration order.
class TypeInfo {
public:
    [[nodiscard]] const std::string& name() const { return m_name; }

    [[nodiscard]] std::span<const PropertyInfo> properties() const { return m_properties; }

    /// Returns the property with the given name, or nullptr.
    [[nodiscard]] const PropertyInfo* findProperty(std::string_view name) const;

    /// Serializes every non-transient property of `object` into a JSON object.
    [[nodiscard]] Json toJson(const void* object) const;

    /// Validates saved data: a JSON object as written by toJson, or by hand in the same
    /// form. Members may be left out. Read-only properties are accepted, because saved
    /// data legitimately contains them; transient ones are treated as unknown members,
    /// because it never does.
    [[nodiscard]] Result<PropertyPatch> parse(const Json& json,
                                              UnknownMembers unknownMembers = UnknownMembers::Reject) const;

    /// Validates a change requested by a person, a script or an agent. Unknown members
    /// and read-only properties are errors; transient properties may be changed.
    [[nodiscard]] Result<PropertyPatch> parseEdit(const Json& json) const;

    /// Restores saved data into `object`: parse() followed by PropertyPatch::applyTo.
    /// Properties the JSON does not mention keep their current value, and on failure
    /// `object` is unchanged.
    [[nodiscard]] Status fromJson(void* object, const Json& json,
                                  UnknownMembers unknownMembers = UnknownMembers::Reject) const;

    /// A JSON description of the type and its properties for the automation schema.
    [[nodiscard]] Json describe() const;

private:
    template <typename T>
    friend class TypeBuilder;

    [[nodiscard]] Result<PropertyPatch> parseWith(const Json& json, const detail::ParseContext& context) const;

    std::string m_name;
    std::vector<PropertyInfo> m_properties;
};

/// Builds the TypeInfo of a struct T.
template <typename T>
class TypeBuilder {
public:
    explicit TypeBuilder(std::string name) { m_type.m_name = std::move(name); }

    /// Describes a data member whose type has a PropertyCodec.
    template <ReflectableValue Member>
    TypeBuilder& property(std::string name, Member T::* member, PropertyAttributes attributes = {}) {
        PropertyInfo& info = add(std::move(name), PropertyCodec<Member>::kType, std::move(attributes));
        if constexpr (requires { PropertyCodec<Member>::kElementType; }) {
            info.m_elementType = PropertyCodec<Member>::kElementType;
        }
        info.m_getter = [member](const void* object) {
            return PropertyCodec<Member>::toJson(static_cast<const T*>(object)->*member);
        };
        info.m_parser = [member](const Json& json,
                                 const detail::ParseContext& /*context*/) -> Result<detail::PropertyWrite> {
            Member parsed{};
            CORDITE_TRY(PropertyCodec<Member>::fromJson(json, parsed));
            return detail::PropertyWrite(
                [member, parsed = std::move(parsed)](void* object) { static_cast<T*>(object)->*member = parsed; });
        };
        return *this;
    }

    /// Describes an enum member. `options` maps each enumerator to the name it has in
    /// files and tools; a value without a name cannot be stored.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    TypeBuilder& enumProperty(std::string name, Enum T::* member, std::vector<std::pair<Enum, std::string>> options,
                              PropertyAttributes attributes = {}) {
        PropertyInfo& info = add(std::move(name), PropertyType::Enum, std::move(attributes));
        for (auto& [value, optionName] : options) {
            info.m_enumOptions.push_back({std::move(optionName), static_cast<int64_t>(value)});
        }

        // The accessors below work on the underlying integer; PropertyInfo translates
        // between it and the option names, so the integer it passes in is always valid.
        info.m_getter = [member](const void* object) {
            return Json(static_cast<int64_t>(static_cast<const T*>(object)->*member));
        };
        info.m_parser = [member](const Json& json,
                                 const detail::ParseContext& /*context*/) -> Result<detail::PropertyWrite> {
            const Enum value = static_cast<Enum>(json.get<int64_t>());
            return detail::PropertyWrite([member, value](void* object) { static_cast<T*>(object)->*member = value; });
        };
        return *this;
    }

    /// Describes a member that is itself a reflected struct. `type` must outlive the
    /// TypeInfo being built.
    template <typename Member>
    TypeBuilder& objectProperty(std::string name, Member T::* member, const TypeInfo& type,
                                PropertyAttributes attributes = {}) {
        PropertyInfo& info = add(std::move(name), PropertyType::Object, std::move(attributes));
        info.m_objectType = &type;
        info.m_getter = [member, &type](const void* object) {
            return type.toJson(&(static_cast<const T*>(object)->*member));
        };
        info.m_parser = [member, &type](const Json& json,
                                        const detail::ParseContext& context) -> Result<detail::PropertyWrite> {
            CORDITE_TRY_ASSIGN(PropertyPatch patch, type.parseWith(json, context));
            return detail::PropertyWrite([member, patch = std::move(patch)](void* object) {
                patch.applyTo(&(static_cast<T*>(object)->*member));
            });
        };
        return *this;
    }

    /// Finishes the description. The builder is left empty.
    [[nodiscard]] TypeInfo build() { return std::exchange(m_type, {}); }

private:
    PropertyInfo& add(std::string name, PropertyType type, PropertyAttributes attributes) {
        CORDITE_ASSERT(!name.empty(), "a property of {} has no name", m_type.m_name);
        CORDITE_ASSERT(m_type.findProperty(name) == nullptr, "{} already has a property named '{}'", m_type.m_name,
                       name);

        PropertyInfo& info = m_type.m_properties.emplace_back();
        info.m_name = std::move(name);
        info.m_type = type;
        info.m_attributes = std::move(attributes);
        return info;
    }

    TypeInfo m_type;
};

} // namespace cordite
