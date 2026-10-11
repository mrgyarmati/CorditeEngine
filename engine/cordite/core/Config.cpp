#include "cordite/core/Config.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/core/StringUtils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace cordite {
namespace {

Json toJsonValue(const ConfigValue& value) {
    return std::visit([](const auto& held) { return Json(held); }, value);
}

/// Converts a JSON scalar to the ConfigValue alternative that best matches it.
std::optional<ConfigValue> fromJsonValue(const Json& value) {
    if (value.is_boolean()) {
        return ConfigValue(value.get<bool>());
    }
    if (value.is_number_integer()) {
        return ConfigValue(value.get<int64_t>());
    }
    if (value.is_number_float()) {
        return ConfigValue(value.get<double>());
    }
    if (value.is_string()) {
        return ConfigValue(value.get<std::string>());
    }
    return std::nullopt;
}

/// A key is one or more non-empty segments of [a-z0-9_] separated by single dots.
bool isValidKey(std::string_view key) {
    bool segmentIsEmpty = true;
    for (const char character : key) {
        if (character == '.') {
            if (segmentIsEmpty) {
                return false;
            }
            segmentIsEmpty = true;
            continue;
        }
        const bool allowed =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
        if (!allowed) {
            return false;
        }
        segmentIsEmpty = false;
    }
    return !segmentIsEmpty;
}

/// True when `group` names a group that `key` lives in, e.g. "render" for "render.vsync".
bool isGroupOf(std::string_view group, std::string_view key) {
    return key.size() > group.size() && key.starts_with(group) && key[group.size()] == '.';
}

double numericValue(const ConfigValue& value) {
    if (const auto* integer = std::get_if<int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    return std::get<double>(value);
}

void flattenJson(const Json& object, const std::string& prefix, std::vector<std::pair<std::string, Json>>& out) {
    for (const auto& [key, value] : object.items()) {
        const std::string path = prefix.empty() ? key : fmt::format("{}.{}", prefix, key);
        if (value.is_object()) {
            flattenJson(value, path, out);
        } else {
            out.emplace_back(path, value);
        }
    }
}

void insertNested(Json& root, std::string_view key, Json value) {
    Json* node = &root;
    const std::vector<std::string_view> parts = split(key, '.');
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        node = &(*node)[std::string(parts[index])];
    }
    (*node)[std::string(parts.back())] = std::move(value);
}

} // namespace

std::string_view nameOf(ConfigType type) {
    switch (type) {
        case ConfigType::Bool:
            return "bool";
        case ConfigType::Int:
            return "int";
        case ConfigType::Float:
            return "float";
        case ConfigType::String:
            return "string";
    }
    return "unknown";
}

ConfigType configTypeOf(const ConfigValue& value) {
    return static_cast<ConfigType>(value.index());
}

Status Config::declare(ConfigDeclaration declaration) {
    if (!isValidKey(declaration.key)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is not a valid setting key: use segments of a-z, 0-9 and _ separated by dots",
                         declaration.key);
    }

    const ConfigType newType = configTypeOf(declaration.defaultValue);
    const auto existing = m_entries.find(declaration.key);
    if (existing != m_entries.end()) {
        const ConfigType existingType = existing->second.type();
        if (existingType != newType) {
            return makeError(ErrorCode::AlreadyExists, "setting '{}' is already declared as {}, cannot redeclare as {}",
                             declaration.key, nameOf(existingType), nameOf(newType));
        }
        return {};
    }

    // "render" cannot be a setting while "render.vsync" exists (or the other way round):
    // the nested JSON form would need one member to be both a value and an object.
    for (const auto& entry : m_entries) {
        if (isGroupOf(entry.first, declaration.key) || isGroupOf(declaration.key, entry.first)) {
            return makeError(ErrorCode::AlreadyExists,
                             "setting '{}' conflicts with '{}': a key cannot be both a setting and a group",
                             declaration.key, entry.first);
        }
    }

    const bool isNumeric = newType == ConfigType::Int || newType == ConfigType::Float;
    if (declaration.minimum || declaration.maximum) {
        if (!isNumeric) {
            return makeError(ErrorCode::InvalidArgument, "setting '{}' is a {} and cannot have a range",
                             declaration.key, nameOf(newType));
        }
        const double defaultNumber = numericValue(declaration.defaultValue);
        if ((declaration.minimum && declaration.maximum && *declaration.minimum > *declaration.maximum) ||
            (declaration.minimum && defaultNumber < *declaration.minimum) ||
            (declaration.maximum && defaultNumber > *declaration.maximum)) {
            return makeError(ErrorCode::OutOfRange, "the default of setting '{}' is outside its declared range",
                             declaration.key);
        }
    }

    std::string key = declaration.key;
    ConfigValue value = declaration.defaultValue;
    m_entries.emplace(std::move(key), ConfigEntry{std::move(declaration), std::move(value)});
    return {};
}

