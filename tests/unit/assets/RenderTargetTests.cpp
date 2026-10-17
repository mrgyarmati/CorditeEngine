#include "cordite/assets/RenderTarget.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "support/TestSupport.h"

#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// A render target file with the given members after its header.
Json targetFile(std::string_view members) {
    return json(std::string(R"({"format": "cordite.target", "version": 1)") + (members.empty() ? "" : ", ") +
                std::string(members) + "}");
}

} // namespace

TEST_SUITE("RenderTarget") {
    TEST_CASE("a render target file says how large its picture is") {
        const RenderTargetData target = unwrap(parseRenderTargetFile(targetFile(R"("width": 320, "height": 180)")));
        CHECK(target.width == 320);
        CHECK(target.height == 180);
        // What a file leaves out is as a new target has it.
        const RenderTargetData square = unwrap(parseRenderTargetFile(targetFile("")));
        CHECK(square == RenderTargetData{});
        CHECK(square.width == 256);
        CHECK(square.height == 256);
        // Written, it reads back the same, with every member.
        const Json written = serializeRenderTarget(target);
        CHECK(written == json(R"({"format": "cordite.target", "version": 1, "width": 320, "height": 180})"));
        CHECK(unwrap(parseRenderTargetFile(written)) == target);
        // The largest and the smallest there may be.
        CHECK(parseRenderTargetFile(targetFile(R"("width": 1, "height": 4096)")).has_value());
    }

    TEST_CASE("a render target file that is wrong is refused, saying what is wrong") {
        for (const auto& [members, fragment] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {R"("width": 0)", "width"},
                 {R"("height": 4097)", "height"},
                 {R"("width": 12.5)", "width"},
                 {R"("width": -3)", "width"},
                 {R"("height": "large")", "height"},
                 {R"("depth": 2)", "depth"},
             }) {
            CAPTURE(std::string(members));
            const Result<RenderTargetData> target = parseRenderTargetFile(targetFile(members));
            REQUIRE_FALSE(target.has_value());
            CHECK(test::errorOf(target).find(fragment) != std::string::npos);
        }
        CHECK_FALSE(parseRenderTargetFile(json(R"({"format": "cordite.material", "version": 1})")).has_value());
        CHECK_FALSE(parseRenderTargetFile(json(R"({"format": "cordite.target", "version": 2})")).has_value());
        CHECK_FALSE(parseRenderTargetFile(json("[1, 2]")).has_value());
    }

    TEST_CASE("the assets of a project know render targets by their files, and load them by id") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "monitor.target.json",
                              R"({"format": "cordite.target", "version": 1, "width": 64, "height": 32})")
                    .has_value());
        REQUIRE(writeTextFile(directory.path() / "broken.target.json",
                              R"({"format": "cordite.target", "version": 1, "width": 0})")
                    .has_value());
        const auto assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        const AssetInfo* monitor = assets->database().findByReference("monitor.target.json");
        REQUIRE(monitor != nullptr);
        CHECK(monitor->type == AssetType::RenderTarget);
        CHECK(nameOf(AssetType::RenderTarget) == "renderTarget");
        CHECK(displayNameOf(AssetType::RenderTarget) == "RenderTarget");
        CHECK(assetTypeFromName("rendertarget") == AssetType::RenderTarget);
        CHECK(unwrap(assets->loadRenderTarget(monitor->id)) == RenderTargetData{.width = 64, .height = 32});

        // A file that is wrong is listed, and is a problem of the project.
        const AssetInfo* broken = assets->database().findByReference("broken.target.json");
        REQUIRE(broken != nullptr);
        bool reported = false;
        for (const std::string& problem : assets->database().problems()) {
            reported = reported || problem.find("broken.target.json") != std::string::npos;
        }
        CHECK(reported);
        const Result<RenderTargetData> loaded = assets->loadRenderTarget(broken->id);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(test::errorOf(loaded).find("broken.target.json") != std::string::npos);
        // Another kind of asset is not a render target.
        CHECK_FALSE(assets->loadRenderTarget(builtinMeshId(BuiltinMesh::Cube)).has_value());
    }
}
