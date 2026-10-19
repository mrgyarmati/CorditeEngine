#include "cordite/shaders/ShaderTypes.h"

#include "support/TestSupport.h"
#include "unit/shaders/ShaderTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;

namespace {

ShaderRequest makeRequest() {
    ShaderRequest request;
    request.path = "passes/GBuffer.slang";
    request.entryPoint = "pixelMain";
    request.stage = ShaderStage::Pixel;
    request.defines = {{"NORMAL_MAP", "1"}, {"ALPHA_TEST", "1"}};
    return request;
}

} // namespace

TEST_SUITE("ShaderTypes") {
    TEST_CASE("normalize sorts the defines by name") {
        ShaderRequest request = makeRequest();
        REQUIRE(request.normalize().has_value());
        REQUIRE(request.defines.size() == 2);
        CHECK(request.defines[0].name == "ALPHA_TEST");
        CHECK(request.defines[1].name == "NORMAL_MAP");
    }

    TEST_CASE("the order in which defines are listed does not change the permutation") {
        ShaderRequest first = makeRequest();
        ShaderRequest second = makeRequest();
        std::swap(second.defines[0], second.defines[1]);

        REQUIRE(first.normalize().has_value());
        REQUIRE(second.normalize().has_value());
        CHECK(first == second);
        CHECK(first.key() == second.key());
    }

    TEST_CASE("every part of a request contributes to its key") {
        ShaderRequest base = makeRequest();
        REQUIRE(base.normalize().has_value());
        const Hash128 baseKey = base.key();
        CHECK(baseKey == base.key());

        ShaderRequest otherPath = base;
        otherPath.path = "passes/Other.slang";
        CHECK(otherPath.key() != baseKey);

        ShaderRequest otherEntry = base;
        otherEntry.entryPoint = "vertexMain";
        CHECK(otherEntry.key() != baseKey);

        ShaderRequest otherStage = base;
        otherStage.stage = ShaderStage::Vertex;
        CHECK(otherStage.key() != baseKey);

        ShaderRequest otherValue = base;
        otherValue.defines[0].value = "0";
        CHECK(otherValue.key() != baseKey);

        ShaderRequest fewerDefines = base;
        fewerDefines.defines.pop_back();
        CHECK(fewerDefines.key() != baseKey);

        // A define's name and value must not be confusable with each other.
        ShaderRequest shifted = base;
        shifted.defines = {{"ALPHA_TEST1", ""}, {"NORMAL_MAP", "1"}};
        CHECK(shifted.key() != baseKey);
    }

    TEST_CASE("the key of a request never changes between engine versions") {
        // Keys name the files of the shader cache and of exported games. If this value
        // changes, every cached shader is orphaned; bump kRequestKeyVersion deliberately
        // instead of changing the hashing by accident.
        ShaderRequest request = makeRequest();
        REQUIRE(request.normalize().has_value());
        CHECK(request.key().toHex() == "1deb80bc9494887815db350df17db61c");
    }

    TEST_CASE("normalize rejects unsafe or malformed paths") {
        for (const char* path :
             {"", "/abs/Shader.slang", "C:/Shader.slang", "..", "../Shader.slang", "passes/../../Shader.slang",
              "passes\\Shader.slang", "passes//Shader.slang", "passes/./Shader.slang", "passes/"}) {
            ShaderRequest request = makeRequest();
            request.path = path;
            const Status status = request.normalize();
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
        }

        ShaderRequest nested = makeRequest();
        nested.path = "a/b/c/Shader.v2.slang";
        CHECK(nested.normalize().has_value());
    }

    TEST_CASE("normalize rejects invalid entry points and defines") {
        ShaderRequest badEntry = makeRequest();
        badEntry.entryPoint = "main; drop";
        CHECK_FALSE(badEntry.normalize().has_value());

        ShaderRequest emptyEntry = makeRequest();
        emptyEntry.entryPoint = "";
        CHECK_FALSE(emptyEntry.normalize().has_value());

        ShaderRequest digitFirst = makeRequest();
        digitFirst.defines = {{"1ST", "1"}};
        CHECK_FALSE(digitFirst.normalize().has_value());

        ShaderRequest spaced = makeRequest();
        spaced.defines = {{"HAS SPACE", "1"}};
        CHECK_FALSE(spaced.normalize().has_value());

        ShaderRequest newline = makeRequest();
        newline.defines = {{"VALUE", "1\n#include \"secret\""}};
        CHECK_FALSE(newline.normalize().has_value());

        ShaderRequest duplicate = makeRequest();
        duplicate.defines = {{"A", "1"}, {"B", "1"}, {"A", "2"}};
        const Status status = duplicate.normalize();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("'A'") != std::string::npos);
    }

    TEST_CASE("describe is a readable summary") {
        ShaderRequest request = makeRequest();
        REQUIRE(request.normalize().has_value());
        CHECK(request.describe() == "passes/GBuffer.slang:pixelMain [ALPHA_TEST=1 NORMAL_MAP=1]");

        request.defines.clear();
        CHECK(request.describe() == "passes/GBuffer.slang:pixelMain");
    }

    TEST_CASE("isSpirv checks the magic number and the word alignment") {
        CHECK(isSpirv(test::makeFakeSpirv()));
        CHECK(isSpirv(test::makeFakeSpirv({})));

        CHECK_FALSE(isSpirv({}));

        std::vector<std::byte> truncated = test::makeFakeSpirv();
        truncated.pop_back();
        CHECK_FALSE(isSpirv(truncated));

        std::vector<std::byte> wrongMagic = test::makeFakeSpirv();
        wrongMagic[0] = std::byte{0};
        CHECK_FALSE(isSpirv(wrongMagic));

        CHECK_FALSE(isSpirv(std::vector<std::byte>(3)));
    }

    TEST_CASE("stage names") {
        CHECK(nameOf(ShaderStage::Vertex) == "vertex");
        CHECK(nameOf(ShaderStage::Pixel) == "pixel");
        CHECK(nameOf(ShaderStage::Compute) == "compute");
    }

    TEST_CASE("target names") {
        CHECK(nameOf(ShaderTarget::Spirv) == "spirv");
        CHECK(nameOf(ShaderTarget::Dxil) == "dxil");
    }

    TEST_CASE("isDxil asks for a signed container of the size it says, with a part of DXIL") {
        const std::vector<std::byte> dxil = test::makeFakeDxil();
        CHECK(isDxil(dxil));
        CHECK(isDxil(test::makeFakeDxil({})));
        CHECK(isCodeOf(ShaderTarget::Dxil, dxil));
        CHECK_FALSE(isCodeOf(ShaderTarget::Spirv, dxil));
        CHECK(targetOf(dxil) == ShaderTarget::Dxil);
        CHECK(targetOf(test::makeFakeSpirv()) == ShaderTarget::Spirv);
        CHECK_FALSE(targetOf(std::vector<std::byte>(64)).has_value());
        CHECK_FALSE(isDxil(test::makeFakeSpirv()));
        CHECK_FALSE(isDxil({}));

        // Each word of the header broken in turn: the magic, the hash - unsigned, all of it
        // nothing - the size, the count and the offset of the parts, the letters of the part.
        const auto broken = [&dxil](size_t offset, std::span<const std::byte> bytes) {
            std::vector<std::byte> code = dxil;
            std::ranges::copy(bytes, code.begin() + static_cast<std::ptrdiff_t>(offset));
            return code;
        };
        const auto word = [](uint32_t value) {
            std::array<std::byte, 4> bytes{};
            std::memcpy(bytes.data(), &value, sizeof(value));
            return bytes;
        };
        CHECK_FALSE(isDxil(broken(0, word(0x43425845u))));
        CHECK_FALSE(isDxil(broken(4, std::vector<std::byte>(16))));
        CHECK(isDxil(broken(4, std::vector<std::byte>(15))));
        CHECK_FALSE(isDxil(broken(24, word(static_cast<uint32_t>(dxil.size()) + 1))));
        CHECK_FALSE(isDxil(broken(24, word(static_cast<uint32_t>(dxil.size()) - 4))));
        CHECK_FALSE(isDxil(broken(28, word(65))));
        CHECK_FALSE(isDxil(broken(28, word(0))));
        CHECK_FALSE(isDxil(broken(32, word(static_cast<uint32_t>(dxil.size()) - 4))));
        CHECK_FALSE(isDxil(broken(36, word(0x48534844u))));
        // Cut short anywhere, it is no container of its size.
        for (size_t length = 0; length < dxil.size(); ++length) {
            CAPTURE(length);
            CHECK_FALSE(isDxil(std::span(dxil).first(length)));
        }
    }
}
