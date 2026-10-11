#include "cordite/core/Document.h"

#include "cordite/core/Reflection.h"

#include <algorithm>
#include <optional>
#include <string>

namespace cordite {

Json makeDocument(const DocumentFormat& format) {
    Json document = Json::object();
    document["format"] = format.name;
    document["version"] = format.version;
    return document;
}

Result<uint32_t> readDocumentHeader(const Json& document, const DocumentFormat& format) {
    if (!document.is_object()) {
        return makeError(ErrorCode::ParseError, "a {} must be a JSON object, found {}", format.kind,
                         jsonTypeName(document));
    }

    CORDITE_TRY_ASSIGN(const std::optional<std::string> name, jsonGetOptional<std::string>(document, "format"));
    if (!name || *name != format.name) {
        return makeError(ErrorCode::ParseError, "not a Cordite {}: 'format' must be \"{}\"", format.kind, format.name);
    }

    const auto versionMember = document.find("version");
    if (versionMember == document.end()) {
        return makeError(ErrorCode::ParseError, "the {} has no 'version'", format.kind);
    }
    uint32_t version = 0;
    if (const Status status = PropertyCodec<uint32_t>::fromJson(*versionMember, version); !status) {
        return makeError(ErrorCode::ParseError, "'version': {}", status.error().message);
    }
    if (version == 0 || version > format.version) {
        return makeError(ErrorCode::VersionMismatch,
                         "the {} has format version {}, but this engine reads versions 1 to {}", format.kind, version,
                         format.version);
    }
    return version;
}

Status rejectUnknownMembers(const Json& object, std::span<const std::string_view> allowed) {
    for (const auto& [key, value] : object.items()) {
        if (std::ranges::find(allowed, std::string_view(key)) == allowed.end()) {
            return makeError(ErrorCode::ParseError, "unknown member '{}'", key);
        }
    }
    return {};
}

} // namespace cordite
