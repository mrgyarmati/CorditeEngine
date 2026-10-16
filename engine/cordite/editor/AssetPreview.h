#pragma once

/// @file
/// Pictures of assets: what the Assets panel shows of the chosen asset, and what
/// `asset.preview` writes into a file.
///
/// A texture is its own picture. A mesh, a material, a model and a prefab are drawn by the
/// scene renderer, in a world of the preview's own: a mesh as it is, a material on a ball,
/// a model and a prefab as they are placed - whole, from in front and above, in the light
/// an editor gives a scene that has none (SceneRenderSettings::defaultLighting), finished
/// as PostProcessSettings are by default. Other assets have no picture.
///
/// @code
/// AssetPreview preview;
/// CORDITE_TRY(preview.show(renderer, device, commandList, assets, components, crateId, 128,
///                          ShadowQuality::Medium));
/// ImGui::Image(UiSystem::textureId(preview.picture()), ImVec2(128.0f, 128.0f));
/// @endcode

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>

namespace cordite {

class AssetPreview {
public:
    /// The sides of a picture that is drawn, in pixels: from a mark to more than a panel
    /// shows.
    static constexpr uint32_t kMinSize = 16;
    static constexpr uint32_t kMaxSize = 2048;

    AssetPreview() = default;
    ~AssetPreview() = default;
    CORDITE_NON_MOVABLE(AssetPreview);

    /// Whether assets of `type` have a picture.
    [[nodiscard]] static bool hasPicture(AssetType type);

    /// Whether assets of `type` are drawn for their picture, rather than being one.
    [[nodiscard]] static bool isDrawn(AssetType type);

    /// Makes `asset` the one pictured. What is drawn is drawn `size` pixels square with
    /// `commandList`, which must be open, and with shadows of the quality that the other
    /// views of `renderer` have: it has one atlas for them all, and makes it anew whenever
    /// its size changes. A texture is not drawn: it is made ready for the GPU, if it was
    /// not, and is the picture.
    ///
    /// Asked again for what it was asked last - at that size, of assets that were not read
    /// again since - it does nothing and answers as it did, so that it can be asked in
    /// every frame. Fails, and leaves no picture, with NotFound for an id that is no asset,
    /// with InvalidArgument for an asset that has no picture or a size outside kMinSize to
    /// kMaxSize, and as loading what is pictured fails.
    [[nodiscard]] Status show(SceneRenderer& renderer, nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                              AssetManager& assets, const ComponentRegistry& components, const Uuid& asset,
                              uint32_t size, ShadowQuality shadows);

    /// What show() made: the texture itself, or the image it drew into. Null before the
    /// first show() and after one that failed. Alive until the next show() at least, which
    /// is what an interface that shows it needs.
    [[nodiscard]] nvrhi::ITexture* picture() const { return m_picture.Get(); }

    /// The asset that was asked for last, whether there is a picture of it or not.
    [[nodiscard]] const Uuid& asset() const { return m_asset; }

    /// How many times a picture was made, or tried: once for every asset, size and reading
    /// of the assets that show() was asked for - however often it was asked.
    [[nodiscard]] uint64_t attempts() const { return m_attempts; }

private:
    [[nodiscard]] Status make(SceneRenderer& renderer, nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                              AssetManager& assets, const ComponentRegistry& components, ShadowQuality shadows);
    /// Fills the world with what pictures `asset`.
    [[nodiscard]] Status compose(AssetManager& assets, const ComponentRegistry& components, const AssetInfo& asset);
    /// Draws the world into the targets, seen whole.
    void draw(SceneRenderer& renderer, nvrhi::ICommandList* commandList, AssetManager& assets, ShadowQuality shadows);

    World m_world;
    SceneTargets m_targets;
    nvrhi::TextureHandle m_picture;
    /// What was asked for last, and how it went.
    Uuid m_asset;
    uint32_t m_size = 0;
    uint64_t m_revision = 0;
    bool m_asked = false;
    std::optional<Error> m_failure;
    uint64_t m_attempts = 0;
};

} // namespace cordite
