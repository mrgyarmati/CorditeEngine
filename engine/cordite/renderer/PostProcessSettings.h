#pragma once

/// @file
/// How the picture of a view is finished: what happens between the light the renderer
/// has worked out and the colors a screen shows - and what of the look of a scene is
/// worked out from the screen before that (ambient occlusion, AmbientOcclusion.h).
///
/// The renderer computes light without an upper limit; a display has one. Finishing a
/// picture is, in this order:
///
/// 1. the lens: chromatic aberration parts the colors towards the corners;
/// 2. bloom: light that is brighter than the threshold spills over what is around it;
/// 3. exposure: all of it is made brighter or darker, by a fixed amount or by what the
///    picture as a whole needs;
/// 4. white balance, and the vignette that darkens the corners;
/// 5. tone mapping: the unlimited range is bent into the range of a display;
/// 6. grading: saturation, contrast, lift / gamma / gain, and a lookup table that can
///    hold any change of color an image editor can make;
/// 7. the encoding a screen expects;
/// 8. anti-aliasing: the steps of slanted edges are smoothed, from the picture itself;
/// 9. film grain - last, or the smoothing would take every grain for an edge.
///
/// PostProcessSettings holds the numbers of all that. A scene sets them with
/// PostProcess components (RenderComponents.h, ScenePostProcess.h), which say where they
/// apply; code that draws a view by itself can pass them directly:
///
/// @code
/// PostProcessSettings look;
/// look.toneMapper = ToneMapper::Aces;
/// look.exposure = 0.5f;             // half a stop brighter
/// look.saturation = 0.85f;
///
/// const Vec3 shown = applyToneMapper(look.toneMapper, sceneLight * exposureScale(look.exposure));
/// @endcode
///
/// The curves and the arithmetic are written down twice, here and in
/// shaders/common/ToneMapping.slang and shaders/passes/PostProcess.slang: tests hold the
/// pictures the shaders make against what the functions of this file say.

#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

/// How light without an upper limit is brought into the range of a display. Both curves
/// leave a mid-grey of 0.18 where it is, so that changing the curve does not change how
/// bright a picture is as a whole - only what becomes of its shadows and highlights.
enum class ToneMapper : uint8_t {
    /// A curve like that of film: shadows keep their depth, highlights roll off slowly
    /// and reach white at about four times the brightness of a white sheet of paper
    /// (J. Hable, "Filmic Tonemapping Operators", 2010). Soft contrast, muted colors.
    Filmic,
    /// The look of the Academy Color Encoding System in the short form K. Narkowicz
    /// fitted to it (2016): more contrast and more saturation than Filmic.
    Aces,
    /// No curve at all: what is brighter than a display can show is cut off.
    Linear,
};

inline constexpr std::array<ToneMapper, 3> kToneMappers = {ToneMapper::Filmic, ToneMapper::Aces, ToneMapper::Linear};

/// The name used in files and by tools: "filmic", "aces", "linear".
std::string_view nameOf(ToneMapper toneMapper);

/// How the exposure of a view is chosen.
enum class ExposureMode : uint8_t {
    /// `exposure` is all there is.
    Manual,
    /// The view measures how bright its picture is and sets itself so that the average
    /// comes out as mid-grey, slowly, as an eye gets used to light; `exposure` is added
    /// to what it finds.
    Automatic,
};

/// The name used in files and by tools: "manual", "automatic".
std::string_view nameOf(ExposureMode mode);

/// How much finishing the picture may cost. From low to high the look stays; what gives
/// is how finely it is worked out.
enum class PostProcessQuality : uint8_t {
    /// The picture is not finished at all: the light is shown as it was worked out, and
    /// what is brighter than white is cut off. For looking at what the renderer made,
    /// and for tests that state what a pixel must be.
    Off,
    /// Bloom from a quarter of the resolution; no chromatic aberration, no film grain.
    Low,
    Medium,
    /// Bloom that reaches further.
    High,
};

inline constexpr std::array<PostProcessQuality, 4> kPostProcessQualities = {
    PostProcessQuality::Off, PostProcessQuality::Low, PostProcessQuality::Medium, PostProcessQuality::High};

/// The name used in files and by tools: "off", "low", "medium", "high".
std::string_view nameOf(PostProcessQuality quality);

