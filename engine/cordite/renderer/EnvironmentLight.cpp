#include "cordite/renderer/EnvironmentLight.h"

#include "cordite/math/Color.h"
#include "cordite/math/Equirectangular.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace cordite {
namespace {

/// The light of an image is added up over a grid of this many cells, whatever the size
/// of the image: nine numbers cannot tell a finer picture apart. Each cell holds the
/// average of the part of the image it covers, so that no pixel is skipped - a sun may
/// be one pixel of millions, and most of the light.
constexpr uint32_t kColumns = 256;
constexpr uint32_t kRows = 128;

/// The light a pixel holds: linear, finite and not less than nothing.
Vec3 lightOf(const Vec4& pixel, bool isEncoded) {
    Vec3 light(pixel);
    if (!isFinite(light)) {
        return Vec3(0.0f);
    }
    light = glm::max(light, Vec3(0.0f));
    return isEncoded ? srgbToLinear(light) : light;
}

/// Calls `visit(cell, share)` for every cell of a row of `cellCount` cells that piece
/// `index` of `pieceCount` equal pieces of the same row lies in; `share` is the part of
/// the cell the piece covers. Over all the pieces, the shares of a cell add up to one.
template <typename Visit>
void forEachCellUnder(uint32_t index, uint32_t pieceCount, uint32_t cellCount, const Visit& visit) {
    const double cellsPerPiece = static_cast<double>(cellCount) / static_cast<double>(pieceCount);
    const double begin = static_cast<double>(index) * cellsPerPiece;
    const double end = static_cast<double>(index + 1) * cellsPerPiece;
    const auto first = static_cast<uint32_t>(begin);
    for (uint32_t cell = first; cell < cellCount && static_cast<double>(cell) < end; ++cell) {
        const double share =
            std::min(end, static_cast<double>(cell) + 1.0) - std::max(begin, static_cast<double>(cell));
        if (share > 0.0) {
            visit(cell, static_cast<float>(share));
        }
    }
}

/// The image as a grid of kColumns by kRows cells, each the average of the light of the
/// part of the image it covers; pixels are taken to hold one light all over.
std::vector<Vec3> cellsOf(const Image& image) {
    const bool isEncoded = image.format() != ImageFormat::Rgba32F;
    const std::span<const std::byte> bytes = image.pixels();
    std::vector<Vec3> cells(static_cast<size_t>(kColumns) * kRows, Vec3(0.0f));
    std::vector<Vec3> row(kColumns);

    for (uint32_t y = 0; y < image.height(); ++y) {
        // The row of pixels, brought to the width of the grid, ...
        std::ranges::fill(row, Vec3(0.0f));
        for (uint32_t x = 0; x < image.width(); ++x) {
            Vec3 light(0.0f);
            if (isEncoded) {
                light = lightOf(image.pixel(x, y), true);
            } else {
                // Straight from the floats: an image of a sky has millions of them.
                Vec4 pixel(0.0f);
                std::memcpy(&pixel, bytes.data() + static_cast<size_t>(y) * image.rowBytes() + x * sizeof(Vec4),
                            sizeof(Vec4));
                light = lightOf(pixel, false);
            }
            forEachCellUnder(x, image.width(), kColumns,
                             [&](uint32_t column, float share) { row[column] += light * share; });
        }
        // ... is part of one row of cells, or of several.
        forEachCellUnder(y, image.height(), kRows, [&](uint32_t cellRow, float share) {
            Vec3* target = cells.data() + static_cast<size_t>(cellRow) * kColumns;
            for (uint32_t column = 0; column < kColumns; ++column) {
                target[column] += row[column] * share;
            }
        });
    }
    return cells;
}

} // namespace

SphericalHarmonics9 projectEnvironment(const Image& image) {
    SphericalHarmonics9 light;
    if (image.isEmpty()) {
        return light;
    }
    const std::vector<Vec3> cells = cellsOf(image);

    for (uint32_t row = 0; row < kRows; ++row) {
        // The band of the sphere this row of cells covers, and what part of the whole
        // sphere a cell of it is: bands near the poles are small.
        const float top = static_cast<float>(row) / static_cast<float>(kRows) * kPi;
        const float bottom = static_cast<float>(row + 1) / static_cast<float>(kRows) * kPi;
        const float solidAngle = (std::cos(top) - std::cos(bottom)) * kTwoPi / static_cast<float>(kColumns);
        const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(kRows);

        // Row by row, so that many small numbers are not added to one large one.
        SphericalHarmonics9 band;
        for (uint32_t column = 0; column < kColumns; ++column) {
            const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(kColumns);
            band.add(equirectDirection(Vec2(u, v)), cells[static_cast<size_t>(row) * kColumns + column], solidAngle);
        }
        for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
            light.coefficients[index] += band.coefficients[index];
        }
    }
    return light;
}

} // namespace cordite
