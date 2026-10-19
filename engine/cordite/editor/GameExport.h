#pragma once

/// @file
/// Making a game of a project: a directory to hand to the people who play it.
///
/// What an export writes is what the program of a game needs and nothing else: the program
/// - cordite-player, under the name of the game - the project file, the asset directory as
/// it is (with the .meta files, which hold the ids), the engine's shaders compiled into the
/// game's cache (compileEngineShaders()), and in `licenses/` the notices of the software
/// the program is made of. No editor, no shader compiler, no sources of shaders, and
/// nothing of the project's own cache. Last, a record in the game's cache that an export
/// made it, how it is laid out and what its program is called: a game is exported into the
/// same place again by replacing what the record names, and nothing else. The game runs on
/// the kind of system that exported it: its program is the one of this build.
///
/// On Windows and Linux all of that is side by side in the destination. On macOS a game is
/// an application bundle (GameLayout::Bundle): the destination is the bundle, `<name>.app`,
/// with the program in Contents/MacOS, what the system is told of it in
/// Contents/Info.plist (bundleInfoPlist()), the Vulkan it draws with in Contents/Frameworks
/// (MoltenVK, kBundledVulkanLibrary, which the build puts beside the player), and the rest
/// in Contents/Resources, where the program looks for its game (gameDirectoryOf()).
///
/// @code
/// CORDITE_TRY_ASSIGN(const Project project, loadProject("samples/showcase"));
/// CORDITE_TRY_ASSIGN(const GameExport made, exportGame(project, {.destination = "out/Showcase"}));
/// // made.program: out/Showcase/Showcase.exe on Windows, out/Showcase/Showcase on Linux
///
/// // On macOS - or anywhere, laid out so - a bundle:
/// CORDITE_TRY_ASSIGN(const GameExport app, exportGame(project, {.destination = "out/Showcase.app",
///                                                                .layout = GameLayout::Bundle}));
/// // app.program: out/Showcase.app/Contents/MacOS/Showcase
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/engine/Project.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {

/// How a game is laid out where it is exported to.
enum class GameLayout : uint8_t {
    /// The program, the project, its assets, its shaders and the notices side by side in
    /// the destination: a game of Windows or Linux.
    Directory,
    /// An application bundle of macOS: the destination is the bundle, its name ending in
    /// ".app" - the program in Contents/MacOS, Contents/Info.plist, and the rest of the
    /// game in Contents/Resources.
    Bundle,
};

/// "directory" and "bundle": what the record of an export says.
[[nodiscard]] std::string_view nameOf(GameLayout layout);

/// How the games of the system that runs this are laid out: a bundle on macOS, a
/// directory elsewhere.
[[nodiscard]] GameLayout nativeGameLayout();

/// Where a game laid out as `layout` goes when a person names `path`: there - for a bundle
/// with ".app" added to a name that does not end so already, which is what a bundle is
/// called ("out/Depot" is "out/Depot.app").
[[nodiscard]] std::filesystem::path gameDestinationFor(const std::filesystem::path& path, GameLayout layout);

struct GameExportDesc {
    /// Where the game goes, outside the project: a directory that is not there yet, is
    /// empty, or holds a game that an export made laid out the same way - what that export
    /// wrote is replaced, and nothing else there is touched. For a bundle, the bundle
    /// itself: a directory whose name ends in ".app".
    std::filesystem::path destination;
    /// The sources of the engine's shaders, compiled for the game. Empty: where the engine
    /// finds them (engineShaderDirectory()).
    std::filesystem::path shaderDirectory;
    /// The program of a game, with the `licenses` directory beside it that the build puts
    /// there - and on macOS MoltenVK, which a bundle brings; without it the game draws with
    /// the Vulkan of the system, which is said. Empty: cordite-player beside the program
    /// that is running.
    std::filesystem::path player;
    /// How the game is laid out. Empty: as the games of this system are
    /// (nativeGameLayout()).
    std::optional<GameLayout> layout;
};

/// What exportGame() wrote.
struct GameExport {
    /// The program of the game, under its name.
    std::filesystem::path program;
    /// Where the game is: the destination, or the Contents/Resources of a bundle - what the
    /// program plays.
    std::filesystem::path game;
    /// How many files were written, and how many bytes they hold.
    uint64_t files = 0;
    uint64_t bytes = 0;
};

/// The file name of the program of a game called `projectName`: gameFileName(), as a
/// program is called on this system ("Showcase.exe" on Windows).
[[nodiscard]] std::string gameProgramName(std::string_view projectName);

/// What the Info.plist of the application bundle of a game says of it to macOS: its name
/// (ProjectSettings::name, as it is), its program (`program`, the file in Contents/MacOS),
/// its identifier (gameBundleIdentifier()), that it is an application, that it draws for
/// screens of high resolution - and, when `minimumSystemVersion` is not empty, the oldest
/// macOS it runs on, which without it the system reads from the program. The text is XML,
/// what the names hold escaped.
[[nodiscard]] std::string bundleInfoPlist(const ProjectSettings& settings, std::string_view program,
                                          std::string_view minimumSystemVersion = {});

/// Writes the game of `project` into `desc.destination` - see the file comment - and checks
/// first what can be checked: fails with InvalidArgument for no destination, one inside the
/// project and a bundle whose name does not end in ".app", with AlreadyExists for a file,
/// a directory that holds what no export made and one that holds a game laid out the other
/// way, and with NotFound for a project without its asset directory and for a program, its
/// notices or shader sources that are not there. Then it takes away what an export
/// wrote there before, and fails with IoError for a file that cannot be taken away or
/// copied, and as compileEngineShaders() does; what it wrote is taken away again then - the
/// directories it made, or what it put into the directory that was there. The project is
/// not changed.
[[nodiscard]] Result<GameExport> exportGame(const Project& project, const GameExportDesc& desc);

} // namespace cordite
