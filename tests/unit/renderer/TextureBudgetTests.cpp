#include "cordite/renderer/TextureBudget.h"

#include "support/TestSupport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace cordite;

namespace {

/// A texture of `size` texels a side, of four bytes a texel.
TextureFootprint square(uint32_t size, uint32_t bytesPerTexel = 4) {
    return {.width = size, .height = size, .bytesPerTexel = bytesPerTexel};
}

} // namespace

TEST_SUITE("TextureBudget") {
    TEST_CASE("a texture is kept from the first level whose larger side is within the limit, and the bias on") {
        // 4096 texels a side has thirteen levels, the last a texel across.
        const TextureFootprint large = square(4096);
        CHECK(firstTextureLevel(large, 0, 0) == 0);
        CHECK(firstTextureLevel(large, 4096, 0) == 0);
        CHECK(firstTextureLevel(large, 2048, 0) == 1);
        CHECK(firstTextureLevel(large, 1024, 0) == 2);
        CHECK(firstTextureLevel(large, 1000, 0) == 3);
        // The larger side decides.
        CHECK(firstTextureLevel({.width = 4096, .height = 1024}, 1024, 0) == 2);
        CHECK(firstTextureLevel({.width = 512, .height = 2048}, 1024, 0) == 1);
        // The bias drops as many levels more, down to the last.
        CHECK(firstTextureLevel(large, 2048, 1) == 2);
        CHECK(firstTextureLevel(large, 0, 12) == 12);
        CHECK(firstTextureLevel(large, 2048, 40) == 12);
        CHECK(firstTextureLevel(large, 2048, std::numeric_limits<uint32_t>::max()) == 12);
        // A texel is all there is.
        CHECK(firstTextureLevel(square(1), 1, 3) == 0);
        CHECK(firstTextureLevel({.width = 0, .height = 0}, 1, 3) == 0);
    }

    TEST_CASE("a texture takes the bytes of its levels from the first it keeps") {
        // 4 x 4, 2 x 2 and 1 x 1 texels.
        CHECK(textureBytesFrom(square(4), 0) == (16 + 4 + 1) * 4);
        CHECK(textureBytesFrom(square(4), 1) == (4 + 1) * 4);
        CHECK(textureBytesFrom(square(4), 2) == 4);
        CHECK(textureBytesFrom(square(4), 3) == 0);
        // Light is four half floats a texel.
        CHECK(textureBytesFrom(square(4, 8), 0) == (16 + 4 + 1) * 8);
        // Not square: 8 x 2, 4 x 1, 2 x 1, 1 x 1.
        CHECK(textureBytesFrom({.width = 8, .height = 2, .bytesPerTexel = 1}, 0) == 16 + 4 + 2 + 1);
    }

    TEST_CASE("the bias is the smallest at which all the textures fit the budget, whatever their order") {
        const std::vector<TextureFootprint> textures = {square(1024), square(512, 8), {.width = 2048, .height = 256}};
        uint64_t asTheyAre = 0;
        uint64_t oneSmaller = 0;
        uint64_t twoSmaller = 0;
        for (const TextureFootprint& texture : textures) {
            asTheyAre += textureBytesFrom(texture, 0);
            oneSmaller += textureBytesFrom(texture, 1);
            twoSmaller += textureBytesFrom(texture, 2);
        }
        // No budget, or one they fit as they are: nothing is dropped.
        CHECK(textureBiasFor(textures, 0, 0) == 0);
        CHECK(textureBiasFor(textures, 0, asTheyAre) == 0);
        // A byte less, and every texture drops a level; at what one level smaller takes, so it stays.
        CHECK(textureBiasFor(textures, 0, asTheyAre - 1) == 1);
        CHECK(textureBiasFor(textures, 0, oneSmaller) == 1);
        CHECK(textureBiasFor(textures, 0, oneSmaller - 1) == 2);
        CHECK(textureBiasFor(textures, 0, twoSmaller) == 2);

        // The same for every order they may have come in.
        std::array<size_t, 3> order = {0, 1, 2};
        do {
            const std::vector<TextureFootprint> reordered = {textures[order[0]], textures[order[1]],
                                                             textures[order[2]]};
            CHECK(textureBiasFor(reordered, 0, oneSmaller - 1) == 2);
        } while (std::next_permutation(order.begin(), order.end()));
    }

    TEST_CASE("the limit comes before the budget, and a budget nothing fits leaves every texture at its last level") {
        // Under a limit of 1024 a texture of 4096 counts as one of 1024.
        const std::vector<TextureFootprint> one = {square(4096)};
        const uint64_t limited = textureBytesFrom(square(4096), 2);
        CHECK(textureBiasFor(one, 1024, limited) == 0);
        CHECK(textureBiasFor(one, 1024, limited - 1) == 1);
        CHECK(textureBiasFor(one, 0, limited) == 2);
        // Not even a texel each fits: as small as they get.
        const std::vector<TextureFootprint> two = {square(4096), square(16)};
        CHECK(textureBiasFor(two, 0, 1) == 12);
        CHECK(firstTextureLevel(two[0], 0, 12) == 12);
        CHECK(firstTextureLevel(two[1], 0, 12) == 4);
        // Nothing gets smaller than a texel: textures of one each stay as they are.
        const std::vector<TextureFootprint> texels = {square(1), square(1)};
        CHECK(textureBiasFor(texels, 0, 1) == 0);
        // No textures fit any budget.
        CHECK(textureBiasFor({}, 2048, 1) == 0);
    }
}
