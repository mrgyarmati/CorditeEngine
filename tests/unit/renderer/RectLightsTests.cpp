#include "cordite/renderer/RectLights.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <vector>

using namespace cordite;

namespace {

constexpr double kPiDouble = std::numbers::pi;

/// A texel of fit/results/ltc.js as the engine has it: the numbers of the file, as floats.
Vec4 texel(double x, double y, double z, double w) {
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)};
}

/// The view factor from a point to a rectangle in a parallel plane, `c` metres above it,
/// with one of its corners straight above the point and sides `a` and `b`: the closed form
/// of the books on radiative heat transfer.
double cornerFormFactor(double a, double b, double c) {
    const double x = a / c;
    const double y = b / c;
    const double rootX = std::sqrt(1.0 + x * x);
    const double rootY = std::sqrt(1.0 + y * y);
    return (x / rootX * std::atan(y / rootX) + y / rootY * std::atan(x / rootY)) / (2.0 * kPiDouble);
}

/// A rectangle `width` by `height` metres around `center`, turned to face the origin, its
/// right along the world's X where it can be.
RectLightShape facingOrigin(const Vec3& center, float width, float height) {
    const Vec3 towards = glm::normalize(center);
    const Vec3 right = glm::normalize(Vec3(1.0f, 0.0f, 0.0f) - towards * towards.x);
    return {.center = center, .right = right * (0.5f * width), .up = glm::cross(towards, right) * (0.5f * height)};
}

/// GGX as shaders/common/Brdf.slang has it, in doubles.
double distributionGgx(double nDotH, double alpha) {
    const double alphaSquared = alpha * alpha;
    const double denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / (kPiDouble * denominator * denominator);
}

