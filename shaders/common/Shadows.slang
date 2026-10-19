// Shadows: how much of a light reaches a point.
//
// A shadow map is the depth of the scene as a light sees it. A point is in shadow when
// it is farther from the light than the map says something is in its direction. All
// the shadow maps of a view are tiles of one texture, the shadow atlas, bound next to
// the view's constants (space0): the views at t3, the atlas at t4 and the sampler that
// compares a depth with it at s0.
//
// The C++ side - which light gets which maps, and what they show - is
// engine/cordite/renderer/ShadowPlan.h.

#pragma once

#include "common/Brdf.slang"
#include "common/View.slang"

// Matches GpuShadowView in engine/cordite/renderer/ViewBindings.cpp.
struct ShadowView
{
    // World space to the atlas: x and y from (0, 0) at its top left to (1, 1), z the
    // depth as the light sees it - 0 at the light, 1 at the end of what it sees - all
    // three to be divided by w.
    float4x4 worldToAtlas;
    // The part of the atlas the filter may read for this map: the smallest and the
    // largest x and y.
    float4 bounds;
    // The directions in the world in which the map's x and y grow.
    float3 right;
    // The size of a texel in the world: in metres for a directional light, in metres
    // one metre from the light for the others.
    float texelSize;
    float3 up;
    // 1 for the maps of point, spot and rect lights: their texels grow with the distance
    // from the light, measured along the way the map looks.
    float perspective;
};

StructuredBuffer<ShadowView> shadowViews : register(t3, space0);
Texture2D<float> shadowAtlas : register(t4, space0);
SamplerComparisonState shadowSampler : register(s0, space0);

// The share of a cascade's depth, at its far end, over which the sun's shadow goes over
// into the next cascade's. Matches kCascadeBlend in ShadowPlan.cpp, which makes each
// cascade begin that much before the one in front of it ends.
static const float kCascadeBlend = 0.1;

// A surface the light strikes flatter than this (the cosine of 87 degrees) is filtered
// as if it struck it so: a plane seen edge-on has no depth to follow.
static const float kFlattestLight = 0.05;

// Which of the six shadow maps of a point light a point is looked up in; `offset` is
// from the light to the point. Matches pointShadowFace() in ShadowPlan.cpp.
uint pointShadowFace(float3 offset)
{
    const float3 size = abs(offset);
    if (size.x >= size.y && size.x >= size.z)
    {
        return offset.x >= 0.0 ? 0 : 1;
    }
    if (size.y >= size.z)
    {
        return offset.y >= 0.0 ? 2 : 3;
    }
    return offset.z >= 0.0 ? 4 : 5;
}

// Where a surface is looked up in a shadow map. Exactly where it is, it would shadow
// itself: the map knows the depth at the middle of each texel only, and a surface the
// light does not strike head-on is nearer than that on one side of a texel and farther
// on the other. So it is looked up a little towards the light, and a little off the
// surface along its normal - the more, the flatter the light strikes it, and both in
// proportion to what a texel covers there (`texelWorld`, in metres).
float3 shadowLookupPosition(Surface surface, float3 toLight, float texelWorld, float bias, float normalOffset)
{
    // By the surface's own normal, not the one its normal map makes of it: it is the
    // surface that the map has the depth of.
    const float nDotL = saturate(dot(surface.geometricNormal, toLight));
    const float slope = sqrt(saturate(1.0 - nDotL * nDotL));
    return surface.position + surface.geometricNormal * (normalOffset * texelWorld * slope) +
           toLight * (bias * texelWorld);
}

