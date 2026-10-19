#include "cordite/editor/GameExport.h"

#include "cordite/core/Base.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Engine.h"
#include "cordite/rhi/GraphicsDevice.h"
#include "cordite/shader_compiler/EngineShaderCache.h"

#include <fmt/format.h>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// What an export writes down of itself, in the cache directory of the game: that the
/// directory is a game it made, which another export may replace, how it is laid out and
/// what its program is. Version 1 said nothing of the layout: it made every game a
/// directory.
constexpr std::string_view kExportRecordFile = "export.json";
constexpr std::string_view kExportRecordFormat = "cordite.export";
constexpr int64_t kExportRecordVersion = 2;

/// The oldest macOS the program of a game runs on, as its build says - nothing when it
/// says nothing, and the system reads it from the program.
#if defined(CORDITE_MACOS_DEPLOYMENT_TARGET)
constexpr std::string_view kMinimumSystemVersion = CORDITE_MACOS_DEPLOYMENT_TARGET;
#else
constexpr std::string_view kMinimumSystemVersion;
#endif

GameLayout otherLayoutThan(GameLayout layout) {
    return layout == GameLayout::Bundle ? GameLayout::Directory : GameLayout::Bundle;
}

/// Where the game is in what an export writes into `destination`: there, or in the
/// resources of a bundle.
std::filesystem::path gameDirectoryIn(const std::filesystem::path& destination, GameLayout layout) {
    return layout == GameLayout::Bundle ? destination / "Contents" / "Resources" : destination;
}

/// Where the program called `program` is in what an export writes into `destination`.
std::filesystem::path programIn(const std::filesystem::path& destination, GameLayout layout, std::string_view program) {
    return layout == GameLayout::Bundle ? destination / "Contents" / "MacOS" / pathFromUtf8(program)
                                        : destination / pathFromUtf8(program);
}

/// What an export writes into `destination` for a game laid out so, whose program is
/// called `program`: what goes when it fails, and when the game is exported there again.
std::vector<std::filesystem::path> gamePaths(const std::filesystem::path& destination, GameLayout layout,
                                             std::string_view program) {
    // All of a bundle that an export writes is in its Contents.
    if (layout == GameLayout::Bundle) {
        return {destination / "Contents"};
    }
    const Project game{.directory = destination};
    return {destination / pathFromUtf8(program), destination / "licenses", game.file(), game.assetDirectory(),
            game.cacheDirectory()};
}

/// The program of the game that an export laid out as `layout` made in `destination`, as the
/// export wrote it down; nullopt when no export of that layout made what is there.
std::optional<std::string> exportedProgramIn(const std::filesystem::path& destination, GameLayout layout) {
    const Project game{.directory = gameDirectoryIn(destination, layout)};
    const std::filesystem::path recordFile = game.cacheDirectory() / pathFromUtf8(kExportRecordFile);
    if (!fileExists(recordFile)) {
        return std::nullopt;
    }
    const Result<Json> record = readJsonFile(recordFile);
    if (!record || jsonGetOr<std::string>(*record, "format", "") != kExportRecordFormat) {
        return std::nullopt;
    }
    if (jsonGetOr<std::string>(*record, "layout", std::string(nameOf(GameLayout::Directory))) != nameOf(layout)) {
        return std::nullopt;
    }
    auto program = jsonGetOr<std::string>(*record, "program", "");
    // Read as anything in a file is: a name in the directory, not a way out of it.
    if (program.empty() || program == "." || program == ".." || program.find_first_of("/\\:") != std::string::npos) {
        return std::nullopt;
    }
    return program;
}

/// `text` as XML holds it between tags.
std::string escapedForXml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

/// Lets the program of a game be run: on a system where a file has to say so, a copy may
/// not have kept it. Nothing changes where files say nothing of it.
Status makeRunnable(const std::filesystem::path& program) {
    std::error_code error;
    std::filesystem::permissions(program,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot make {} a program that runs: {}", pathToUtf8(program),
                         error.message());
    }
    return {};
}

