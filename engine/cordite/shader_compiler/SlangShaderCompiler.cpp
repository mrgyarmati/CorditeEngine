#include "cordite/shader_compiler/SlangShaderCompiler.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Hash.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace cordite {
namespace {

/// Bumped when the options below change in a way that alters the generated code, so that
/// shaders cached with the old options are rebuilt.
constexpr int kOptionsRevision = 1;

SlangStage toSlangStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return SLANG_STAGE_VERTEX;
        case ShaderStage::Pixel:
            return SLANG_STAGE_FRAGMENT;
        case ShaderStage::Compute:
            return SLANG_STAGE_COMPUTE;
    }
    return SLANG_STAGE_NONE;
}

std::string blobText(slang::IBlob* blob) {
    if (blob == nullptr || blob->getBufferSize() == 0) {
        return {};
    }
    const auto* text = static_cast<const char*>(blob->getBufferPointer());
    return std::string(trim(std::string_view(text, blob->getBufferSize())));
}

/// Slang reports warnings through the same channel as errors. When a step succeeded,
/// whatever it printed is a warning worth showing but not a failure.
void logWarnings(slang::IBlob* diagnostics, const ShaderRequest& request) {
    const std::string text = blobText(diagnostics);
    if (!text.empty()) {
        CORDITE_LOG_WARN("Shaders", "{}:\n{}", request.describe(), text);
    }
}

tl::unexpected<Error> compileError(slang::IBlob* diagnostics, std::string_view fallback) {
    const std::string text = blobText(diagnostics);
    return makeError(ErrorCode::ParseError, "{}", text.empty() ? fallback : std::string_view(text));
}

} // namespace

struct SlangShaderCompiler::Impl {
    SlangShaderCompilerDesc desc;
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    std::string version;
    // The global session and the sessions created from it are not thread-safe.
    std::mutex mutex;

    Result<Slang::ComPtr<slang::ISession>> createSession(const ShaderRequest& request);
    [[nodiscard]] std::filesystem::path resolveSource(std::string_view relativePath) const;
    [[nodiscard]] std::string toRootRelative(const std::filesystem::path& path) const;
};

std::filesystem::path SlangShaderCompiler::Impl::resolveSource(std::string_view relativePath) const {
    const std::filesystem::path relative = pathFromUtf8(relativePath);
    for (const std::filesystem::path& root : desc.sourceRoots) {
        std::filesystem::path candidate = root / relative;
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

/// Dependencies are recorded relative to a shader root so that a cache stays valid when
/// the project directory moves. A file outside every root keeps its full path.
std::string SlangShaderCompiler::Impl::toRootRelative(const std::filesystem::path& path) const {
    for (const std::filesystem::path& root : desc.sourceRoots) {
        if (Result<std::string> relative = relativePathWithin(root, path)) {
            return std::move(*relative);
        }
    }
    return pathToUtf8(path);
}

Result<Slang::ComPtr<slang::ISession>> SlangShaderCompiler::Impl::createSession(const ShaderRequest& request) {
    // The options are given in slangc's command-line syntax and parsed by Slang itself,
    // which keeps this code independent of the numeric option identifiers in slang.h.
    std::vector<std::string> arguments = {
        "-target",
        "spirv",
        "-profile",
        "spirv_1_5",
        "-matrix-layout-column-major",
        // NVRHI's default mapping from HLSL registers to Vulkan bindings.
        "-fvk-t-shift",
        "0",
        "all",
        "-fvk-s-shift",
        "128",
        "all",
        "-fvk-b-shift",
        "256",
        "all",
        "-fvk-u-shift",
        "384",
        "all",
        desc.optimize ? "-O2" : "-O0",
    };
    if (desc.debugInfo) {
        arguments.emplace_back("-g2");
    }
    for (const std::filesystem::path& root : desc.sourceRoots) {
        arguments.emplace_back("-I");
        arguments.push_back(pathToUtf8(root));
    }
    for (const ShaderDefine& define : request.defines) {
        arguments.emplace_back("-D");
        arguments.push_back(fmt::format("{}={}", define.name, define.value));
    }

    std::vector<const char*> argumentPointers;
    argumentPointers.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argumentPointers.push_back(argument.c_str());
    }

    slang::SessionDesc sessionDesc;
    // Owns the memory that the parsed session description points into.
    Slang::ComPtr<ISlangUnknown> sessionDescStorage;
    if (SLANG_FAILED(globalSession->parseCommandLineArguments(static_cast<int>(argumentPointers.size()),
                                                              argumentPointers.data(), &sessionDesc,
                                                              sessionDescStorage.writeRef()))) {
        return makeError(ErrorCode::InvalidState, "Slang rejected the engine's compiler options");
    }

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef()))) {
        return makeError(ErrorCode::InvalidState, "cannot create a Slang compilation session");
    }
    return session;
}

