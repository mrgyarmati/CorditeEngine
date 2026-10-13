#pragma once

/// @file
/// The header every JSON document of the engine starts with.
///
/// Scenes, prefabs, materials, projects and asset descriptions all begin with a "format"
/// member that says what the file is and a "version" that says which revision of that
/// format it was written in. Checking both first turns "this is some other JSON file" and
/// "this was written by a newer engine" into clear messages instead of a list of unknown
/// members.
///
/// @code
/// constexpr DocumentFormat kMaterialFormat{.name = "cordite.material", .version = 1, .kind = "material"};
///
/// Json document = makeDocument(kMaterialFormat);      // {"format": ..., "version": 1}
/// document["roughness"] = 0.4;
///
/// CORDITE_TRY_ASSIGN(const uint32_t version, readDocumentHeader(loaded, kMaterialFormat));
/// CORDITE_TRY(rejectUnknownMembers(loaded, kMaterialMembers));
/// @endcode

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace cordite {

/// What kind of document a file is, and which revisions of it this engine reads.
struct DocumentFormat {
    /// The value of the "format" member, such as "cordite.scene".
    std::string_view name;
    /// The revision this engine writes, and the newest it reads. Revisions start at 1.
    uint32_t version = 1;
    /// What to call the document in messages: "scene", "material".
    std::string_view kind;
};

/// Starts a document: an object holding "format" and "version", in that order.
[[nodiscard]] Json makeDocument(const DocumentFormat& format);

/// Checks that `document` is an object whose "format" is the expected one and whose
/// "version" is a whole number between 1 and the format's. Returns the version. Fails
/// with ParseError for a document that is something else and with VersionMismatch for
/// one this engine is too old (or too new) to read.
[[nodiscard]] Result<uint32_t> readDocumentHeader(const Json& document, const DocumentFormat& format);

/// Fails when `object` has a member whose name is not in `allowed`, naming it. A
/// misspelled member would otherwise be dropped without a word, and its data with it.
[[nodiscard]] Status rejectUnknownMembers(const Json& object, std::span<const std::string_view> allowed);

} // namespace cordite
