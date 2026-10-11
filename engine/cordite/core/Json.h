#pragma once

/// @file
/// JSON parsing and writing with Result-based error handling.
///
/// nlohmann::json reports problems by throwing. These wrappers convert them into
/// Results so that malformed files never unwind through engine code.
///
/// @code
/// CORDITE_TRY_ASSIGN(const Json document, readJsonFile(path));
/// CORDITE_TRY_ASSIGN(const std::string name, jsonGet<std::string>(document, "name"));
/// const int width = jsonGetOr<int>(document, "width", 1280);
/// @endcode

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace cordite {

/// Parses JSON text. `//` and `/* */` comments are accepted. `sourceName` is used in
/// error messages only.
Result<Json> parseJson(std::string_view text, std::string_view sourceName = "<memory>");

/// Serializes with the given indentation. The output is deterministic.
std::string dumpJson(const Json& value, int indent = 4);

Result<Json> readJsonFile(const std::filesystem::path& path);

/// Writes atomically, with a trailing newline.
Status writeJsonFile(const std::filesystem::path& path, const Json& value);

/// Returns the JSON type name of a value ("object", "array", "string", ...).
std::string_view jsonTypeName(const Json& value);

/// Reads a required member of a JSON object, checking that it has a convertible type.
template <typename T>
Result<T> jsonGet(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return makeError(ErrorCode::ParseError, "expected an object containing '{}', found {}", key,
                         jsonTypeName(object));
    }
    const auto member = object.find(key);
    if (member == object.end()) {
        return makeError(ErrorCode::ParseError, "missing required field '{}'", key);
    }
    try {
        return member->template get<T>();
    } catch (const nlohmann::json::exception&) {
        return makeError(ErrorCode::ParseError, "field '{}' has the wrong type ({})", key, jsonTypeName(*member));
    }
}

/// Reads an optional member of a JSON object. Returns `fallback` when the member is
/// missing, null or has the wrong type.
template <typename T>
T jsonGetOr(const Json& object, std::string_view key, T fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto member = object.find(key);
    if (member == object.end() || member->is_null()) {
        return fallback;
    }
    try {
        return member->template get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

} // namespace cordite
