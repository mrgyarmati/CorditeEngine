// What a rectangle that gives off light gives a surface: the integral of the cosine over
// it for what is matte, and for the GGX lobe a cosine that a matrix has turned into the
// lobe's shape - linearly transformed cosines (Heitz, Dupuy, Hill and Neubelt, 2016),
// with the fit of ltc_code at t12 and t13 of the view's bindings (space0). The arithmetic
// is written down twice: here and in engine/cordite/renderer/RectLights.cpp, which the
// tests hold the pictures to. Change both.

#pragma once

#include "common/Brdf.slang"
#include "common/Environment.slang"
#include "common/Math.slang"

// The fit (RectLights.h): for a roughness across and sqrt(1 - n.v) down, the inverse of
// the matrix that makes the lobe of a cosine, as (m00, m20, m02, m22), and what the lobe
// reflects with a reflectance of one and the part of that Schlick's factor gives at
// grazing angles. Read between their texels and clamped to the edges, which the
// environment's sampler does.
Texture2D<float4> rectLightMatrices : register(t12, space0);
Texture2D<float4> rectLightMagnitudes : register(t13, space0);

// Matches kLtcTableSize.
static const float kLtcTableSize = 64.0;

// A polygon as a point sees it: the corners as offsets from the point, in order. A
// rectangle has four, and gains one at the most each time it is cut by a plane through
// the point; it is cut twice.
struct LightPolygon
{
    float3 corners[6];
    uint count;
};

// What of a convex polygon lies above z = 0, in the same order. Matches aboveHorizon().
LightPolygon aboveHorizon(LightPolygon polygon)
{
    LightPolygon above = {};
    for (uint index = 0; index < polygon.count; ++index)
    {
        const float3 from = polygon.corners[index];
        const float3 to = polygon.corners[(index + 1) % polygon.count];
        const bool fromAbove = from.z > 0.0;
        if (fromAbove && above.count < 6)
        {
            above.corners[above.count++] = from;
        }
        // Where an edge crosses the horizon: only between corners on either side of it,
        // so the division is by more than nothing.
        if (fromAbove != (to.z > 0.0) && above.count < 6)
        {
            above.corners[above.count++] = lerp(from, to, from.z / (from.z - to.z));
        }
    }
    return above;
}

// The vector form factor of a polygon (Arvo, 1995): over every edge, the angle it spans
// as the point sees it, along the unit normal of the plane through it and the point,
// summed and divided by two pi. Its z is the integral of the cosine about +Z over the
// polygon, over pi - more than nothing for corners that turn clockwise as the point sees
// them - and it points the way the light of the polygon arrives on average. Matches
// vectorFormFactor().
float3 vectorFormFactor(LightPolygon polygon)
{
    float3 sum = float3(0.0);
    for (uint index = 0; index < polygon.count; ++index)
    {
        const float3 from = normalizeOr(polygon.corners[index], float3(0.0));
        const float3 to = normalizeOr(polygon.corners[(index + 1) % polygon.count], float3(0.0));
        const float3 normal = cross(from, to);
        const float sine = length(normal);
        // An edge that the point sees as a point spans no angle.
        if (sine > 1.0e-6)
        {
            sum += normal * (atan2(sine, dot(from, to)) / sine);
        }
    }
    return sum / (2.0 * kPi);
}

