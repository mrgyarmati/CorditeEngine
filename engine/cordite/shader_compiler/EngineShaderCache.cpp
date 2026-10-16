#include "cordite/shader_compiler/EngineShaderCache.h"

#include "cordite/shader_compiler/SlangShaderCompiler.h"
#include "cordite/shaders/EngineShaders.h"
#include "cordite/shaders/ShaderLibrary.h"

#include <utility>

namespace cordite {

Result<std::unique_ptr<IShaderCompiler>> makeSlangShaderCompiler(const std::filesystem::path& sourceDirectory) {
    CORDITE_TRY_ASSIGN(std::unique_ptr<SlangShaderCompiler> compiler,
                       SlangShaderCompiler::create({.sourceRoots = {sourceDirectory}}));
    return std::unique_ptr<IShaderCompiler>(std::move(compiler));
}

Status compileEngineShaders(const std::filesystem::path& sourceDirectory, const std::filesystem::path& cacheDirectory) {
    CORDITE_TRY_ASSIGN(const std::unique_ptr<SlangShaderCompiler> compiler,
                       SlangShaderCompiler::create({.sourceRoots = {sourceDirectory}}));
    // Through a library: the cache keeps each permutation as a library looks for it.
    ShaderLibrary library(
        {.sourceRoots = {sourceDirectory}, .cacheDirectory = cacheDirectory, .compiler = compiler.get()});
    for (const ShaderRequest& request : engineShaderRequests()) {
        CORDITE_TRY(library.load(request));
    }
    return {};
}

} // namespace cordite
