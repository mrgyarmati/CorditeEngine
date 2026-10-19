#include "cordite/shader_compiler/EngineShaderCache.h"

#include "cordite/shader_compiler/SlangShaderCompiler.h"
#include "cordite/shaders/EngineShaders.h"
#include "cordite/shaders/ShaderLibrary.h"

#include <fmt/format.h>

#include <utility>

namespace cordite {

Result<std::unique_ptr<IShaderCompiler>> makeSlangShaderCompiler(const std::filesystem::path& sourceDirectory) {
    CORDITE_TRY_ASSIGN(std::unique_ptr<SlangShaderCompiler> compiler,
                       SlangShaderCompiler::create({.sourceRoots = {sourceDirectory}}));
    return std::unique_ptr<IShaderCompiler>(std::move(compiler));
}

std::vector<ShaderTarget> gameShaderTargets() {
    std::vector<ShaderTarget> targets;
    for (const ShaderTarget target : kShaderTargets) {
        if (canCompileTo(target)) {
            targets.push_back(target);
        }
    }
    return targets;
}

Status compileEngineShaders(const std::filesystem::path& sourceDirectory, const std::filesystem::path& cacheDirectory,
                            std::span<const ShaderTarget> targets) {
    CORDITE_TRY_ASSIGN(const std::unique_ptr<SlangShaderCompiler> compiler,
                       SlangShaderCompiler::create({.sourceRoots = {sourceDirectory}}));
    for (const ShaderTarget target : targets) {
        // Through a library: the cache keeps each permutation as a library looks for it.
        ShaderLibrary library({.sourceRoots = {sourceDirectory},
                               .cacheDirectory = cacheDirectory,
                               .compiler = compiler.get(),
                               .target = target});
        for (const ShaderRequest& request : engineShaderRequests()) {
            if (Result<std::shared_ptr<const ShaderBinary>> compiled = library.load(request); !compiled) {
                return tl::unexpected(compiled.error().withContext(fmt::format("for {}", nameOf(target))));
            }
        }
    }
    return {};
}

} // namespace cordite
