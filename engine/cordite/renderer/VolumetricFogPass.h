#pragma once

/// @file
/// The two passes that work out what lights throw into the fog of a view (VolumetricFog.h),
/// before anything is lit: what the lights throw at the middle of each box of the grid,
/// with their shadows and their cookies, and the sums of that along the grid's columns -
/// which the lighting and the forward pass then read (ViewBindings, t8).
///
/// @code
/// CORDITE_TRY_ASSIGN(VolumetricFogPass fogLight, VolumetricFogPass::create(device, shaders, view.layout()));
///
/// const FogVolumeGrid grid = fogVolumeGridFor(targets.size(), ScreenEffectsQuality::Medium);
/// CORDITE_TRY(targets.prepareFogVolume(device, UVec3(grid.columns, grid.rows, grid.slices)));
/// view.update(commandList, {.camera = cameraView, .targetSize = targets.size(), .fog = fog, .fogVolume = grid},
///             lightClusters, resources.cookies(), shadows, environment, targets.fogVolume());
/// // Once the shadow maps are drawn:
/// fogLight.draw(commandList, targets, view.bindings(), view.constants());
/// lighting.draw(commandList, targets, view.bindings());
/// @endcode
///
/// Both are compute shaders (shaders/passes/FogScattering.slang, FogIntegration.slang).
/// The first binds the view's set; the second the view's constants alone, because the set
/// holds the volume that the second writes. The volumes belong to the targets of the view
/// (SceneTargets::prepareFogVolume): the pass keeps nothing between calls.

#include "cordite/core/Result.h"
#include "cordite/renderer/ComputePass.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

class VolumetricFogPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<VolumetricFogPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                          nvrhi::IBindingLayout* viewLayout);

    /// Works out what the lights throw into the fog of the view that `targets` are drawn
    /// for, into their volumes (SceneTargets::prepareFogVolume(), for the grid the view's
    /// constants were given). `viewBindings` are the view's set (ViewBindings::bindings())
    /// and `viewConstants` its constants alone (ViewBindings::constants()), as the update
    /// for this view left them; the shadow maps must have been drawn. The command list must
    /// be open.
    ///
    /// Returns how many passes it took: two - or none, when the targets have no volumes or
    /// what the passes need cannot be bound, which the log then says once.
    uint32_t draw(nvrhi::ICommandList* commandList, const SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  nvrhi::IBuffer* viewConstants);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BindingLayoutHandle scatteringLayout;
        nvrhi::BindingLayoutHandle constantsLayout;
        nvrhi::BindingLayoutHandle integrationLayout;
        ComputePass scattering;
        ComputePass integration;
    };

    explicit VolumetricFogPass(Parts parts);

    nvrhi::IDevice* m_device;
    /// Of the first pass, beside the view's: the volume it writes (u0, space1).
    nvrhi::BindingLayoutHandle m_scatteringLayout;
    /// Of the second: the view's constants (b0, space0), and the volume it reads and the one
    /// it writes (t0 and u0, space1).
    nvrhi::BindingLayoutHandle m_constantsLayout;
    nvrhi::BindingLayoutHandle m_integrationLayout;
    ComputePass m_scattering;
    ComputePass m_integration;
    /// So that a failure to bind what the passes need is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
