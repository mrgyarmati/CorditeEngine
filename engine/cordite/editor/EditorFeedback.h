#pragma once

/// @file
/// How the editor's user interface tells the user that something did not work.
///
/// A panel cannot return an error to anyone: it was called to draw a frame. An edit it
/// attempted and that failed is logged instead, which makes it appear in the console -
/// and brings the console to the front of the panels it shares its place with.
///
/// @code
/// if (ImGui::MenuItem("Delete")) {
///     reportIfFailed(EditorActions(context).deleteEntities(context.selection()));
/// }
/// @endcode

#include "cordite/core/Log.h"
#include "cordite/core/Result.h"

#include <string_view>

namespace cordite {

/// The categories of the log under which the editor answers the user: what its interface
/// could not do, and what a line of the console did or could not do.
inline constexpr std::string_view kInterfaceLogCategory = "Editor";
inline constexpr std::string_view kConsoleLogCategory = "Console";

/// Logs the error of a failed operation under the "Editor" category. Returns true when
/// the operation succeeded, so the caller can go on only in that case.
template <typename T>
bool reportIfFailed(const Result<T>& result) {
    if (!result) {
        CORDITE_LOG_ERROR(kInterfaceLogCategory, "{}", result.error().message);
    }
    return result.has_value();
}

/// Whether a message of the log says that something the user asked for was not done: an
/// error from the interface (reportIfFailed) or from a line of the console. It is what
/// the console has to be seen for; an error of the game that runs - a script, an asset -
/// is not, or a game that logs one in every frame would never let go of the console.
[[nodiscard]] inline bool reportsFailedAction(const LogMessage& message) {
    return (message.level == LogLevel::Error || message.level == LogLevel::Critical) &&
           (message.category == kInterfaceLogCategory || message.category == kConsoleLogCategory);
}

} // namespace cordite
