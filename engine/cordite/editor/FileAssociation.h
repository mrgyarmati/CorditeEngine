#pragma once

/// @file
/// Opening a file with a double click: the type of a file, by its extension, is told
/// which program opens it. The editor tells the system that project files open with it.
///
/// @code
/// const FileAssociation projects = projectFileAssociation(executablePath());
/// CORDITE_TRY_ASSIGN(const bool done, isAssociated(projects));
/// if (!done) {
///     CORDITE_TRY(associate(projects));      // project.cordite now opens in this editor
/// }
/// @endcode
///
/// On Windows a type is registered for the person who runs the program, in
/// HKEY_CURRENT_USER\Software\Classes: no administrator is asked, and nobody else's
/// files change. Linux and macOS learn about types from what an installation puts in
/// place - a .desktop file, the Info.plist of an application - which packaging is to
/// do; there every function here fails with Unsupported.

#include "cordite/core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cordite {

/// Where types of files are registered on Windows, under HKEY_CURRENT_USER: the person's
/// own. Tests give a key of their own instead, which they delete afterwards.
inline constexpr std::string_view kUserFileClasses = "Software\\Classes";

/// A type of file and the program that opens it.
struct FileAssociation {
    /// With its dot: ".cordite".
    std::string extension;
    /// What the type is registered by: "Cordite.Project".
    std::string programId;
    /// What the type is called where files are listed: "Cordite project".
    std::string description;
    /// The program that opens a file of the type, given its path as its only argument.
    std::filesystem::path program;
};

/// Project files (project.cordite) opened by `editor`, the executable of the editor.
[[nodiscard]] FileAssociation projectFileAssociation(const std::filesystem::path& editor);

/// The command line with which the system opens a file with `program`: the program and
/// the file, each in quotes - "C:\Program Files\Cordite\cordite-editor.exe" "%1" - since
/// either may have spaces in it.
[[nodiscard]] std::string openCommand(const std::filesystem::path& program);

/// Whether files of the type open with the program, for the person who runs this one.
/// Fails with Unsupported on platforms other than Windows, and with InvalidArgument for
/// an association that is no type of file (an extension without a dot, an empty id).
[[nodiscard]] Result<bool> isAssociated(const FileAssociation& association,
                                        std::string_view classes = kUserFileClasses);

/// Makes files of the type open with the program, for the person who runs this one,
/// whatever opened them before, and tells the shell. Fails like isAssociated, and with
/// IoError when the registry refuses.
[[nodiscard]] Status associate(const FileAssociation& association, std::string_view classes = kUserFileClasses);

/// Undoes associate(): the type is no longer registered by the id, and the extension no
/// longer names it - unless it names another type by then, which is left alone. Succeeds
/// when there was nothing to undo. Fails like associate().
[[nodiscard]] Status dissociate(const FileAssociation& association, std::string_view classes = kUserFileClasses);

} // namespace cordite