bool Config::contains(std::string_view key) const {
    return m_entries.find(key) != m_entries.end();
}

const ConfigEntry* Config::find(std::string_view key) const {
    const auto entry = m_entries.find(key);
    return entry != m_entries.end() ? &entry->second : nullptr;
}

bool Config::getBool(std::string_view key) const {
    const ConfigEntry* entry = find(key);
    CORDITE_ASSERT(entry != nullptr && entry->type() == ConfigType::Bool, "'{}' is not a declared bool setting", key);
    const bool* value = entry != nullptr ? std::get_if<bool>(&entry->value) : nullptr;
    return value != nullptr && *value;
}

int64_t Config::getInt(std::string_view key) const {
    const ConfigEntry* entry = find(key);
    CORDITE_ASSERT(entry != nullptr && entry->type() == ConfigType::Int, "'{}' is not a declared int setting", key);
    const int64_t* value = entry != nullptr ? std::get_if<int64_t>(&entry->value) : nullptr;
    return value != nullptr ? *value : 0;
}

double Config::getFloat(std::string_view key) const {
    const ConfigEntry* entry = find(key);
    CORDITE_ASSERT(entry != nullptr && entry->type() == ConfigType::Float, "'{}' is not a declared float setting", key);
    const double* value = entry != nullptr ? std::get_if<double>(&entry->value) : nullptr;
    return value != nullptr ? *value : 0.0;
}

std::string Config::getString(std::string_view key) const {
    const ConfigEntry* entry = find(key);
    CORDITE_ASSERT(entry != nullptr && entry->type() == ConfigType::String, "'{}' is not a declared string setting",
                   key);
    const std::string* value = entry != nullptr ? std::get_if<std::string>(&entry->value) : nullptr;
    return value != nullptr ? *value : std::string();
}

Status Config::set(std::string_view key, const ConfigValue& value) {
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        return makeError(ErrorCode::NotFound, "unknown setting '{}'", key);
    }

    ConfigEntry& entry = found->second;
    const ConfigType expected = entry.type();
    ConfigValue converted = value;
    if (expected == ConfigType::Float && std::holds_alternative<int64_t>(value)) {
        converted = static_cast<double>(std::get<int64_t>(value));
    }
    if (configTypeOf(converted) != expected) {
        return makeError(ErrorCode::InvalidArgument, "setting '{}' expects {} but got {}", key, nameOf(expected),
                         nameOf(configTypeOf(value)));
    }

    if (expected == ConfigType::Int || expected == ConfigType::Float) {
        const double number = numericValue(converted);
        if (!std::isfinite(number)) {
            return makeError(ErrorCode::OutOfRange, "setting '{}' must be a finite number", key);
        }
        const ConfigDeclaration& declaration = entry.declaration;
        if ((declaration.minimum && number < *declaration.minimum) ||
            (declaration.maximum && number > *declaration.maximum)) {
            return makeError(ErrorCode::OutOfRange, "setting '{}' = {} is outside the allowed range [{}, {}]", key,
                             number, declaration.minimum ? fmt::format("{}", *declaration.minimum) : "-inf",
                             declaration.maximum ? fmt::format("{}", *declaration.maximum) : "+inf");
        }
    }

    if (entry.value == converted) {
        return {};
    }
    entry.value = std::move(converted);

    // Copy first: a callback may add or remove callbacks.
    const auto callbacks = m_callbacks;
    for (const auto& callback : callbacks) {
        callback.second(found->first, entry.value);
    }
    return {};
}

