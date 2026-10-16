#include "cordite/editor/GameExport.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/StringUtils.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Engine.h"
#include "cordite/shader_compiler/EngineShaderCache.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Names that no program of a game may have, whatever their case: what Windows keeps for
/// devices - a file of such a name is no file there, with any ending - and the directories
/// of a game beside its program, which on systems where programs have no ending would be
/// the same name.
constexpr std::array<std::string_view, 32> kTakenNames = {
    "con",  "prn",  "aux",  "nul",  "com0", "com1", "com2", "com3", "com4",   "com5",    "com6",
    "com7", "com8", "com9", "com¹", "com²", "com³", "lpt0", "lpt1", "lpt2",   "lpt3",    "lpt4",
    "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", "lpt¹", "lpt²", "lpt³", "assets", "licenses"};

/// What an export writes down of itself, in the cache directory of the game: that the
/// directory is a game it made, which another export may replace, and what its program is.
constexpr std::string_view kExportRecordFile = "export.json";
constexpr std::string_view kExportRecordFormat = "cordite.export";
constexpr int64_t kExportRecordVersion = 1;

/// What an export writes into `destination` for a game whose program is called `program`:
/// what goes when it fails, and when the game is exported there again.
std::vector<std::filesystem::path> gamePaths(const std::filesystem::path& destination, std::string_view program) {
    const Project game{.directory = destination};
    return {destination / pathFromUtf8(program), destination / "licenses", game.file(), game.assetDirectory(),
            game.cacheDirectory()};
}

/// The program of the game that an export made in `destination`, as the export wrote it
/// down; nullopt when no export made what is there.
std::optional<std::string> exportedProgram(const std::filesystem::path& destination) {
    const Project game{.directory = destination};
    const std::filesystem::path recordFile = game.cacheDirectory() / pathFromUtf8(kExportRecordFile);
    if (!fileExists(recordFile)) {
        return std::nullopt;
    }
    const Result<Json> record = readJsonFile(recordFile);
    if (!record || jsonGetOr<std::string>(*record, "format", "") != kExportRecordFormat) {
        return std::nullopt;
    }
    auto program = jsonGetOr<std::string>(*record, "program", "");
    // Read as anything in a file is: a name in the directory, not a way out of it.
    if (program.empty() || program == "." || program == ".." || program.find_first_of("/\\:") != std::string::npos) {
        return std::nullopt;
    }
    return program;
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

/// Refuses a destination that a game cannot be put into, and says what is in the way.
Result<Destination> checkDestination(const Project& project, const std::filesystem::path& destination) {
    if (destination.empty()) {
        return makeError(ErrorCode::InvalidArgument, "say where the game goes");
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
    if (const std::optional<std::string> program = exportedProgram(destination)) {
        return Destination{.previous = gamePaths(destination, *program)};
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

/// Writes the files of a game: everything but its checks.
Status writeGame(const Project& project, const GameExportDesc& desc, const std::filesystem::path& player,
                 const std::filesystem::path& shaders, GameExport& made) {
    CORDITE_TRY(copyFile(player, made.program, made));
    CORDITE_TRY(copyTree(player.parent_path() / "licenses", desc.destination / "licenses", made));
    const Project game{.directory = desc.destination, .settings = project.settings};
    CORDITE_TRY(copyFile(project.file(), game.file(), made));
    CORDITE_TRY(copyTree(project.assetDirectory(), game.assetDirectory(), made));

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
    record["program"] = pathToUtf8(made.program.filename());
    const std::filesystem::path recordFile = game.cacheDirectory() / pathFromUtf8(kExportRecordFile);
    CORDITE_TRY(writeJsonFile(recordFile, record));
    countFile(recordFile, made);
    return {};
}

} // namespace

std::string gameProgramName(std::string_view projectName) {
    std::string name;
    for (const char character : projectName) {
        const auto byte = static_cast<unsigned char>(character);
        // Letters of other alphabets are bytes above 127 in UTF-8, and welcome.
        if (byte >= 0x80 || std::isalnum(byte) != 0 || character == ' ' || character == '-' || character == '_') {
            name += character;
        }
    }
    name = std::string(trim(name));
    if (name.empty() || std::ranges::find(kTakenNames, toLowerAscii(name)) != kTakenNames.end()) {
        name = "Game";
    }
    return executableFileName(name);
}

Result<GameExport> exportGame(const Project& project, const GameExportDesc& desc) {
    if (!directoryExists(project.assetDirectory())) {
        return makeError(ErrorCode::NotFound, "the project {} has no asset directory, {}",
                         pathToUtf8(project.directory), pathToUtf8(project.assetDirectory()));
    }
    CORDITE_TRY_ASSIGN(const Destination destination, checkDestination(project, desc.destination));
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
    GameExport game{.program = desc.destination / pathFromUtf8(program)};
    if (const Status written = writeGame(project, desc, player, shaders, game); !written) {
        // A game that could not be made is not left half made.
        takeAway(destination.made.empty() ? gamePaths(desc.destination, program)
                                          : std::vector<std::filesystem::path>{destination.made});
        return tl::unexpected(written.error());
    }
    CORDITE_LOG_INFO("Editor", "made {} of {}: {} files, {} bytes", pathToUtf8(game.program),
                     pathToUtf8(project.directory), game.files, game.bytes);
    return game;
}

} // namespace cordite
