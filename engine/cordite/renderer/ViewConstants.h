#pragma once

/// @file
/// The constants every pass that draws a scene is given about its view, as the shaders
/// lay them out: the C++ side of `ViewConstants` in shaders/common/View.slang.
///
/// Two things fill them in: ViewBindings, for the view of a camera, and ShadowAtlas, for
/// the views lights have of the scene. Passes do not use this struct; they bind what
/// those two provide.
///
/// @code
/// ViewConstants constants{};
/// constants.viewProjection = shadow.viewProjection;
/// commandList->writeBuffer(constantBuffer, &constants, sizeof(constants));
/// @endcode

#include "cordite/math/Math.h"

#include <array>
#include <cstdint>

namespace cordite {

/// How many views can be on their way through the GPU at once: a few views in each of
/// a few frames (FramePacer keeps it to a few). The constants are rewritten for every
/// one of them, so whatever writes them keeps this many versions of its buffer.
inline constexpr uint32_t kMaxViewsInFlight = 64;

struct ViewConstants {
    Mat4 viewProjection;
    Mat4 inverseViewProjection;
    /// The world to clip space as the view was when it was last drawn, for its motion
    /// vectors (MotionVectors.h): viewProjection where it goes on from no picture before.
    Mat4 previousViewProjection;
    Vec3 cameraPosition;
    /// 1 for a camera without perspective, 0 for one with.
    float orthographic;
    Vec3 cameraForward;
    uint32_t directionalLightCount;
    Vec3 skyColor;
    float clusterDepthScale;
    Vec3 groundColor;
    float clusterDepthBias;
    Vec2 targetSize;
    Vec2 inverseTargetSize;
    /// Where each cascade of the sun's shadow ends, in metres from the camera.
    Vec4 cascadeEnds;
    uint32_t cascadeCount;
    /// Where the sun's shadows start to fade out, and where they are gone.
    float shadowFadeStart;
    float shadowFadeEnd;
    /// How many shadow maps the lights of the view have between them: 0 when nothing
    /// in it casts a shadow.
    uint32_t shadowViewCount;
    /// How far the filter that softens shadows reaches, in texels.
    uint32_t shadowFilterRadius;
    /// The environment (shaders/common/Environment.slang): how bright it is - 0 when
    /// there is none -, the last level of its cube map, and 1 when its sky is drawn
    /// where nothing else is.
    float environmentIntensity;
    float environmentLastLevel;
    float environmentVisible;
    /// The sine and the cosine of the angle the environment is turned by about the
    /// vertical axis.
    Vec2 environmentTurn;
    /// 1 when the ambient occlusion of the view's targets holds this frame's, and the
    /// lighting is to read it.
    float screenOcclusion;
    float padding0;
    /// What the environment lights a matte surface with, as nine spherical harmonics
    /// (rgb; the fourth number is not used).
    std::array<Vec4, 9> environmentDiffuse;
    /// The fog (shaders/common/Fog.slang; FogSettings in Fog.h says what the numbers
    /// are). A density of 0 is no fog.
    Vec3 fogColor;
    float fogDensity;
    Vec3 fogSunColor;
    float fogSunExponent;
    Vec3 fogTowardsSun;
    float fogHeight;
    float fogHeightFalloff;
    float fogStartDistance;
    float fogMaxOpacity;
    float padding1;
    /// What the targets of the view were cleared to: what is seen where nothing is
    /// drawn and no sky is shown.
    Vec3 backgroundColor;
    float padding2;
};

static_assert(sizeof(ViewConstants) == 560, "must match the cbuffer layout in View.slang");

} // namespace cordite