// How much of a light reaches `position` according to one shadow map: 1 all of it, 0
// none. The map is read at several places around the point and the answers averaged,
// which makes the edge of a shadow soft. What the map does not see is lit.
//
// The places are not beside the point in the map but beside it on its own surface: each
// is a step along the map's axes and then along the light, back onto the plane through
// the point with the normal `normal`. A flat surface is as far from the light at each of
// them as the map says, and does not shadow itself however far the filter reaches.
// `normal` is that of the surface itself (Surface::geometricNormal), `texelWorld` what a
// texel covers at the point, in metres.
float shadowMapVisibility(ShadowView shadow, float3 position, float3 normal, float3 toLight, float texelWorld)
{
    const float4 centre = mul(shadow.worldToAtlas, float4(position, 1.0));
    if (centre.w <= 0.0)
    {
        return 1.0;
    }
    const float centreDepth = centre.z / centre.w;
    if (centreDepth <= 0.0 || centreDepth >= 1.0)
    {
        return 1.0;
    }
    // A map with perspective can see less than its light reaches: that of a spot light
    // ends 85 degrees from the light's axis, however wide the cone is. What is beside
    // the map is lit, like what is beyond it. (A cascade of the sun holds whatever is
    // looked up in it; the offsets may carry a lookup a little past its edge, and the
    // clamp below brings that back.)
    const float2 centreUv = centre.xy / centre.w;
    if (shadow.perspective > 0.5 && (any(centreUv < shadow.bounds.xy) || any(centreUv > shadow.bounds.zw)))
    {
        return 1.0;
    }

    // How far the surface falls away from the light for each metre along the map's axes.
    const float facing = max(dot(normal, toLight), kFlattestLight);
    const float fallRight = dot(shadow.right, normal) / facing;
    const float fallUp = dot(shadow.up, normal) / facing;

    const int radius = int(shadowFilterRadius);
    float lit = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const float2 step = float2(x, y) * texelWorld;
            const float3 beside = position + shadow.right * step.x + shadow.up * step.y -
                                  toLight * (step.x * fallRight + step.y * fallUp);
            const float4 place = mul(shadow.worldToAtlas, float4(beside, 1.0));
            const float3 coordinates = place.xyz / max(place.w, 1.0e-6);
            // Never outside the map's own tile: next to it is another light's map.
            const float2 uv = clamp(coordinates.xy, shadow.bounds.xy, shadow.bounds.zw);
            // Each read compares with four texels and blends the answers. Beyond the
            // far end of the map there is nothing that could stand in the light.
            lit += coordinates.z >= 1.0
                       ? 1.0
                       : shadowAtlas.SampleCmpLevelZero(shadowSampler, uv, max(coordinates.z, 0.0));
        }
    }
    const float side = float(2 * radius + 1);
    return lit / (side * side);
}

// How much of a point, spot or rect light reaches the surface. `firstView` is the light's
// (first) shadow view, `isPointLight` whether it has the six maps of a point light - a
// rect light has those of a point light in its middle - `lightDirection` the unit vector
// along which a spot light shines, and `toLight` the unit vector from the surface towards
// the light.
float localLightVisibility(int firstView, bool isPointLight, float3 lightPosition, float3 lightDirection, float bias,
                           float normalOffset, Surface surface, float3 toLight)
{
    if (firstView < 0)
    {
        return 1.0;
    }
    // A texel of a map with perspective covers as many times what it covers a metre
    // from the light as the point is metres away - measured along the way the map
    // looks, not along the way to the point. A spot light's map looks along the light;
    // of the six maps of a point light, which are alike, the point is in the one that
    // looks along the largest part of its offset.
    const float3 fromLight = surface.position - lightPosition;
    const float3 apart = abs(fromLight);
    const float depth = isPointLight ? max(apart.x, max(apart.y, apart.z)) : dot(fromLight, lightDirection);
    const float texelWorld = shadowViews[firstView].texelSize * max(depth, 0.0);
    const float3 position = shadowLookupPosition(surface, toLight, texelWorld, bias, normalOffset);
    const uint view = uint(firstView) + (isPointLight ? pointShadowFace(position - lightPosition) : 0);
    return shadowMapVisibility(shadowViews[view], position, surface.geometricNormal, toLight, texelWorld);
}

// How much of the sun reaches the surface through one of its cascades.
float cascadeVisibility(int firstView, uint cascade, float bias, float normalOffset, Surface surface, float3 toLight)
{
    const ShadowView shadow = shadowViews[uint(firstView) + cascade];
    const float3 position = shadowLookupPosition(surface, toLight, shadow.texelSize, bias, normalOffset);
    return shadowMapVisibility(shadow, position, surface.geometricNormal, toLight, shadow.texelSize);
}

