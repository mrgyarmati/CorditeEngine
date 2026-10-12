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
/// CORDITE_TRY_ASSIGN(const std::optional<bool> enabled, jsonGetOptional<bool>(document, "enabled"));
/// @endcode

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {

/// Parses JSON text. `//` and `/* */` comments are accepted. Fails with ParseError for
/// text that is not JSON, and for an object that has a key twice - a mistake that would
/// otherwise go unnoticed, with one of the two values silently lost. `sourceName` is
/// used in error messages only.
Result<Json> parseJson(std::string_view text, std::string_view sourceName = "<memory>");

/// Serializes with the given indentation. The output is deterministic.
std::string dumpJson(const Json& value, int indent = 4);

Result<Json> readJsonFile(const std::filesystem::path& path);

/// Writes atomically, with a trailing newline.
Status writeJsonFile(const std::filesystem::path& path, const Json& value);

/// Returns the JSON type name of a value ("object", "array", "string", ...).
std::string_view jsonTypeName(const Json& value);

/// Wraps a float so that it is written the way a person would write it. JSON numbers are
/// doubles, and the double nearest to 0.1f prints as 0.10000000149011612; this returns
/// the number that prints as 0.1 instead. Reading the result back as a float always
/// gives exactly `value`. Use it for every float that ends up in a file.
[[nodiscard]] Json jsonFromFloat(float value);

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

/// Reads an optional member of a JSON object strictly: an empty optional when the member
/// is missing or null, and an error when it is there with the wrong type. Use it for
/// input where a wrong type must not be mistaken for "not given".
template <typename T>
Result<std::optional<T>> jsonGetOptional(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return makeError(ErrorCode::ParseError, "expected an object, found {}", jsonTypeName(object));
    }
    const auto member = object.find(key);
    if (member == object.end() || member->is_null()) {
        return std::optional<T>();
    }
    try {
        return std::optional<T>(member->template get<T>());
    } catch (const nlohmann::json::exception&) {
        return makeError(ErrorCode::ParseError, "field '{}' has the wrong type ({})", key, jsonTypeName(*member));
    }
}

/// Reads an optional member of a JSON object leniently. Returns `fallback` when the
/// member is missing, null or has the wrong type.
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
