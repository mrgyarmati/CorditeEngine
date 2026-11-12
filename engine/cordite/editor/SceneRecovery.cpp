#include "cordite/editor/SceneRecovery.h"

#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"

#include <array>
#include <string>
#include <string_view>

namespace cordite {
namespace {

constexpr DocumentFormat kRecoveryFormat{.name = "cordite.recovery", .version = 1, .kind = "kept scene"};
constexpr std::array<std::string_view, 4> kMembers = {"format", "version", "scenePath", "scene"};

} // namespace

Status writeSceneRecovery(const std::filesystem::path& file, const SceneRecovery& recovery) {
    Json document = makeDocument(kRecoveryFormat);
    document["scenePath"] = pathToUtf8(recovery.scenePath);
    document["scene"] = recovery.scene;
    return writeJsonFile(file, document);
}

Result<SceneRecovery> readSceneRecovery(const std::filesystem::path& file) {
    if (!fileExists(file)) {
        return makeError(ErrorCode::NotFound, "there is no kept scene at {}", pathToUtf8(file));
    }
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(file));
    const auto parse = [&document]() -> Result<SceneRecovery> {
        CORDITE_TRY(readDocumentHeader(document, kRecoveryFormat));
        CORDITE_TRY(rejectUnknownMembers(document, kMembers));
        SceneRecovery recovery;
        if (const auto path = document.find("scenePath"); path != document.end()) {
            if (!path->is_string()) {
                return makeError(ErrorCode::ParseError, "'scenePath' must be the path of a scene file, got {}",
                                 dumpJson(*path));
            }
            recovery.scenePath = pathFromUtf8(path->get_ref<const std::string&>());
        }
        const auto scene = document.find("scene");
        if (scene == document.end() || !scene->is_object()) {
            return makeError(ErrorCode::ParseError, "'scene' must be the scene that was kept");
        }
        recovery.scene = *scene;
        return recovery;
    };
    Result<SceneRecovery> recovery = parse();
    if (!recovery) {
        return tl::unexpected(recovery.error().withContext(pathToUtf8(file)));
    }
    return recovery;
}

} // namespace cordite