// A frame about the normal: x along the surface towards the viewer, y across, z the
// normal. Seen along the normal any way along the surface will do: there the lobe is
// round. Matches frameAbout().
void frameAbout(float3 normal, float3 toViewer, out float3 x, out float3 y)
{
    const float3 anyTangent = abs(normal.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 fallback = normalize(anyTangent - normal * dot(anyTangent, normal));
    x = normalizeOr(toViewer - normal * dot(toViewer, normal), fallback);
    y = cross(normal, x);
}

// The corners of a rectangle - its middle, and from there to the middles of its right and
// its top edges - as offsets from `position` in the frame (x, y, normal):
// counter-clockwise seen from behind the light, and so clockwise as a point in front of
// it sees them. Matches cornersIn().
LightPolygon cornersIn(float3 x, float3 y, float3 normal, float3 center, float3 right, float3 up, float3 position)
{
    const float3 offsets[4] = {center - right - up - position, center + right - up - position,
                               center + right + up - position, center - right + up - position};
    LightPolygon polygon = {};
    polygon.count = 4;
    for (uint index = 0; index < 4; ++index)
    {
        polygon.corners[index] = float3(dot(x, offsets[index]), dot(y, offsets[index]), dot(normal, offsets[index]));
    }
    return polygon;
}

// The fit at a roughness and n.v, both clamped to 0..1: the texels at those times
// kLtcTableSize - 1, read between their middles. Matches ltcFit().
void ltcFit(float roughness, float nDotV, out float4 inverseMatrix, out float4 magnitude)
{
    const float2 values = float2(saturate(roughness), sqrt(1.0 - saturate(nDotV)));
    const float2 uv = values * ((kLtcTableSize - 1.0) / kLtcTableSize) + 0.5 / kLtcTableSize;
    inverseMatrix = rectLightMatrices.SampleLevel(environmentSampler, uv, 0.0);
    magnitude = rectLightMagnitudes.SampleLevel(environmentSampler, uv, 0.0);
}

// What a rectangle of light gives a matte surface: the integral of the cosine over what
// of it is above the surface's horizon, over pi, and the way its light arrives on
// average: unit length, or nothing where nothing arrives. Matches RectLightIrradiance.
struct RectLightIrradiance
{
    float formFactor;
    float3 direction;
};

// What the GGX lobe of a surface reflects of a rectangle of light, with a reflectance of
// one, and the part of that Schlick's factor gives to the reflectance at grazing angles.
// Matches RectLightSpecular.
struct RectLightSpecular
{
    float lobe;
    float grazing;
};

// Whether `position` is in front of the face of a rectangle that shines towards
// cross(up, right). Matches isInFront().
bool isInFrontOfRectangle(float3 center, float3 right, float3 up, float3 position)
{
    return dot(position - center, cross(up, right)) > 0.0;
}

// What a matte surface at `position` with the unit `normal` gets of a rectangle: nothing
// where it is behind the light, or in the plane of it. Matches rectLightIrradiance().
RectLightIrradiance rectLightIrradiance(float3 center, float3 right, float3 up, float3 position, float3 normal)
{
    RectLightIrradiance irradiance = {};
    if (!isInFrontOfRectangle(center, right, up, position))
    {
        return irradiance;
    }
    float3 x;
    float3 y;
    frameAbout(normal, normal, x, y);
    const float3 factor = vectorFormFactor(aboveHorizon(cornersIn(x, y, normal, center, right, up, position)));
    if (factor.z > 0.0)
    {
        irradiance.formFactor = factor.z;
        irradiance.direction = normalizeOr(x * factor.x + y * factor.y + normal * factor.z, normal);
    }
    return irradiance;
}

// What the GGX lobe of a surface at `position`, with the unit `normal` and a `roughness`
// (perceptual; kMinimumRoughness at the least), reflects of a rectangle along the unit
// `toViewer`: the rectangle cut at the surface's horizon, taken through the inverse of
// the fit's matrix and cut again at the horizon of the cosine there. Matches
// rectLightSpecular().
RectLightSpecular rectLightSpecular(float3 center, float3 right, float3 up, float3 position, float3 normal,
                                    float3 toViewer, float roughness)
{
    RectLightSpecular specular = {};
    if (!isInFrontOfRectangle(center, right, up, position))
    {
        return specular;
    }
    float3 x;
    float3 y;
    frameAbout(normal, toViewer, x, y);
    const LightPolygon visible = aboveHorizon(cornersIn(x, y, normal, center, right, up, position));
    if (visible.count < 3)
    {
        return specular;
    }
    float4 inverseMatrix;
    float4 magnitude;
    ltcFit(max(roughness, kMinimumRoughness), dot(normal, toViewer), inverseMatrix, magnitude);
    LightPolygon cosine = visible;
    for (uint index = 0; index < visible.count; ++index)
    {
        const float3 corner = visible.corners[index];
        cosine.corners[index] = float3(inverseMatrix.x * corner.x + inverseMatrix.z * corner.z, corner.y,
                                       inverseMatrix.y * corner.x + inverseMatrix.w * corner.z);
    }
    const float integral = vectorFormFactor(aboveHorizon(cosine)).z;
    if (integral > 0.0)
    {
        specular.lobe = integral * magnitude.x;
        specular.grazing = integral * magnitude.y;
    }
    return specular;
}

// How much of everything around a point the triangle of the unit vectors a, b and c
// fills, in steradians (Van Oosterom and Strackee, 1983).
float triangleSolidAngle(float3 a, float3 b, float3 c)
{
    return 2.0 * atan2(abs(dot(a, cross(b, c))), 1.0 + dot(a, b) + dot(b, c) + dot(c, a));
}

// How much of everything around `position` a rectangle fills, as a point there sees it,
// in steradians: nothing where the point is behind it or in its plane. Matches
// rectLightSolidAngle().
float rectLightSolidAngle(float3 center, float3 right, float3 up, float3 position)
{
    if (!isInFrontOfRectangle(center, right, up, position))
    {
        return 0.0;
    }
    const float3 a = normalizeOr(center - right - up - position, float3(0.0));
    const float3 b = normalizeOr(center + right - up - position, float3(0.0));
    const float3 c = normalizeOr(center + right + up - position, float3(0.0));
    const float3 d = normalizeOr(center - right + up - position, float3(0.0));
    return triangleSolidAngle(a, b, c) + triangleSolidAngle(a, c, d);
}