/// The name shown to people: "Off", "Low", "Medium", "High".
std::string_view displayNameOf(PostProcessQuality quality);

/// The quality with the given nameOf(), or nothing. Upper and lower case are not told
/// apart.
[[nodiscard]] std::optional<PostProcessQuality> postProcessQualityFromName(std::string_view name);

/// How the steps that slanted edges have in a picture made of pixels are smoothed.
/// A picture that is not finished (PostProcessQuality::Off, debug views) never is.
enum class AntiAliasing : uint8_t {
    /// Not at all: every pixel has the color of what is in its middle.
    Off,
    /// From the finished picture alone ("fast approximate anti-aliasing", T. Lottes,
    /// 2011). Where neighbouring pixels differ enough to be an edge, the edge is
    /// followed both ways to where its step ends, and the pixel is blended with its
    /// neighbour across the edge by as much as the edge must cover of it. One more
    /// pass over the picture; nothing lags behind what moves, and nothing in a scene
    /// has to know of it. What it cannot mend is what is finer than a pixel to begin
    /// with - a wire, a glint - which flickers as it moves. How far edges are
    /// followed goes by the PostProcessQuality.
    Fxaa,
};

inline constexpr std::array<AntiAliasing, 2> kAntiAliasingModes = {AntiAliasing::Off, AntiAliasing::Fxaa};

/// The name used in files and by tools: "off", "fxaa".
std::string_view nameOf(AntiAliasing antiAliasing);

/// The name shown to people: "Off", "FXAA".
std::string_view displayNameOf(AntiAliasing antiAliasing);

/// The way of anti-aliasing with the given nameOf(), or nothing. Upper and lower case
/// are not told apart.
[[nodiscard]] std::optional<AntiAliasing> antiAliasingFromName(std::string_view name);

/// The numbers of finishing a picture. As they are here, a picture is tone mapped and
/// otherwise left alone, but for a little bloom around what is brighter than white.
struct PostProcessSettings {
    ExposureMode exposureMode = ExposureMode::Manual;
    /// In stops: each one doubles (or, negative, halves) the light. With automatic
    /// exposure it is added to what the view chooses.
    float exposure = 0.0f;
    /// Automatic exposure: the fewest and the most stops the view may choose.
    float autoExposureMinimum = -3.0f;
    float autoExposureMaximum = 3.0f;
    /// Automatic exposure: how fast the view gets used to a brighter picture and to a
    /// darker one. What is left of the way shrinks to about a third (1 / e) in one
    /// second divided by the speed. Eyes take longer for the dark.
    float adaptationSpeedUp = 3.0f;
    float adaptationSpeedDown = 1.0f;

    ToneMapper toneMapper = ToneMapper::Filmic;

    /// How much of the bloom is added to the picture: 0 none.
    float bloomIntensity = 0.15f;
    /// Light brighter than this blooms; 1 is white. At 0 everything does, a little.
    float bloomThreshold = 1.0f;
    /// How far the bloom spreads: 0 keeps it tight around what is bright, 1 spreads it
    /// over much of the picture.
    float bloomScatter = 0.65f;

    /// White balance: positive makes the picture warmer (more red, less blue), negative
    /// cooler; -1 to 1.
    float temperature = 0.0f;
    /// White balance: positive towards magenta, negative towards green; -1 to 1.
    float tint = 0.0f;
    /// 1 leaves the picture as it is; less is flatter, more is harder. Mid-grey stays.
    float contrast = 1.0f;
    /// 1 leaves the picture as it is; 0 is black and white.
    float saturation = 1.0f;
    /// Lift raises the shadows (0 leaves them), gamma bends the mid-tones (1 leaves
    /// them), gain scales the highlights (1 leaves them); each per color.
    Vec3 lift{0.0f};
    Vec3 gamma{1.0f};
    Vec3 gain{1.0f};
    /// A lookup table for the colors of the finished picture: an image asset of N * N
    /// by N pixels, N slices side by side (see docs/architecture.md). Nil for none.
    Uuid colorLut;
    /// How much of the lookup table's colors is taken: 0 none, 1 all.
    float colorLutIntensity = 1.0f;

