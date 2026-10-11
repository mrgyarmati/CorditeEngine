#include "cordite/core/Json.h"

#include "cordite/core/FileSystem.h"

#include <cmath>

namespace cordite {

Result<Json> parseJson(std::string_view text, std::string_view sourceName) {
    try {
        const bool allowExceptions = true;
        const bool ignoreComments = true;
        return Json::parse(text.begin(), text.end(), nullptr, allowExceptions, ignoreComments);
    } catch (const nlohmann::json::exception& exception) {
        return makeError(ErrorCode::ParseError, "{}: {}", sourceName, exception.what());
    }
}

std::string dumpJson(const Json& value, int indent) {
    const bool ensureAscii = false;
    // Invalid UTF-8 in a string is replaced rather than thrown on, so that saving never fails.
    return value.dump(indent, ' ', ensureAscii, nlohmann::json::error_handler_t::replace);
}

Result<Json> readJsonFile(const std::filesystem::path& path) {
    CORDITE_TRY_ASSIGN(const std::string text, readTextFile(path));
    return parseJson(text, pathToUtf8(path));
}

Status writeJsonFile(const std::filesystem::path& path, const Json& value) {
    std::string text = dumpJson(value);
    text.push_back('\n');
    return writeTextFile(path, text);
}

std::string_view jsonTypeName(const Json& value) {
    return value.type_name();
}

Json jsonFromFloat(float value) {
    const auto exact = static_cast<double>(value);
    if (!std::isfinite(value)) {
        return Json(exact);
    }

    // fmt prints the shortest decimal that identifies the float; the JSON parser turns it
    // into the nearest double without depending on the C locale.
    const bool allowExceptions = false;
    const Json shortest = Json::parse(fmt::format("{}", value), nullptr, allowExceptions);
    if (shortest.is_number()) {
        const double candidate = shortest.get<double>();
        // Rounding twice (decimal to double to float) can in rare cases land on a
        // neighbouring float, and "-0" parses as an integer without a sign. Keep the
        // exact value then.
        if (static_cast<float>(candidate) == value && std::signbit(candidate) == std::signbit(value)) {
            return Json(candidate);
        }
    }
    return Json(exact);
}

} // namespace cordite