Result<std::unique_ptr<SlangShaderCompiler>> SlangShaderCompiler::create(const SlangShaderCompilerDesc& desc) {
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;

    if (SLANG_FAILED(slang::createGlobalSession(impl->globalSession.writeRef())) || !impl->globalSession) {
        return makeError(ErrorCode::Unsupported, "cannot initialize the Slang shader compiler");
    }

    impl->version = fmt::format("slang-{} options-{} {}{}", impl->globalSession->getBuildTagString(), kOptionsRevision,
                                desc.optimize ? "O2" : "O0", desc.debugInfo ? " g2" : "");
    return std::unique_ptr<SlangShaderCompiler>(new SlangShaderCompiler(std::move(impl)));
}

SlangShaderCompiler::SlangShaderCompiler(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

SlangShaderCompiler::~SlangShaderCompiler() = default;

std::string SlangShaderCompiler::version() const {
    return m_impl->version;
}

Result<ShaderBinary> SlangShaderCompiler::compile(const ShaderRequest& request) {
    const std::lock_guard lock(m_impl->mutex);

    const std::filesystem::path sourcePath = m_impl->resolveSource(request.path);
    if (sourcePath.empty()) {
        return makeError(ErrorCode::NotFound, "shader source '{}' was not found in any shader directory", request.path);
    }

    CORDITE_TRY_ASSIGN(const Slang::ComPtr<slang::ISession> session, m_impl->createSession(request));

    // Passing no source text makes Slang read the file itself, so that relative
    // #include directives resolve against the file's own directory.
    const std::string sourcePathText = pathToUtf8(sourcePath);
    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module =
        session->loadModuleFromSource(request.path.c_str(), sourcePathText.c_str(), nullptr, diagnostics.writeRef());
    if (module == nullptr) {
        return compileError(diagnostics, "the shader module could not be loaded");
    }
    logWarnings(diagnostics, request);

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    diagnostics = nullptr;
    if (SLANG_FAILED(module->findAndCheckEntryPoint(request.entryPoint.c_str(), toSlangStage(request.stage),
                                                    entryPoint.writeRef(), diagnostics.writeRef())) ||
        !entryPoint) {
        return compileError(diagnostics, fmt::format("entry point '{}' was not found or is not a valid {} shader",
                                                     request.entryPoint, nameOf(request.stage)));
    }

    const std::array<slang::IComponentType*, 2> components = {module, entryPoint.get()};
    Slang::ComPtr<slang::IComponentType> program;
    diagnostics = nullptr;
    if (SLANG_FAILED(session->createCompositeComponentType(components.data(), static_cast<SlangInt>(components.size()),
                                                           program.writeRef(), diagnostics.writeRef()))) {
        return compileError(diagnostics, "the shader program could not be composed");
    }

    Slang::ComPtr<slang::IComponentType> linked;
    diagnostics = nullptr;
    if (SLANG_FAILED(program->link(linked.writeRef(), diagnostics.writeRef()))) {
        return compileError(diagnostics, "the shader program could not be linked");
    }

    Slang::ComPtr<slang::IBlob> code;
    diagnostics = nullptr;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef())) || !code) {
        return compileError(diagnostics, "code generation failed");
    }
    logWarnings(diagnostics, request);

    ShaderBinary binary;
    binary.stage = request.stage;
    binary.entryPoint = request.entryPoint;
    binary.compilerVersion = m_impl->version;
    binary.code.resize(code->getBufferSize());
    std::memcpy(binary.code.data(), code->getBufferPointer(), binary.code.size());

    const SlangInt32 dependencyCount = module->getDependencyFileCount();
    for (SlangInt32 index = 0; index < dependencyCount; ++index) {
        const char* dependencyPath = module->getDependencyFilePath(index);
        if (dependencyPath == nullptr) {
            continue;
        }
        const std::filesystem::path path = pathFromUtf8(dependencyPath);
        // Slang also lists modules it synthesizes internally; only real files matter.
        if (!fileExists(path)) {
            continue;
        }
        CORDITE_TRY_ASSIGN(const std::vector<std::byte> content, readBinaryFile(path));
        binary.dependencies.push_back({m_impl->toRootRelative(path), hashBytes128(content)});
    }

    // Slang does not promise an order; sort so that identical inputs give identical output.
    std::ranges::sort(binary.dependencies, {}, &ShaderDependency::path);
    const auto duplicates = std::ranges::unique(binary.dependencies, {}, &ShaderDependency::path);
    binary.dependencies.erase(duplicates.begin(), duplicates.end());
    return binary;
}

} // namespace cordite