/// Where a game goes, and what is in the way there.
struct Destination {
    /// The outermost directory that writing the game makes; empty when it is there.
    std::filesystem::path made;
    /// What an export wrote there before, which goes before the game is written anew.
    std::vector<std::filesystem::path> previous;
};

/// The outermost directory that writing into `path` makes: `path` itself, or a directory it
/// is in, that is not there. Empty when `path` is there, or whether it is cannot be told.
std::filesystem::path outermostMissing(const std::filesystem::path& path) {
    std::filesystem::path missing;
    std::filesystem::path candidate = path;
    while (!candidate.empty()) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error) || error) {
            break;
        }
        missing = candidate;
        std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = std::move(parent);
    }
    return missing;
}

/// Refuses a destination that a game laid out as `layout` cannot be put into, and says what
/// is in the way.
Result<Destination> checkDestination(const Project& project, const std::filesystem::path& destination,
                                     GameLayout layout) {
    if (destination.empty()) {
        return makeError(ErrorCode::InvalidArgument, "say where the game goes");
    }
    if (layout == GameLayout::Bundle && destination.extension() != ".app") {
        return makeError(ErrorCode::InvalidArgument,
                         "{} is no application bundle: a game laid out as one goes into a directory whose name ends "
                         "in .app",
                         pathToUtf8(destination));
    }
    // Made of the project and put into it, it would copy itself.
    std::error_code error;
    if (relativePathWithin(project.directory, destination) ||
        std::filesystem::equivalent(project.directory, destination, error)) {
        return makeError(ErrorCode::InvalidArgument, "{} is inside the project; a game goes somewhere else",
                         pathToUtf8(destination));
    }
    if (!std::filesystem::exists(destination, error)) {
        if (error) {
            return makeError(ErrorCode::IoError, "cannot tell whether {} is there: {}", pathToUtf8(destination),
                             error.message());
        }
        return Destination{.made = outermostMissing(destination)};
    }
    if (!std::filesystem::is_directory(destination, error)) {
        return makeError(ErrorCode::AlreadyExists, "{} is a file; a game goes into a directory",
                         pathToUtf8(destination));
    }
    if (std::filesystem::is_empty(destination, error) && !error) {
        return Destination{};
    }
    if (const std::optional<std::string> program = exportedProgramIn(destination, layout)) {
        return Destination{.previous = gamePaths(destination, layout, *program)};
    }
    // Laid out the other way, a game is no more to be replaced than what no export made -
    // but it is a game, and is said to be one.
    if (const GameLayout other = otherLayoutThan(layout); exportedProgramIn(destination, other)) {
        return makeError(ErrorCode::AlreadyExists,
                         "{} holds a game exported as a {}; export it as one there, or into another directory",
                         pathToUtf8(destination), nameOf(other));
    }
    return makeError(ErrorCode::AlreadyExists,
                     "{} holds something that no export made; a game goes into a directory of its own, or "
                     "replaces one that was exported there",
                     pathToUtf8(destination));
}

/// Takes away what is at `paths`, and says what could not be.
void takeAway(const std::vector<std::filesystem::path>& paths) {
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            CORDITE_LOG_WARN("Editor", "cannot take away {}: {}", pathToUtf8(path), error.message());
        }
    }
}

/// Adds a file that was written to what an export counts.
void countFile(const std::filesystem::path& file, GameExport& made) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(file, error);
    ++made.files;
    if (!error) {
        made.bytes += size;
    }
}

Status copyFile(const std::filesystem::path& from, const std::filesystem::path& to, GameExport& made) {
    CORDITE_TRY(ensureDirectory(to.parent_path()));
    std::error_code error;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot copy {} to {}: {}", pathToUtf8(from), pathToUtf8(to),
                         error.message());
    }
    countFile(to, made);
    return {};
}

