// The grid of boxes laid over a view that what lights throw into its fog is worked out
// for, and where its boxes are. The C++ side, with the same arithmetic, is
// engine/cordite/renderer/VolumetricFog.h: change both.
//
// The grid covers the picture exactly, fogVolumeSize.x columns across and fogVolumeSize.y
// rows down, and reaches from the camera to fogVolumeDistance metres in front of it in
// fogVolumeSize.z slices, thin near the camera and deep far from it: the end of a share u
// of the way through the slices is u * u of that distance in.
//
// Nothing here but the view's constants (common/View.slang): the second pass of the fog's
// light binds those alone.

#pragma once

#include "common/Fog.slang"
#include "common/View.slang"

// How far in front of the camera a share `share` of the way through the slices is.
float fogSliceDepth(float share)
{
    const float clamped = saturate(share);
    return fogVolumeDistance * clamped * clamped;
}

// The share of the way through the slices at which a point `depth` metres in front of the
// camera is: from 0 to 1, where everything beyond the grid is at its end.
float fogSliceShare(float depth)
{
    return sqrt(saturate(depth / max(fogVolumeDistance, 1.0e-6)));
}

// The line of sight through the middle of the boxes of the column at `uv` of the picture:
// where it sets out, which way it goes, and the cosine of the angle between that and the
// way the camera looks, by which a depth is divided to become a distance along it.
struct FogColumn
{
    float3 viewer;
    float3 direction;
    float cosine;
};

FogColumn fogColumnAt(float2 uv)
{
    FogColumn column;
    column.viewer = viewerAt(uv);
    column.direction = viewDirectionAt(uv);
    column.cosine = max(dot(column.direction, cameraForward), 1.0e-4);
    return column;
}

// The middle of a column's boxes in the picture: `cell` counts columns and rows from the
// top left.
float2 fogCellMiddle(uint2 cell)
{
    return (float2(cell) + 0.5) / fogVolumeSize.xy;
}
