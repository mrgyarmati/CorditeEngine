// How a surface reflects light: the description of a shaded point and the functions
// that turn incoming light into outgoing light.
//
// The model is the usual metallic-roughness one: Lambert diffuse plus a GGX microfacet
// specular lobe with Smith visibility and Schlick's Fresnel approximation. Every pass
// that lights a surface, forward or deferred, uses these functions, so a material looks
// the same wherever it is drawn.

#pragma once

static const float kPi = 3.14159265358979;

// Below this, a perfectly smooth surface would turn a point light into a single
// infinitely bright pixel.
static const float kMinimumRoughness = 0.045;

// A point on a surface, ready to be lit. All colors are linear.
struct Surface
{
    float3 position;
    // Unit length, facing the viewer's side of the surface.
    float3 normal;
    // The normal of the surface itself - of the triangle, without what a normal map
    // makes of it - unit length and on the same side. Shadows are looked up with
    // this one: a normal map bends the light on a surface, not the surface.
    float3 geometricNormal;
    float3 baseColor;
    // 1 is opaque.
    float opacity;
    float metallic;
    float roughness;
    // Reflectivity of a non-metal: 0.5 is the usual 4%.
    float specular;
    // 1 where ambient light reaches the surface freely, 0 where none does.
    float occlusion;
    // Light the surface gives off by itself.
    float3 emissive;
    // The lighting channels the surface belongs to, one bit each: a light shines on
    // it when they share one.
    uint lightChannels;
};

// The color of the light scattered inside the surface: none for a metal.
float3 diffuseColor(Surface surface)
{
    return surface.baseColor * (1.0 - surface.metallic);
}

// The color reflected straight back at the viewer: a few percent of white for a
// non-metal, the base color for a metal.
float3 specularColor(Surface surface)
{
    return lerp(float3(0.08 * surface.specular), surface.baseColor, surface.metallic);
}

// GGX / Trowbridge-Reitz: how many microfacets face the half vector.
float distributionGgx(float nDotH, float alpha)
{
    const float alphaSquared = alpha * alpha;
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / (kPi * denominator * denominator);
}

// Smith's height-correlated visibility for GGX, including the 1 / (4 n.l n.v) of the
// microfacet equation.
float visibilitySmithGgx(float nDotL, float nDotV, float alpha)
{
    const float alphaSquared = alpha * alpha;
    const float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0 - alphaSquared) + alphaSquared);
    const float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / max(lambdaV + lambdaL, 1.0e-5);
}

// How bright a surface's reflection gets at grazing angles: white for anything that
// reflects at all. Real materials reflect at least 2%; less than that is taken to mean
// "no reflection wanted" and fades the grazing reflection out with it, so that a
// material with `specular` 0 is truly matte.
float grazingReflectance(float3 f0)
{
    return saturate(50.0 * f0.g);
}

// Schlick's approximation: reflectivity rises from `f0` towards white at grazing angles.
float3 fresnelSchlick(float3 f0, float vDotH)
{
    const float factor = pow(1.0 - saturate(vDotH), 5.0);
    return f0 * (1.0 - factor) + grazingReflectance(f0) * factor;
}

// The light leaving the surface towards the viewer for light arriving from one
// direction. `toLight` and `toViewer` are unit vectors pointing away from the surface;
// `radiance` is the light's color and strength at the surface.
float3 shadeDirect(Surface surface, float3 toViewer, float3 toLight, float3 radiance)
{
    const float nDotL = saturate(dot(surface.normal, toLight));
    if (nDotL <= 0.0)
    {
        return float3(0.0);
    }

    const float3 halfVector = normalize(toLight + toViewer);
    const float nDotV = max(dot(surface.normal, toViewer), 1.0e-4);
    const float nDotH = saturate(dot(surface.normal, halfVector));
    const float vDotH = saturate(dot(toViewer, halfVector));

    // Roughness is perceptual; the distribution wants its square.
    const float roughness = max(surface.roughness, kMinimumRoughness);
    const float alpha = roughness * roughness;

    const float3 fresnel = fresnelSchlick(specularColor(surface), vDotH);
    const float3 specular = fresnel * distributionGgx(nDotH, alpha) * visibilitySmithGgx(nDotL, nDotV, alpha);
    // What is reflected at the surface does not enter it.
    const float3 diffuse = (1.0 - fresnel) * diffuseColor(surface) / kPi;

    return (diffuse + specular) * radiance * nDotL;
}

// How much of the light from all around a rough surface reflects towards the viewer.
// A fitted curve that replaces a lookup table (Karis, "Physically Based Shading on
// Mobile", 2014).
float3 environmentReflectance(float3 f0, float roughness, float nDotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const float2 scaleBias = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + grazingReflectance(f0) * scaleBias.y;
}

// Ambient light from a sky above and a ground below, blended by how far a direction
// points up. `skyColor` and `groundColor` are the light arriving from each half.
float3 hemisphereLight(float3 direction, float3 skyColor, float3 groundColor)
{
    return lerp(groundColor, skyColor, direction.y * 0.5 + 0.5);
}

// The light leaving the surface towards the viewer under hemisphere ambient light.
float3 shadeHemisphere(Surface surface, float3 toViewer, float3 skyColor, float3 groundColor)
{
    const float nDotV = max(dot(surface.normal, toViewer), 1.0e-4);
    const float3 reflected = reflect(-toViewer, surface.normal);

    const float3 diffuse = diffuseColor(surface) * hemisphereLight(surface.normal, skyColor, groundColor);
    // A rough surface reflects a blur of its surroundings; towards full roughness that
    // is the same light the diffuse term sees.
    const float3 reflectedLight = lerp(hemisphereLight(reflected, skyColor, groundColor),
                                       hemisphereLight(surface.normal, skyColor, groundColor),
                                       surface.roughness);
    const float3 specular =
        reflectedLight * environmentReflectance(specularColor(surface), surface.roughness, nDotV);

    return (diffuse + specular) * surface.occlusion;
}
