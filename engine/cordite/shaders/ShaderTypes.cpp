#include "cordite/shaders/ShaderTypes.h"

#include <algorithm>
#include <cstring>

namespace cordite {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;

/// Bumped when the meaning of a request changes, to orphan every existing cache entry.
constexpr uint32_t kRequestKeyVersion = 1;

bool isIdentifier(std::string_view text) {
    if (text.empty() || (text.front() >= '0' && text.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(text, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

/// A shader path is relative, uses forward slashes and never climbs out of its root.
bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }

    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = std::min(path.find('/', begin), path.size());
        const std::string_view segment = path.substr(begin, end - begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        begin = end + 1;
    }
    return true;
}

} // namespace

std::string_view nameOf(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return "vertex";
        case ShaderStage::Pixel:
            return "pixel";
        case ShaderStage::Compute:
            return "compute";
    }
    return "unknown";
}

Status ShaderRequest::normalize() {
    if (!isSafeRelativePath(path)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is not a valid shader path: use a relative path with forward slashes", path);
    }
    if (!isIdentifier(entryPoint)) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is not a valid entry point name", entryPoint);
    }

    for (const ShaderDefine& define : defines) {
        if (!isIdentifier(define.name)) {
            return makeError(ErrorCode::InvalidArgument, "'{}' is not a valid shader define name", define.name);
        }
        // A define value ends up on a compiler command line and in the preprocessor.
        if (define.value.find_first_of("\r\n") != std::string::npos) {
            return makeError(ErrorCode::InvalidArgument, "the value of shader define '{}' contains a line break",
                             define.name);
        }
    }

    std::ranges::sort(defines, {}, &ShaderDefine::name);
    const auto duplicate = std::ranges::adjacent_find(defines, {}, &ShaderDefine::name);
    if (duplicate != defines.end()) {
        return makeError(ErrorCode::InvalidArgument, "shader define '{}' is given more than once", duplicate->name);
    }
    return {};
}

Hash128 ShaderRequest::key() const {
    ContentHasher hasher;
    hasher.updateValue(kRequestKeyVersion);
    hasher.update(std::string_view(path));
    hasher.update(std::string_view(entryPoint));
    hasher.updateValue(static_cast<uint8_t>(stage));
    hasher.updateValue(static_cast<uint64_t>(defines.size()));
    for (const ShaderDefine& define : defines) {
        hasher.update(std::string_view(define.name));
        hasher.update(std::string_view(define.value));
    }
    return hasher.finish();
}

std::string ShaderRequest::describe() const {
    std::string text = fmt::format("{}:{}", path, entryPoint);
    if (!defines.empty()) {
        text += " [";
        for (size_t index = 0; index < defines.size(); ++index) {
            text += fmt::format("{}{}={}", index > 0 ? " " : "", defines[index].name, defines[index].value);
        }
        text += "]";
    }
    return text;
}

bool isSpirv(std::span<const std::byte> code) {
    if (code.size() < sizeof(uint32_t) || code.size() % sizeof(uint32_t) != 0) {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, code.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

} // namespace cordite