/// Copies the files of a directory and of the directories in it, but for directories whose
/// names begin with a dot, which the assets do not count either.
Status copyTree(const std::filesystem::path& from, const std::filesystem::path& to, GameExport& made) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(
        from, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        std::error_code statusError;
        if (entry->is_directory(statusError)) {
            if (pathToUtf8(entry->path().filename()).starts_with('.')) {
                entry.disable_recursion_pending();
            }
            continue;
        }
        if (entry->is_regular_file(statusError)) {
            CORDITE_TRY(copyFile(entry->path(), to / entry->path().lexically_relative(from), made));
        }
    }
    if (error) {
        return makeError(ErrorCode::IoError, "cannot read {}: {}", pathToUtf8(from), error.message());
    }
    return {};
}

/// Counts the files a directory holds, with their sizes.
Status countTree(const std::filesystem::path& directory, GameExport& made) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(directory, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        std::error_code statusError;
        if (entry->is_regular_file(statusError)) {
            countFile(entry->path(), made);
        }
    }
    if (error) {
        return makeError(ErrorCode::IoError, "cannot read {}: {}", pathToUtf8(directory), error.message());
    }
    return {};
}

/// Writes the files of a game laid out as `layout`: everything but its checks.
Status writeGame(const Project& project, const GameExportDesc& desc, GameLayout layout,
                 const std::filesystem::path& player, const std::filesystem::path& shaders, GameExport& made) {
    CORDITE_TRY(copyFile(player, made.program, made));
    CORDITE_TRY(makeRunnable(made.program));
    CORDITE_TRY(copyTree(player.parent_path() / "licenses", made.game / "licenses", made));
    const Project game{.directory = made.game, .settings = project.settings};
    CORDITE_TRY(copyFile(project.file(), game.file(), made));
    CORDITE_TRY(copyTree(project.assetDirectory(), game.assetDirectory(), made));
    if (layout == GameLayout::Bundle) {
        const std::filesystem::path information = desc.destination / "Contents" / "Info.plist";
        CORDITE_TRY(writeTextFile(information, bundleInfoPlist(project.settings, pathToUtf8(made.program.filename()),
                                                               kMinimumSystemVersion)));
        countFile(information, made);
        // Vulkan, which macOS does not have: what the build put beside the player.
        const std::filesystem::path vulkan = player.parent_path() / pathFromUtf8(kBundledVulkanLibrary);
        if (fileExists(vulkan)) {
            CORDITE_TRY(copyFile(
                vulkan, desc.destination / "Contents" / "Frameworks" / pathFromUtf8(kBundledVulkanLibrary), made));
        } else {
            CORDITE_LOG_WARN("Editor",
                             "{} is not beside {}: the game draws with the Vulkan of the system it runs on, which a "
                             "Mac has only with the Vulkan SDK",
                             kBundledVulkanLibrary, pathToUtf8(player));
        }
    }

    // What the game has instead of a compiler: the engine's shaders, compiled, where its
    // engine looks for them.
    const std::filesystem::path cache = game.cacheDirectory() / "shaders";
    CORDITE_TRY(compileEngineShaders(shaders, cache));
    CORDITE_TRY(countTree(cache, made));

    // Last: a directory that says an export made it is one.
    Json record = Json::object();
    record["format"] = std::string(kExportRecordFormat);
    record["version"] = kExportRecordVersion;
    record["engine"] = std::string(kEngineVersion);
    record["layout"] = std::string(nameOf(layout));
    record["program"] = pathToUtf8(made.program.filename());
    const std::filesystem::path recordFile = game.cacheDirectory() / pathFromUtf8(kExportRecordFile);
    CORDITE_TRY(writeJsonFile(recordFile, record));
    countFile(recordFile, made);
    return {};
}

} // namespace

std::string_view nameOf(GameLayout layout) {
    switch (layout) {
        case GameLayout::Directory:
            return "directory";
        case GameLayout::Bundle:
            return "bundle";
    }
    return "unknown";
}