double visibilitySmithGgx(double nDotL, double nDotV, double alpha) {
    const double alphaSquared = alpha * alpha;
    const double lambdaV = nDotL * std::sqrt(nDotV * nDotV * (1.0 - alphaSquared) + alphaSquared);
    const double lambdaL = nDotV * std::sqrt(nDotL * nDotL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / std::max(lambdaV + lambdaL, 1.0e-5);
}

/// What a rectangle gives a surface, added up over `steps` by `steps` patches of it: what
/// the closed form and the fit are held to.
struct AddedUp {
    double formFactor = 0.0;
    double lobe = 0.0;
    double grazing = 0.0;
};

AddedUp addedUp(const RectLightShape& light, const Vec3& position, const Vec3& normal, const Vec3& toViewer,
                float roughness, int steps) {
    const glm::dvec3 center(light.center);
    const glm::dvec3 right(light.right);
    const glm::dvec3 up(light.up);
    const glm::dvec3 point(position);
    const glm::dvec3 n(normal);
    const glm::dvec3 v(toViewer);
    const glm::dvec3 facing = glm::normalize(glm::cross(up, right));
    const double alpha = static_cast<double>(roughness) * static_cast<double>(roughness);
    const double nDotV = std::max(glm::dot(n, v), 1.0e-4);
    const double patch = 4.0 * glm::length(right) * glm::length(up) / (static_cast<double>(steps) * steps);
    AddedUp sum;
    for (int row = 0; row < steps; ++row) {
        for (int column = 0; column < steps; ++column) {
            const double across = (column + 0.5) / steps * 2.0 - 1.0;
            const double along = (row + 0.5) / steps * 2.0 - 1.0;
            const glm::dvec3 offset = center + right * across + up * along - point;
            const double distanceSquared = glm::dot(offset, offset);
            const glm::dvec3 toLight = offset / std::sqrt(distanceSquared);
            const double nDotL = glm::dot(n, toLight);
            const double facingCosine = -glm::dot(toLight, facing);
            if (nDotL <= 0.0 || facingCosine <= 0.0) {
                continue;
            }
            const double solidAngle = patch * facingCosine / distanceSquared;
            sum.formFactor += nDotL / kPiDouble * solidAngle;
            const glm::dvec3 halfway = glm::normalize(toLight + v);
            const double lobe = distributionGgx(std::clamp(glm::dot(n, halfway), 0.0, 1.0), alpha) *
                                visibilitySmithGgx(nDotL, nDotV, alpha) * nDotL * solidAngle;
            sum.lobe += lobe;
            sum.grazing += lobe * std::pow(1.0 - std::clamp(glm::dot(v, halfway), 0.0, 1.0), 5.0);
        }
    }
    return sum;
}

/// What the same GGX reflects of light that arrives from all of the sky above a surface
/// seen at `nDotV`, added up over a grid of directions: what a lobe reflects in all.
AddedUp allAround(double nDotV, float roughness) {
    const double alpha = static_cast<double>(roughness) * static_cast<double>(roughness);
    const glm::dvec3 toViewer(std::sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
    constexpr int kRings = 1000;
    constexpr int kSectors = 1000;
    const double ringAngle = 0.5 * kPiDouble / kRings;
    const double sectorAngle = 2.0 * kPiDouble / kSectors;
    AddedUp sum;
    for (int ring = 0; ring < kRings; ++ring) {
        const double elevation = (ring + 0.5) * ringAngle;
        const double nDotL = std::cos(elevation);
        const double solidAngle = std::sin(elevation) * ringAngle * sectorAngle;
        for (int sector = 0; sector < kSectors; ++sector) {
            const double around = (sector + 0.5) * sectorAngle;
            const glm::dvec3 toLight(std::sin(elevation) * std::cos(around), std::sin(elevation) * std::sin(around),
                                     nDotL);
            const glm::dvec3 halfway = glm::normalize(toLight + toViewer);
            const double lobe = distributionGgx(halfway.z, alpha) *
                                visibilitySmithGgx(nDotL, std::max(nDotV, 1.0e-4), alpha) * nDotL * solidAngle;
            sum.lobe += lobe;
            sum.grazing += lobe * std::pow(1.0 - std::clamp(glm::dot(toViewer, halfway), 0.0, 1.0), 5.0);
        }
    }
    return sum;
}

bool isFinite(const Vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

bool isFinite(const RectLightIrradiance& irradiance) {
    return std::isfinite(irradiance.formFactor) && std::isfinite(irradiance.direction.x) &&
           std::isfinite(irradiance.direction.y) && std::isfinite(irradiance.direction.z);
}

const Vec3 kUp(0.0f, 1.0f, 0.0f);

} // namespace

TEST_SUITE("RectLights") {
    TEST_CASE("the tables of the fit are those of ltc_code, texel for texel") {
        const std::span<const Vec4> matrices = ltcMatrixTexels();
        const std::span<const Vec4> magnitudes = ltcMagnitudeTexels();
        REQUIRE(matrices.size() == size_t{kLtcTableSize} * kLtcTableSize);
        REQUIRE(magnitudes.size() == size_t{kLtcTableSize} * kLtcTableSize);

        // The texels of the roughness a and the angle t are at a + 64 t; these are lines of
        // fit/results/ltc.js. The first is a mirror seen straight on.
        CHECK(matrices[0] == texel(1, 0, 0, 2e-05));
        CHECK(magnitudes[0] == texel(1, 0, 0, 0));
        CHECK(matrices[2080] == texel(0.626092, -0.219644, 0.531595, 0.312202));
        CHECK(magnitudes[2080] == texel(0.88238, 0.00296637, 0, 0.19188));
        CHECK(matrices[3210] == texel(0.137431, -0.0172713, 0.344491, 0.00701868));
        CHECK(magnitudes[3210] == texel(0.995731, 0.0988425, 0, 0.0232484));
        CHECK(matrices[4095] == texel(0.996389, -0.0808124, 0.0489007, 1.6577));
        CHECK(magnitudes[4095] == texel(0.932164, 0.0471899, 0, 1));

        // Every texel is made of numbers; a lobe reflects some light and never more than all,
        // and Schlick's factor has a part of that.
        for (size_t index = 0; index < matrices.size(); ++index) {
            const Vec4& matrix = matrices[index];
            const Vec4& magnitude = magnitudes[index];
            REQUIRE_MESSAGE(isFinite(matrix), "texel " << index);
            REQUIRE_MESSAGE(isFinite(magnitude), "texel " << index);
            REQUIRE_MESSAGE(magnitude.x > 0.0f, "texel " << index);
            REQUIRE_MESSAGE(magnitude.x <= 1.0f, "texel " << index);
            REQUIRE_MESSAGE(magnitude.y >= 0.0f, "texel " << index);
            REQUIRE_MESSAGE(magnitude.y <= magnitude.x, "texel " << index);
        }
    }

    TEST_CASE("the fit is read between its texels as a GPU reads a texture") {
        const std::span<const Vec4> matrices = ltcMatrixTexels();
        const std::span<const Vec4> magnitudes = ltcMagnitudeTexels();
        const auto roughnessOf = [](uint32_t column) { return static_cast<float>(column) / 63.0f; };
        const auto cosineOf = [](uint32_t row) {
            const float root = static_cast<float>(row) / 63.0f;
            return 1.0f - root * root;
        };

        // At the middles of texels, the texels.
        for (const auto [column, row] : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                 {0, 0}, {32, 32}, {10, 50}, {63, 63}, {63, 0}, {0, 63}}) {
            const LtcFit fit = ltcFit(roughnessOf(column), cosineOf(row));
            const size_t index = row * kLtcTableSize + column;
            CHECK_MESSAGE(test::nearlyEqual(fit.matrix, matrices[index], 1.0e-5f), test::describe(fit.matrix));
            CHECK_MESSAGE(test::nearlyEqual(fit.magnitude, magnitudes[index], 1.0e-5f), test::describe(fit.magnitude));
        }

        // Half way between two texels across, and between two down, their means.
        const LtcFit across = ltcFit(0.5f * (roughnessOf(20) + roughnessOf(21)), cosineOf(40));
        CHECK(test::nearlyEqual(across.matrix, 0.5f * (matrices[40 * 64 + 20] + matrices[40 * 64 + 21]), 1.0e-5f));
        const float halfwayRoot = 40.5f / 63.0f;
        const LtcFit down = ltcFit(roughnessOf(20), 1.0f - halfwayRoot * halfwayRoot);
        CHECK(test::nearlyEqual(down.magnitude, 0.5f * (magnitudes[40 * 64 + 20] + magnitudes[41 * 64 + 20]), 1.0e-5f));

        // Beyond the edges, the edges - and what is no number counts as nothing.
        CHECK(ltcFit(-1.0f, 0.5f).matrix == ltcFit(0.0f, 0.5f).matrix);
        CHECK(ltcFit(2.0f, 0.5f).matrix == ltcFit(1.0f, 0.5f).matrix);
        CHECK(ltcFit(0.5f, 1.5f).magnitude == ltcFit(0.5f, 1.0f).magnitude);
        CHECK(ltcFit(0.5f, -0.5f).magnitude == ltcFit(0.5f, 0.0f).magnitude);
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        CHECK(ltcFit(notANumber, notANumber).matrix == ltcFit(0.0f, 0.0f).matrix);
    }

    TEST_CASE("a matte surface gets of a rectangle what the closed form of the view factor says") {
        const Vec3 origin(0.0f);

        // A panel one metre by half of one, two metres above, that shines down: four corner
        // pieces, each half a metre by a quarter.
        const RectLightShape above{
            .center = Vec3(0.0f, 2.0f, 0.0f), .right = Vec3(0.5f, 0, 0), .up = Vec3(0, 0, -0.25f)};
        const RectLightIrradiance centered = rectLightIrradiance(above, origin, kUp);
        CHECK(std::abs(centered.formFactor - 4.0 * cornerFormFactor(0.5, 0.25, 2.0)) < 1.0e-6);
        CHECK(test::nearlyEqual(centered.direction, kUp, 1.0e-5f));

        // The same panel moved so that a corner is straight above.
        RectLightShape cornered = above;
        cornered.center = Vec3(0.5f, 2.0f, -0.25f);
        const RectLightIrradiance corner = rectLightIrradiance(cornered, origin, kUp);
        CHECK(std::abs(corner.formFactor - cornerFormFactor(1.0, 0.5, 2.0)) < 1.0e-6);
        // It comes from the side the panel is on.
        CHECK(corner.direction.x > 0.0f);
        CHECK(corner.direction.z < 0.0f);

        // Close and large it fills much of what the surface sees; far and small, it is a
        // point: the cosines of both ends times its area over pi times the square of the way.
        const RectLightShape ceiling{.center = Vec3(0.0f, 0.5f, 0.0f), .right = Vec3(20, 0, 0), .up = Vec3(0, 0, -20)};
        CHECK(std::abs(rectLightIrradiance(ceiling, origin, kUp).formFactor - 4.0 * cornerFormFactor(20, 20, 0.5)) <
              1.0e-5);
        const RectLightShape far = facingOrigin(Vec3(0.0f, 6.0f, -8.0f), 0.02f, 0.01f);
        const double cosine = 0.6;
        const double pointLike = 0.02 * 0.01 * cosine / (kPiDouble * 100.0);
        CHECK(std::abs(rectLightIrradiance(far, origin, kUp).formFactor / pointLike - 1.0) < 1.0e-3);

        // Turned, tilted and in part below the surface's horizon: as it adds up.
        const Vec3 tilted = glm::normalize(Vec3(0.3f, 1.0f, 0.2f));
        const std::vector<RectLightShape> lights = {
            facingOrigin(Vec3(1.0f, 2.0f, -1.5f), 1.0f, 0.5f),
            facingOrigin(Vec3(0.0f, 0.3f, -2.0f), 2.0f, 1.5f),
            {.center = Vec3(0.0f, 0.2f, -2.0f), .right = Vec3(-1.0f, 0, 0), .up = Vec3(0, 0.6f, 0)},
        };
        for (const RectLightShape& light : lights) {
            for (const Vec3& normal : {kUp, tilted}) {
                const AddedUp reference = addedUp(light, origin, normal, normal, 1.0f, 600);
                const float formFactor = rectLightIrradiance(light, origin, normal).formFactor;
                CHECK_MESSAGE(std::abs(formFactor / reference.formFactor - 1.0) < 1.0e-3,
                              formFactor << " against " << reference.formFactor);
            }
        }

        // Nothing for a surface behind the light, in its plane, or facing away from it.
        CHECK(rectLightIrradiance(above, Vec3(0.0f, 3.0f, 0.0f), kUp).formFactor == 0.0f);
        CHECK(rectLightIrradiance(above, Vec3(5.0f, 2.0f, 0.0f), kUp).formFactor == 0.0f);
        const RectLightIrradiance away = rectLightIrradiance(above, origin, -kUp);
        CHECK(away.formFactor == 0.0f);
        CHECK(away.direction == Vec3(0.0f));
    }

    TEST_CASE("the lobe of GGX reflects of a rectangle in its way what the fit says, within an eighth of it") {
        const Vec3 origin(0.0f);
        double sumOfErrors = 0.0;
        double largestError = 0.0;
        double largestGrazingError = 0.0;
        int cases = 0;
        for (const float degrees : {0.0f, 30.0f, 60.0f}) {
            const float angle = glm::radians(degrees);
            const Vec3 toViewer(0.0f, std::cos(angle), std::sin(angle));
            const Vec3 mirror(0.0f, std::cos(angle), -std::sin(angle));
            for (const float roughness : {0.25f, 0.5f, 0.75f, 1.0f}) {
                // Lights where the surface reflects them: small and large in the way of the
                // mirror's reflection, and one on the flank of the lobe - aside and nearer the
                // normal, the further the wider the lobe is.
                const Vec3 flank = glm::normalize(mirror + roughness * Vec3(0.25f, 0.25f, 0.0f));
                const std::vector<RectLightShape> lights = {
                    facingOrigin(mirror * 2.0f, 1.0f, 0.5f),
                    facingOrigin(mirror * 2.0f, 3.0f, 1.5f),
                    facingOrigin(flank * 2.5f, 1.0f, 1.0f),
                };
                for (const RectLightShape& light : lights) {
                    const AddedUp reference = addedUp(light, origin, kUp, toViewer, roughness, 400);
                    const RectLightSpecular fitted = rectLightSpecular(light, origin, kUp, toViewer, roughness);
                    const double error = std::abs(fitted.lobe - reference.lobe) / reference.lobe;
                    const double grazingError = std::abs(fitted.grazing - reference.grazing) / reference.lobe;
                    CHECK_MESSAGE(error < 0.12, degrees << " degrees, roughness " << roughness << ": " << fitted.lobe
                                                        << " against " << reference.lobe);
                    CHECK_MESSAGE(grazingError < 0.05, degrees << " degrees, roughness " << roughness << ": "
                                                               << fitted.grazing << " against " << reference.grazing);
                    sumOfErrors += error;
                    largestError = std::max(largestError, error);
                    largestGrazingError = std::max(largestGrazingError, grazingError);
                    ++cases;
                }
            }
        }
        // And within a twentieth on the whole: a matrix read the wrong way round, the tables
        // read along the wrong axes or the wrong channel of them are off by more than that.
        CHECK_MESSAGE(sumOfErrors / cases < 0.05, "on average " << sumOfErrors / cases << ", at worst " << largestError
                                                                << "; grazing at worst " << largestGrazingError);
    }

    TEST_CASE("a rectangle that fills the sky above a surface is reflected as all of the lobe of GGX") {
        // Two kilometres a side, a metre up: all of the sky but its last tenth of a degree.
        const RectLightShape sky{
            .center = Vec3(0.0f, 1.0f, 0.0f), .right = Vec3(1000.0f, 0, 0), .up = Vec3(0, 0, -1000.0f)};
        CHECK(std::abs(rectLightIrradiance(sky, Vec3(0.0f), kUp).formFactor - 1.0f) < 1.0e-3f);
        // Seen at grazing angles too, where Schlick's factor gives the reflectance much of its
        // part. There the cosine that stands in for the lobe reaches below the horizon, where
        // no light is, and the fit gives less: a tenth at 75 degrees, a seventh at 85.
        for (const auto [degrees, tolerance] : std::initializer_list<std::pair<float, double>>{
                 {0.0f, 0.05}, {45.0f, 0.05}, {75.0f, 0.12}, {85.0f, 0.18}}) {
            const float angle = glm::radians(degrees);
            const Vec3 toViewer(0.0f, std::cos(angle), std::sin(angle));
            for (const float roughness : {0.25f, 0.5f, 1.0f}) {
                const AddedUp reference = allAround(std::cos(angle), roughness);
                const RectLightSpecular fitted = rectLightSpecular(sky, Vec3(0.0f), kUp, toViewer, roughness);
                CHECK_MESSAGE(std::abs(fitted.lobe / reference.lobe - 1.0) < tolerance,
                              degrees << " degrees, roughness " << roughness << ": " << fitted.lobe << " against "
                                      << reference.lobe);
                CHECK_MESSAGE(std::abs(fitted.grazing - reference.grazing) < 0.15 * reference.grazing + 1.0e-3,
                              degrees << " degrees, roughness " << roughness << ": grazing " << fitted.grazing
                                      << " against " << reference.grazing);
            }
        }
    }

    TEST_CASE("what of a rectangle is below the horizon of a surface gives it nothing") {
        const Vec3 origin(0.0f);
        const Vec3 toViewer = glm::normalize(Vec3(0.0f, 1.0f, 1.0f));
        // A wall two metres ahead from half a metre below the floor to a metre above it, and
        // the part of it above the floor alone.
        const RectLightShape wall{
            .center = Vec3(0.0f, 0.25f, -2.0f), .right = Vec3(-1.0f, 0, 0), .up = Vec3(0, 0.75f, 0)};
        const RectLightShape aboveFloor{
            .center = Vec3(0.0f, 0.5f, -2.0f), .right = Vec3(-1.0f, 0, 0), .up = Vec3(0, 0.5f, 0)};
        for (const float roughness : {0.2f, 0.5f, 1.0f}) {
            const RectLightSpecular whole = rectLightSpecular(wall, origin, kUp, toViewer, roughness);
            const RectLightSpecular part = rectLightSpecular(aboveFloor, origin, kUp, toViewer, roughness);
            REQUIRE(part.lobe > 0.0f);
            CHECK(std::abs(whole.lobe / part.lobe - 1.0f) < 1.0e-4f);
            CHECK(std::abs(whole.grazing - part.grazing) <= 1.0e-4f * part.lobe);
        }
        const float wholeFactor = rectLightIrradiance(wall, origin, kUp).formFactor;
        const float partFactor = rectLightIrradiance(aboveFloor, origin, kUp).formFactor;
        CHECK(std::abs(wholeFactor / partFactor - 1.0f) < 1.0e-4f);

        // A light all below the floor gives it nothing.
        const RectLightShape sunk{
            .center = Vec3(0.0f, -1.0f, -2.0f), .right = Vec3(-1.0f, 0, 0), .up = Vec3(0, 0.5f, 0)};
        CHECK(rectLightSpecular(sunk, origin, kUp, toViewer, 0.5f).lobe == 0.0f);
        CHECK(rectLightIrradiance(sunk, origin, kUp).formFactor == 0.0f);

        // Nor does what is beyond the horizon of the cosine that stands in for the lobe. In the
        // fit's frame - x along the floor towards the viewer, which is +Z here, and z up - that
        // horizon is where m20 x + m22 z is nothing: low walls on the viewer's side reach across.
        const float angle = glm::radians(60.0f);
        const Vec3 viewer(0.0f, std::cos(angle), std::sin(angle));
        const LtcFit fit = ltcFit(0.5f, viewer.y);
        const float slope = -fit.matrix.y / fit.matrix.w;
        REQUIRE(slope > 0.1f);
        const float distance = 2.0f;
        const float cut = slope * distance;
        const auto wallBetween = [&](float bottom, float top) {
            return RectLightShape{.center = Vec3(0.0f, 0.5f * (bottom + top), distance),
                                  .right = Vec3(1.0f, 0.0f, 0.0f),
                                  .up = Vec3(0.0f, 0.5f * (top - bottom), 0.0f)};
        };
        const RectLightSpecular crossing =
            rectLightSpecular(wallBetween(0.3f * cut, 2.0f * cut), origin, kUp, viewer, 0.5f);
        const RectLightSpecular aboveIt = rectLightSpecular(wallBetween(cut, 2.0f * cut), origin, kUp, viewer, 0.5f);
        REQUIRE(aboveIt.lobe > 0.0f);
        CHECK(std::abs(crossing.lobe / aboveIt.lobe - 1.0f) < 1.0e-4f);
    }

    TEST_CASE("a rectangle gives nothing where there is nothing to give, and never what is no number") {
        const Vec3 origin(0.0f);
        const RectLightShape panel = facingOrigin(Vec3(0.0f, 2.0f, -1.0f), 1.0f, 0.5f);
        const Vec3 toViewer = glm::normalize(Vec3(0.0f, 1.0f, 1.0f));

        // A light of no size, and a point behind the light or in its plane.
        RectLightShape line = panel;
        line.up = Vec3(0.0f);
        CHECK(rectLightIrradiance(line, origin, kUp).formFactor == 0.0f);
        CHECK(rectLightSpecular(line, origin, kUp, toViewer, 0.5f).lobe == 0.0f);
        const Vec3 behind = panel.center * 1.5f;
        CHECK(rectLightSpecular(panel, behind, kUp, toViewer, 0.5f).lobe == 0.0f);
        CHECK(rectLightIrradiance(panel, panel.center + panel.right * 3.0f, kUp).formFactor == 0.0f);

        // Seen along the normal, the lobe is round, and any way along the surface is the same:
        // the frame of a viewer a little towards Z is turned a quarter from the one that is
        // taken for a viewer straight above.
        const RectLightSpecular straight = rectLightSpecular(panel, origin, kUp, kUp, 0.5f);
        const RectLightSpecular nearly =
            rectLightSpecular(panel, origin, kUp, glm::normalize(Vec3(0.0f, 1.0f, 1.0e-4f)), 0.5f);
        CHECK(straight.lobe > 0.0f);
        CHECK(std::abs(straight.lobe / nearly.lobe - 1.0f) < 1.0e-3f);

        // Rougher than nothing: below the least roughness, the least; numbers all the way for
        // a viewer below the surface, which a normal map can make, and for no roughness at all.
        CHECK(rectLightSpecular(panel, origin, kUp, toViewer, 0.0f).lobe ==
              rectLightSpecular(panel, origin, kUp, toViewer, kMinimumRoughness).lobe);
        const RectLightSpecular below = rectLightSpecular(panel, origin, kUp, -toViewer, 0.5f);
        CHECK(std::isfinite(below.lobe));
        CHECK(std::isfinite(below.grazing));
        const RectLightSpecular none =
            rectLightSpecular(panel, origin, kUp, toViewer, std::numeric_limits<float>::quiet_NaN());
        CHECK(std::isfinite(none.lobe));
        CHECK(isFinite(rectLightIrradiance(panel, origin, glm::normalize(Vec3(0.0f, 1.0f, -2.0f)))));
    }
}
