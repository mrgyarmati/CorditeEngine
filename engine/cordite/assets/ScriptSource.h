#pragma once

/// @file
/// A script as an asset: the text of a `.lua` file.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const ScriptSource> door, assets->loadScript(doorScriptId));
/// CORDITE_LOG_INFO("Game", "{} bytes of Lua", door->text.size());
/// @endcode
///
/// The asset is the text and nothing else. What the text means - whether it is Lua at
/// all - is for whoever runs it to find out: the scripting module, which the assets
/// know nothing of.

#include <cstddef>
#include <string>

namespace cordite {

/// The most a script file may hold, in bytes. Far more than anyone writes by hand; a
/// file that is larger is something else under the wrong name.
inline constexpr size_t kMaxScriptBytes = size_t{4} * 1024 * 1024;

struct ScriptSource {
    /// The file, as it is.
    std::string text;
};

} // namespace cordite
