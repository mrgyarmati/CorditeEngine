#pragma once

/// @file
/// Environments on the GPU: images of everything around a scene, made ready to light it.
///
/// An environment starts as one image asset - an HDR photograph of a sky, say. To light
/// a scene with it, three things are made of it:
///
/// | Member of GpuEnvironment | Is                                           | Lights              |
/// |--------------------------|----------------------------------------------|---------------------|
/// | `diffuse`                | Nine numbers per color (spherical harmonics) | matte surfaces      |
/// | `radiance`               | A cube map, blurred more level by level      | reflections         |
/// | `sky`                    | The image itself                             | the sky, where seen |
///
/// @code
/// CORDITE_TRY_ASSIGN(EnvironmentMaps environments, EnvironmentMaps::create(device, shaders, assets));
///
/// // In a frame, before the passes that are lit by it:
/// const GpuEnvironment* sky = environments.environment(commandList, skyImageId, resources);
/// resources.flushUploads();
/// view.update(commandList, sceneView, lightClusters, shadows,
///             {.environment = sky != nullptr ? sky : &environments.none(), .intensity = 1.0f, .visible = true});
/// @endcode
///
/// The cube map is made the first time an environment is asked for, by three dozen small
/// passes recorded into the command list given - once - and kept until the image
/// changes or the environment has not been asked for in a while. The shader side is
/// shaders/common/Environment.slang.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/SphericalHarmonics.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

namespace cordite {

/// The cube map of an environment is this many texels a side at its sharpest, ...
inline constexpr uint32_t kEnvironmentFaceSize = 256;

/// ... and has this many levels, each half the size of the one before: one for each of
/// as many steps of roughness, from a mirror to the roughest surface.
inline constexpr uint32_t kEnvironmentLevels = 6;

/// The memory of one environment's cube map on the GPU, in bytes.
inline constexpr uint64_t kEnvironmentBytes = [] {
    uint64_t texels = 0;
    for (uint32_t level = 0; level < kEnvironmentLevels; ++level) {
        const uint64_t side = kEnvironmentFaceSize >> level;
        texels += side * side * 6;
    }
    // Four half floats a texel.
    return texels * 8;
}();

/// An environment as the shaders use it.
struct GpuEnvironment {
    /// The light arriving from each direction, as a cube map; level 0 as sharp as it
    /// gets, the last level as a perfectly rough surface reflects it.
    nvrhi::TextureHandle radiance;
    /// The image itself, for where the sky is seen.
    nvrhi::TextureHandle sky;
    /// What the environment lights a matte surface with
    /// (SphericalHarmonics9::diffuseCoefficients()).
    std::array<Vec3, kSphericalHarmonicsCount> diffuse{};
    /// The levels `radiance` has.
    uint32_t levelCount = 1;
};

class EnvironmentMaps {
public:
    /// Loads the shaders and makes the stand-in for "no environment". The device, the
    /// shader manager and the asset manager must outlive the object.
    [[nodiscard]] static Result<EnvironmentMaps> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                        AssetManager& assets);

    /// The environment that the image asset `id` makes, or null when the id is nil or
    /// the image cannot be used (which is reported once). `resources` must be those
    /// of the same asset manager. The first call for an image makes its cube map with
    /// passes recorded into `commandList`, which must be open, and sends the image to
    /// the GPU: `resources.flushUploads()` must follow before that list is executed.
    /// The pointer is valid until the next call.
    ///
    /// An environment that has not been asked for in the last kKeepUnusedCalls calls
    /// gives its memory back, and is made anew when it is asked for again.
    [[nodiscard]] const GpuEnvironment* environment(nvrhi::ICommandList* commandList, const Uuid& id,
                                                    RenderResources& resources);

    /// What a view without an environment binds: no light from anywhere.
    [[nodiscard]] const GpuEnvironment& none() const { return m_none; }

    /// How many cube maps have been made so far. An environment that is used every
    /// frame is made once.
    [[nodiscard]] uint32_t bakeCount() const { return m_bakeCount; }

    /// How many environments are held, and the memory of their cube maps, in bytes.
    [[nodiscard]] uint32_t count() const;

    [[nodiscard]] uint64_t byteSize() const { return kEnvironmentBytes * count(); }

    /// For how many calls of environment() an environment is kept that none of them
    /// asked for: long enough for the views of a few frames to take turns.
    static constexpr uint64_t kKeepUnusedCalls = 64;

private:
    /// An environment that has been made, and what it was made of.
    struct Entry {
        Uuid id;
        /// The texture of the image when the cube map was made. Another one means the
        /// image has been loaded anew; holding on to this one keeps a new texture from
        /// getting its address.
        nvrhi::TextureHandle source;
        GpuEnvironment environment;
        /// False when the environment could not be made of `source`: reported once,
        /// and not tried again until the image changes.
        bool usable = false;
        /// The call of environment() that last asked for it.
        uint64_t lastUsed = 0;
    };

    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        AssetManager* assets;
        nvrhi::BufferHandle constants;
        nvrhi::SamplerHandle sampler;
        nvrhi::BindingLayoutHandle layout;
        FullscreenPass prefilter;
        GpuEnvironment none;
    };

    explicit EnvironmentMaps(Parts parts);

    /// Makes the environment of the image `id`, whose texture is `source`. False, with
    /// the reason in the log, when it cannot be made.
    [[nodiscard]] bool make(nvrhi::ICommandList* commandList, const Uuid& id, nvrhi::ITexture* source,
                            GpuEnvironment& made);

    /// Draws every face of every level of `cube` from `source`. False when the GPU
    /// objects that takes cannot be created.
    [[nodiscard]] bool bake(nvrhi::ICommandList* commandList, nvrhi::ITexture* source, nvrhi::ITexture* cube);

    nvrhi::IDevice* m_device;
    AssetManager* m_assets;
    nvrhi::BufferHandle m_constants;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BindingLayoutHandle m_layout;
    FullscreenPass m_prefilter;
    GpuEnvironment m_none;
    std::vector<Entry> m_entries;
    uint64_t m_calls = 0;
    uint32_t m_bakeCount = 0;
};

} // namespace cordite
