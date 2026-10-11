#pragma once

/// @file
/// How the editor's user interface tells the user that something did not work.
///
/// A panel cannot return an error to anyone: it was called to draw a frame. An edit it
/// attempted and that failed is logged instead, which makes it appear in the console.
///
/// @code
/// if (ImGui::MenuItem("Delete")) {
///     reportIfFailed(EditorActions(context).deleteEntities(context.selection()));
/// }
/// @endcode

#include "cordite/core/Log.h"
#include "cordite/core/Result.h"

namespace cordite {

/// Logs the error of a failed operation under the "Editor" category. Returns true when
/// the operation succeeded, so the caller can go on only in that case.
template <typename T>
bool reportIfFailed(const Result<T>& result) {
    if (!result) {
        CORDITE_LOG_ERROR("Editor", "{}", result.error().message);
    }
    return result.has_value();
}

} // namespace cordite
