#include "cordite/shaders/ShaderLibrary.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Time.h"

#include <algorithm>

namespace cordite {

ShaderLibrary::ShaderLibrary(ShaderLibraryDesc desc) : m_desc(std::move(desc)) {
    if (!m_desc.cacheDirectory.empty()) {
        m_cache.emplace(m_desc.cacheDirectory);
    }
}

std::filesystem::path ShaderLibrary::resolveSource(std::string_view relativePath) const {
    const std::filesystem::path relative = pathFromUtf8(relativePath);
    for (const std::filesystem::path& root : m_desc.sourceRoots) {
        std::filesystem::path candidate = root / relative;
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool ShaderLibrary::isUpToDate(const ShaderBinary& binary) const {
    if (m_desc.trustCache) {
        return true;
    }
    // A compiler upgrade can change the generated code for unchanged sources.
    if (m_desc.compiler != nullptr && binary.compilerVersion != m_desc.compiler->version()) {
        return false;
    }

    return std::ranges::all_of(binary.dependencies, [this](const ShaderDependency& dependency) {
        const std::filesystem::path source = resolveSource(dependency.path);
        if (source.empty()) {
            return false;
        }
        const Result<std::vector<std::byte>> content = readBinaryFile(source);
        return content.has_value() && hashBytes128(*content) == dependency.contentHash;
    });
}

Result<std::shared_ptr<const ShaderBinary>> ShaderLibrary::load(ShaderRequest request) {
    CORDITE_TRY(request.normalize());
    const Hash128 key = request.key();
    const std::string keyText = key.toHex();

    // One lock for the whole operation: compiling the same permutation twice in
    // parallel would be wasted work, and the compiler is not re-entrant.
    const std::lock_guard lock(m_mutex);

    if (const auto found = m_entries.find(keyText); found != m_entries.end()) {
        ++m_stats.memoryHits;
        return found->second.binary;
    }

    if (m_cache) {
        if (std::optional<ShaderBinary> cached = m_cache->load(key)) {
            if (isUpToDate(*cached)) {
                ++m_stats.diskHits;
                auto binary = std::make_shared<const ShaderBinary>(std::move(*cached));
                m_entries.emplace(keyText, Entry{std::move(request), binary});
                return binary;
            }
        }
    }

    if (m_desc.compiler == nullptr) {
        ++m_stats.failures;
        return makeError(ErrorCode::NotFound,
                         "shader {} is not in the shader cache and no compiler is available to build it",
                         request.describe());
    }

    const Stopwatch stopwatch;
    Result<ShaderBinary> compiled = m_desc.compiler->compile(request);
    if (!compiled) {
        ++m_stats.failures;
        return tl::unexpected(compiled.error().withContext(fmt::format("compiling {}", request.describe())));
    }
    if (!isSpirv(compiled->code)) {
        ++m_stats.failures;
        return makeError(ErrorCode::InvalidState, "the compiler produced no SPIR-V for {}", request.describe());
    }
    ++m_stats.compilations;
    CORDITE_LOG_DEBUG("Shaders", "compiled {} in {:.0f} ms", request.describe(), stopwatch.elapsedMilliseconds());

    if (m_cache) {
        // A cache that cannot be written only costs time on the next run.
        if (const Status stored = m_cache->store(key, *compiled); !stored) {
            CORDITE_LOG_WARN("Shaders", "cannot write the shader cache: {}", stored.error().message);
        }
    }

    auto binary = std::make_shared<const ShaderBinary>(std::move(*compiled));
    m_entries.emplace(keyText, Entry{std::move(request), binary});
    return binary;
}

std::vector<ShaderRequest> ShaderLibrary::invalidateChanged() {
    const std::lock_guard lock(m_mutex);

    std::vector<ShaderRequest> invalidated;
    std::erase_if(m_entries, [&](const auto& item) {
        if (isUpToDate(*item.second.binary)) {
            return false;
        }
        invalidated.push_back(item.second.request);
        return true;
    });
    return invalidated;
}

void ShaderLibrary::clearMemory() {
    const std::lock_guard lock(m_mutex);
    m_entries.clear();
}

ShaderLibraryStats ShaderLibrary::stats() const {
    const std::lock_guard lock(m_mutex);
    return m_stats;
}

} // namespace cordite