// How much of the sun - the directional light that casts shadows - reaches the surface.
// The cascade is chosen by how far the surface is from the camera; towards the end of a
// cascade the next one is blended in, and towards the shadow distance the shadow fades.
float sunVisibility(int firstView, float bias, float normalOffset, Surface surface, float3 toLight)
{
    if (firstView < 0 || cascadeCount == 0)
    {
        return 1.0;
    }
    const float depth = viewDepth(surface.position);
    if (depth >= shadowFadeEnd)
    {
        return 1.0;
    }

    uint cascade = 0;
    float start = 0.0;
    for (uint index = 0; index + 1 < cascadeCount; ++index)
    {
        if (depth > cascadeEnds[index])
        {
            cascade = index + 1;
            start = cascadeEnds[index];
        }
    }
    float visibility = cascadeVisibility(firstView, cascade, bias, normalOffset, surface, toLight);

    const float end = cascadeEnds[cascade];
    const float blendStart = end - (end - start) * kCascadeBlend;
    if (cascade + 1 < cascadeCount && depth > blendStart)
    {
        const float next = cascadeVisibility(firstView, cascade + 1, bias, normalOffset, surface, toLight);
        visibility = lerp(visibility, next, saturate((depth - blendStart) / max(end - blendStart, 1.0e-4)));
    }

    const float fade = saturate((depth - shadowFadeStart) / max(shadowFadeEnd - shadowFadeStart, 1.0e-4));
    return lerp(visibility, 1.0, fade);
}

// The cascade of the sun's shadow that a point `depth` metres in front of the camera is
// looked up in, or -1 where the sun casts no shadow: for the debug view, and for points
// in the air.
int shadowCascadeAt(float depth)
{
    if (cascadeCount == 0 || depth >= shadowFadeEnd)
    {
        return -1;
    }
    int cascade = 0;
    for (uint index = 0; index + 1 < cascadeCount; ++index)
    {
        if (depth > cascadeEnds[index])
        {
            cascade = int(index) + 1;
        }
    }
    return cascade;
}

// How much of a light reaches `position`, a point in the air rather than on a surface,
// according to one shadow map: one read, which compares with the four texels around the
// point and blends the answers. A point in the air needs none of the offsets that keep a
// surface out of its own shadow, and the light of a fog is soft enough without a filter.
// What the map does not see is lit.
float pointVisibilityIn(ShadowView shadow, float3 position)
{
    const float4 place = mul(shadow.worldToAtlas, float4(position, 1.0));
    if (place.w <= 0.0)
    {
        return 1.0;
    }
    const float3 coordinates = place.xyz / place.w;
    if (coordinates.z <= 0.0 || coordinates.z >= 1.0)
    {
        return 1.0;
    }
    if (shadow.perspective > 0.5 && (any(coordinates.xy < shadow.bounds.xy) || any(coordinates.xy > shadow.bounds.zw)))
    {
        return 1.0;
    }
    // Never outside the map's own tile: next to it is another light's map.
    const float2 uv = clamp(coordinates.xy, shadow.bounds.xy, shadow.bounds.zw);
    return shadowAtlas.SampleCmpLevelZero(shadowSampler, uv, coordinates.z);
}

// How much of a point, spot or rect light reaches a point in the air; `firstView` is the
// light's (first) shadow view.
float localLightVisibilityAt(int firstView, bool isPointLight, float3 lightPosition, float3 position)
{
    if (firstView < 0)
    {
        return 1.0;
    }
    const uint view = uint(firstView) + (isPointLight ? pointShadowFace(position - lightPosition) : 0);
    return pointVisibilityIn(shadowViews[view], position);
}

// How much of the sun - the directional light that casts shadows - reaches a point in the
// air: through the cascade its depth falls in, fading out towards the shadow distance.
float sunVisibilityAt(int firstView, float3 position)
{
    const float depth = viewDepth(position);
    const int cascade = shadowCascadeAt(depth);
    if (firstView < 0 || cascade < 0)
    {
        return 1.0;
    }
    const float visibility = pointVisibilityIn(shadowViews[uint(firstView) + uint(cascade)], position);
    const float fade = saturate((depth - shadowFadeStart) / max(shadowFadeEnd - shadowFadeStart, 1.0e-4));
    return lerp(visibility, 1.0, fade);
}
