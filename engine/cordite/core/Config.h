#pragma once

/// @file
/// Typed engine settings ("console variables").
///
/// Subsystems declare the settings they understand, with a default, a description and an
/// optional numeric range. Values are then overridden from JSON files, quality presets,
/// command-line arguments, the editor and scripts. Because every setting is declared,
/// typos and out-of-range values are rejected instead of being silently ignored.
///
/// @code
/// Config config;
/// config.declare({.key = "render.bloom.intensity", .defaultValue = 0.6,
///                 .description = "Strength of the bloom effect", .minimum = 0.0, .maximum = 4.0});
/// CORDITE_TRY(config.set("render.bloom.intensity", 0.8));
/// const double intensity = config.getFloat("render.bloom.intensity");
/// @endcode
///
/// Config is not thread-safe; use it from the main thread.

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cordite {

/// A setting value. Integers convert implicitly to Float settings; nothing else converts.
using ConfigValue = std::variant<bool, int64_t, double, std::string>;

enum class ConfigType : uint8_t {
    Bool,
    Int,
    Float,
    String,
};

std::string_view nameOf(ConfigType type);

/// Returns the type of the alternative a value holds.
ConfigType configTypeOf(const ConfigValue& value);

struct ConfigDeclaration {
    /// Dotted path of lowercase segments made of a-z, 0-9 and _, e.g.
    /// "render.shadows.resolution".
    std::string key;
    /// Also determines the type of the setting.
    ConfigValue defaultValue;
    std::string description;
    /// Inclusive bounds for Int and Float settings.
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct ConfigEntry {
    ConfigDeclaration declaration;
    ConfigValue value;

    [[nodiscard]] ConfigType type() const { return configTypeOf(declaration.defaultValue); }

    [[nodiscard]] bool isDefault() const { return value == declaration.defaultValue; }
};

class Config {
public:
    using CallbackId = uint32_t;
    using ChangeCallback = std::function<void(std::string_view key, const ConfigValue& value)>;

    /// Declares a setting. Declaring the same key twice with the same type is allowed
    /// (the first declaration wins); a different type is an AlreadyExists error.
    ///
    /// Fails with InvalidArgument for a malformed key or a range on a non-numeric
    /// setting, with OutOfRange when the default lies outside its own range, and with
    /// AlreadyExists when the key is a group of an existing setting or vice versa
    /// ("render" and "render.vsync" cannot both be settings).
    Status declare(ConfigDeclaration declaration);

    [[nodiscard]] bool contains(std::string_view key) const;

    /// Returns the entry for a key, or nullptr when it has not been declared.
    [[nodiscard]] const ConfigEntry* find(std::string_view key) const;

    /// Typed getters. Reading an undeclared key or the wrong type is a programmer error:
    /// it asserts and returns a zero value.
    [[nodiscard]] bool getBool(std::string_view key) const;
    [[nodiscard]] int64_t getInt(std::string_view key) const;
    [[nodiscard]] double getFloat(std::string_view key) const;
    [[nodiscard]] std::string getString(std::string_view key) const;

    /// Sets a value after checking its type and range. Fails with NotFound for an
    /// undeclared key, InvalidArgument for a type mismatch and OutOfRange for a value
    /// outside the declared bounds.
    Status set(std::string_view key, const ConfigValue& value);

    /// Parses `text` according to the declared type of the setting, then sets it. Used
    /// for command-line overrides such as `--set render.vsync=false`.
    Status setFromString(std::string_view key, std::string_view text);

    void resetToDefaults();

    /// Applies a JSON object. Nested objects are flattened with '.', so
    /// {"render": {"vsync": false}} sets "render.vsync". Valid entries are applied even
    /// when others fail; the returned error lists every rejected key.
    Status applyJson(const Json& object);

    /// Serializes to a nested JSON object, optionally only the non-default values.
    [[nodiscard]] Json toJson(bool onlyModified = false) const;

    /// Returns all entries sorted by key.
    [[nodiscard]] std::vector<ConfigEntry> entries() const;

    /// Registers a callback invoked after a setting changes value.
    CallbackId addChangeCallback(ChangeCallback callback);
    void removeChangeCallback(CallbackId id);

private:
    std::map<std::string, ConfigEntry, std::less<>> m_entries;
    std::vector<std::pair<CallbackId, ChangeCallback>> m_callbacks;
    CallbackId m_nextCallbackId = 1;
};

} // namespace cordite