GameLayout nativeGameLayout() {
#if CORDITE_PLATFORM_MACOS
    return GameLayout::Bundle;
#else
    return GameLayout::Directory;
#endif
}

std::filesystem::path gameDestinationFor(const std::filesystem::path& path, GameLayout layout) {
    if (layout != GameLayout::Bundle || path.empty() || path.extension() == ".app") {
        return path;
    }
    // A name that ends in a separator names the directory before it.
    std::filesystem::path bundle = path.filename().empty() ? path.parent_path() : path;
    bundle += ".app";
    return bundle;
}

std::string gameProgramName(std::string_view projectName) {
    return executableFileName(gameFileName(projectName));
}

std::string bundleInfoPlist(const ProjectSettings& settings, std::string_view program,
                            std::string_view minimumSystemVersion) {
    std::string keys;
    const auto add = [&keys](std::string_view key, std::string_view value) {
        keys += fmt::format("    <key>{}</key>\n    <string>{}</string>\n", key, escapedForXml(value));
    };
    add("CFBundleDevelopmentRegion", "en");
    add("CFBundleDisplayName", settings.name);
    add("CFBundleExecutable", program);
    add("CFBundleIdentifier", gameBundleIdentifier(settings));
    add("CFBundleInfoDictionaryVersion", "6.0");
    add("CFBundleName", settings.name);
    add("CFBundlePackageType", "APPL");
    if (!minimumSystemVersion.empty()) {
        add("LSMinimumSystemVersion", minimumSystemVersion);
    }
    keys += "    <key>NSHighResolutionCapable</key>\n    <true/>\n";
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n" +
           keys + "</dict>\n</plist>\n";
}

Result<GameExport> exportGame(const Project& project, const GameExportDesc& desc) {
    if (!directoryExists(project.assetDirectory())) {
        return makeError(ErrorCode::NotFound, "the project {} has no asset directory, {}",
                         pathToUtf8(project.directory), pathToUtf8(project.assetDirectory()));
    }
    const GameLayout layout = desc.layout.value_or(nativeGameLayout());
    CORDITE_TRY_ASSIGN(const Destination destination, checkDestination(project, desc.destination, layout));
    const std::filesystem::path player =
        desc.player.empty() ? executableDirectory() / pathFromUtf8(executableFileName("cordite-player")) : desc.player;
    if (!fileExists(player)) {
        return makeError(ErrorCode::NotFound, "the program of a game, {}, is not there: build cordite-player",
                         pathToUtf8(player));
    }
    if (!directoryExists(player.parent_path() / "licenses")) {
        return makeError(ErrorCode::NotFound,
                         "the notices of the software a game is made of are not beside {}: build it again",
                         pathToUtf8(player));
    }
    CORDITE_TRY_ASSIGN(const std::filesystem::path shaders, engineShaderDirectory(desc.shaderDirectory));

    // A game that is exported again replaces what the export wrote before, and nothing else.
    for (const std::filesystem::path& path : destination.previous) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            return makeError(ErrorCode::IoError, "cannot take away {} of the game that was exported there: {}",
                             pathToUtf8(path), error.message());
        }
    }

    const std::string program = gameProgramName(project.settings.name);
    GameExport game{.program = programIn(desc.destination, layout, program),
                    .game = gameDirectoryIn(desc.destination, layout)};
    if (const Status written = writeGame(project, desc, layout, player, shaders, game); !written) {
        // A game that could not be made is not left half made.
        takeAway(destination.made.empty() ? gamePaths(desc.destination, layout, program)
                                          : std::vector<std::filesystem::path>{destination.made});
        return tl::unexpected(written.error());
    }
    CORDITE_LOG_INFO("Editor", "made {} of {}: {} files, {} bytes", pathToUtf8(game.program),
                     pathToUtf8(project.directory), game.files, game.bytes);
    return game;
}

} // namespace cordite