    /// How dark the corners of the picture get: 0 not at all, 1 black.
    float vignette = 0.0f;
    /// How far red and blue part in the corners of the picture, in hundredths of its
    /// size: the fringes of a cheap lens.
    float chromaticAberration = 0.0f;
    /// How much film grain is laid over the picture: 0 none, 1 a lot.
    float filmGrain = 0.0f;

    /// Ambient occlusion: how much darker the light from all around gets where
    /// something stands in its way - in creases, under things, at the foot of a wall.
    /// 0 is not at all, 1 as much as the geometry says, more than that exaggerated.
    /// The light of the lights is not touched: shadows are theirs.
    float ambientOcclusionIntensity = 1.0f;
    /// Ambient occlusion: how far from a point things are looked for that stand in its
    /// light, in metres.
    float ambientOcclusionRadius = 0.5f;
    /// Ambient occlusion: how far something has to rise above a surface before it
    /// counts as standing in its light, as an angle in degrees. More of it hides the
    /// streaks a surface seen at a low angle can show, and takes from shallow creases.
    float ambientOcclusionBias = 2.0f;
};

/// `settings` with every number inside the range it is documented for, and "not a
/// number" replaced by what the setting is when nothing is said. What reaches the GPU
/// has been through here.
[[nodiscard]] PostProcessSettings sanitized(const PostProcessSettings& settings);

/// The settings `amount` of the way from `from` to `to`: 0 is `from`, 1 is `to`.
/// Numbers are blended; what cannot be - the tone mapper, the exposure mode, the
/// lookup table - changes over half way. A lookup table that only one of the two has
/// fades in or out through its intensity instead.
[[nodiscard]] PostProcessSettings blend(const PostProcessSettings& from, const PostProcessSettings& to, float amount);

/// What `stops` of exposure multiply light by: two to the power of them.
[[nodiscard]] float exposureScale(float stops);

/// What a white balance multiplies red, green and blue by. The brightness of white
/// stays what it was. `temperature` and `tint` are each -1 to 1.
[[nodiscard]] Vec3 whiteBalanceScale(float temperature, float tint);

/// What a tone mapper makes of light: linear, and between 0 and 1 for a display.
/// Negative light is taken as none.
[[nodiscard]] Vec3 applyToneMapper(ToneMapper toneMapper, const Vec3& light);

/// What grading makes of a color of the tone-mapped picture: saturation, then contrast,
/// then lift, gamma and gain. In and out are linear, between 0 and 1. `settings` are
/// taken as they are: bring them into their ranges first (sanitized()).
[[nodiscard]] Vec3 applyGrading(const PostProcessSettings& settings, const Vec3& color);

/// How much of the light is left at a place of the picture by a vignette: `fromCentre`
/// is -1 to 1 across the picture in both directions, (0, 0) its middle.
[[nodiscard]] float vignetteScale(float vignette, const Vec2& fromCentre);

/// What a quality comes to, for a picture that is finished: with PostProcessQuality::Off
/// none is, and the numbers are those of the lowest quality.
struct PostProcessQualitySettings {
    /// The level of the bloom chain that the bloom starts from: 0 is half the
    /// resolution of the picture, 1 a quarter.
    uint32_t bloomFirstLevel = 0;
    /// How many levels the bloom is spread over, each half the size of the one before.
    uint32_t bloomLevels = 5;
    /// Whether chromatic aberration and film grain are drawn at all.
    bool lensEffects = true;
    /// Anti-aliasing: how many times an edge is looked along, either way, for where
    /// its step ends - 5, 8 or 12; the more, the shallower the slants that come out
    /// smooth.
    uint32_t antiAliasingSteps = 8;
    /// Anti-aliasing: what counts as an edge. A pixel and its four neighbours must
    /// differ in brightness by this share of the brightest of them, and by this much
    /// (of white, as a display has it) at the least. The lower, the more of the
    /// picture is looked at.
    float antiAliasingEdgeThreshold = 0.166f;
    float antiAliasingEdgeMinimum = 0.0625f;

    bool operator==(const PostProcessQualitySettings&) const = default;
};

[[nodiscard]] PostProcessQualitySettings postProcessSettingsFor(PostProcessQuality quality);

} // namespace cordite
