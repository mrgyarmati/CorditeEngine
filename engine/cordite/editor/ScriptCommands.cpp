#include "cordite/editor/ScriptCommands.h"

#include "cordite/core/Document.h"
#include "cordite/core/Json.h"
#include "cordite/scripting/ScriptRuntime.h"

#include <optional>
#include <string>

namespace cordite {

Status registerScriptCommands(CommandRegistry& commands, ScriptSystem& scripts) {
    using Type = CommandParameterType;

    return commands.add({
        .name = "script.api",
        .description = "Returns what gameplay scripts are written against: `functions`, everything a script can "
                       "call, each with its name, how it is called and what it does; and `callbacks`, the functions "
                       "the engine calls in a script, in the order a frame calls them.",
        .parameters = {{"prefix", Type::String,
                        "Only the functions whose names begin with this, such as \"cordite.physics.\" or "
                        "\"cordite.Entity.\"; all of them when left out. The callbacks are always returned.",
                        false}},
        .handler = [&scripts](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const std::optional<std::string> prefix,
                               jsonGetOptional<std::string>(parameters, "prefix"));
            Json functions = Json::array();
            for (const ScriptFunctionInfo& function : scripts.runtime().functions()) {
                if (prefix && !function.name.starts_with(*prefix)) {
                    continue;
                }
                Json described = Json::object();
                described["name"] = function.name;
                described["signature"] = function.signature;
                described["description"] = function.description;
                functions.push_back(std::move(described));
            }
            Json callbacks = Json::array();
            for (const ScriptCallbackInfo& callback : scripts.callbacks()) {
                Json described = Json::object();
                described["name"] = std::string(callback.name);
                described["signature"] = std::string(callback.signature);
                described["description"] = std::string(callback.description);
                callbacks.push_back(std::move(described));
            }
            Json api = Json::object();
            api["functions"] = std::move(functions);
            api["callbacks"] = std::move(callbacks);
            return api;
        },
    });
}

} // namespace cordite