Status Config::setFromString(std::string_view key, std::string_view text) {
    const ConfigEntry* entry = find(key);
    if (entry == nullptr) {
        return makeError(ErrorCode::NotFound, "unknown setting '{}'", key);
    }

    const std::string_view trimmed = trim(text);
    switch (entry->type()) {
        case ConfigType::Bool: {
            if (equalsIgnoreCase(trimmed, "true") || trimmed == "1" || equalsIgnoreCase(trimmed, "on")) {
                return set(key, true);
            }
            if (equalsIgnoreCase(trimmed, "false") || trimmed == "0" || equalsIgnoreCase(trimmed, "off")) {
                return set(key, false);
            }
            return makeError(ErrorCode::InvalidArgument, "setting '{}' expects true or false, got '{}'", key, trimmed);
        }
        case ConfigType::Int: {
            int64_t parsed = 0;
            const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (error != std::errc() || end != trimmed.data() + trimmed.size()) {
                return makeError(ErrorCode::InvalidArgument, "setting '{}' expects an integer, got '{}'", key, trimmed);
            }
            return set(key, parsed);
        }
        case ConfigType::Float: {
            // std::from_chars for floating point is missing from Apple's libc++, so use
            // strtod. The engine never changes the C locale, so '.' is the decimal point.
            const std::string buffer(trimmed);
            char* end = nullptr;
            const double parsed = std::strtod(buffer.c_str(), &end);
            if (buffer.empty() || end != buffer.c_str() + buffer.size()) {
                return makeError(ErrorCode::InvalidArgument, "setting '{}' expects a number, got '{}'", key, trimmed);
            }
            return set(key, parsed);
        }
        case ConfigType::String:
            return set(key, std::string(text));
    }
    return makeError(ErrorCode::InvalidState, "setting '{}' has an unknown type", key);
}

void Config::resetToDefaults() {
    // Collect first so the change callbacks observe a consistent store.
    std::vector<std::pair<std::string, ConfigValue>> changes;
    for (const auto& [key, entry] : m_entries) {
        if (!entry.isDefault()) {
            changes.emplace_back(key, entry.declaration.defaultValue);
        }
    }
    for (const auto& [key, value] : changes) {
        const Status status = set(key, value);
        CORDITE_ASSERT(status.has_value(), "default of '{}' failed validation: {}", key, status.error().message);
    }
}

Status Config::applyJson(const Json& object) {
    if (!object.is_object()) {
        return makeError(ErrorCode::ParseError, "settings must be a JSON object, found {}", jsonTypeName(object));
    }

    std::vector<std::pair<std::string, Json>> flattened;
    flattenJson(object, "", flattened);

    std::string problems;
    for (const auto& [key, json] : flattened) {
        Status status;
        if (const std::optional<ConfigValue> value = fromJsonValue(json)) {
            status = set(key, *value);
        } else {
            status = makeError(ErrorCode::InvalidArgument, "setting '{}' has unsupported JSON type {}", key,
                               jsonTypeName(json));
        }
        if (!status) {
            if (!problems.empty()) {
                problems += "; ";
            }
            problems += status.error().message;
        }
    }

    if (!problems.empty()) {
        return makeError(ErrorCode::InvalidArgument, "{}", problems);
    }
    return {};
}

Json Config::toJson(bool onlyModified) const {
    Json root = Json::object();
    for (const auto& [key, entry] : m_entries) {
        if (onlyModified && entry.isDefault()) {
            continue;
        }
        insertNested(root, key, toJsonValue(entry.value));
    }
    return root;
}

std::vector<ConfigEntry> Config::entries() const {
    std::vector<ConfigEntry> result;
    result.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        result.push_back(entry);
    }
    return result;
}

Config::CallbackId Config::addChangeCallback(ChangeCallback callback) {
    const CallbackId id = m_nextCallbackId++;
    m_callbacks.emplace_back(id, std::move(callback));
    return id;
}

void Config::removeChangeCallback(CallbackId id) {
    std::erase_if(m_callbacks, [id](const auto& entry) { return entry.first == id; });
}

} // namespace cordite
