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
/// made it, with the name of its program: a game is exported into the same directory again
/// by replacing what the record names, and nothing else. The game runs on the kind of
/// system that exported it: its program is the one of this build.
///
/// @code
/// CORDITE_TRY_ASSIGN(const Project project, loadProject("samples/showcase"));
/// CORDITE_TRY_ASSIGN(const GameExport made, exportGame(project, {.destination = "out/Showcase"}));
/// // made.program: out/Showcase/Showcase.exe on Windows, out/Showcase/Showcase elsewhere
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/engine/Project.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace cordite {

struct GameExportDesc {
    /// The directory the game goes into, outside the project: one that is not there yet, is
    /// empty, or holds a game that an export made - what that export wrote is replaced, and
    /// nothing else there is touched.
    std::filesystem::path destination;
    /// The sources of the engine's shaders, compiled for the game. Empty: where the engine
    /// finds them (engineShaderDirectory()).
    std::filesystem::path shaderDirectory;
    /// The program of a game, with the `licenses` directory beside it that the build puts
    /// there. Empty: cordite-player beside the program that is running.
    std::filesystem::path player;
};

/// What exportGame() wrote.
struct GameExport {
    /// The program of the game, under its name.
    std::filesystem::path program;
    /// How many files were written, and how many bytes they hold.
    uint64_t files = 0;
    uint64_t bytes = 0;
};

/// The file name of the program of a game called `projectName`: its letters, digits,
/// spaces, dashes and underscores - letters of every alphabet - without blanks at the ends,
/// with what a program is called on this system ("Showcase.exe" on Windows). "Game" when
/// nothing is left of the name, or what is left is what Windows keeps for a device ("con",
/// "lpt1") or names a directory beside the program ("assets", "licenses"), in any case.
[[nodiscard]] std::string gameProgramName(std::string_view projectName);

/// Writes the game of `project` into `desc.destination` - see the file comment - and checks
/// first what can be checked: fails with InvalidArgument for no destination or one inside
/// the project, with AlreadyExists for a file or a directory that holds what no export
/// made, and with NotFound for a project without its asset directory and for a program,
/// its notices or shader sources that are not there. Then it takes away what an export
/// wrote there before, and fails with IoError for a file that cannot be taken away or
/// copied, and as compileEngineShaders() does; what it wrote is taken away again then - the
/// directories it made, or what it put into the directory that was there. The project is
/// not changed.
[[nodiscard]] Result<GameExport> exportGame(const Project& project, const GameExportDesc& desc);

} // namespace cordite
