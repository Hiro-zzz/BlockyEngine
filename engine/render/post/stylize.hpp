#pragma once
// Stylised looks that are applied to a finished frame.
//
// This is the second of the two seams a render style can use. It sits after
// light transport, so it can only rearrange light that is already there --
// which is exactly right for banding and for pixel grids, and exactly wrong
// for anything a surface has to *be*. A highlight cannot be filtered into
// existence; that belongs to scene/material_style.hpp.
//
// Both effects below run in linear light, before the tonemap, for the same
// reason bloom does: quantising or averaging values that have already been
// through a curve bands the curve rather than the light.
#include "engine/core/image.hpp"
#include "engine/render/trace/pathtrace.hpp"

namespace blocky {

// ------------------------------------------------------------ cel shading
// Posterised lighting with an ink outline.
//
// The move that makes this work is the denoiser's: divide the frame by the
// albedo to get illumination on its own, quantise *that*, then multiply the
// texture back. Quantising the colour directly would step the texture as well
// as the light, and every block face would break into contour bands.
//
// The outline is found in the depth and normal buffers rather than in the
// colour, so it traces geometry and ignores texture -- an edge between two
// stone blocks at different depths gets a line, the mortar drawn on one of
// them does not.
struct CelSettings {
    // Quantisation steps of the illumination. Two reads as hard cartoon, five
    // or six as a soft comic. Zero or one switches banding off and leaves the
    // outline, which is a useful look on its own.
    //
    // The steps run from black to full: with two bands a surface is either
    // unlit or fully lit, and nothing in between survives.
    int bands = 4;

    // The illumination that maps to the top band; everything at or above it
    // is fully lit.
    //
    // This is not a nicety. Banding runs over 0..range, so on a scene lit
    // brighter than that every surface lands in the top step and the frame
    // comes out flat white with an outline round it -- which looks like the
    // cel pass is broken and is really the pass being told the wrong window.
    // A key of `sun.intensity` around four wants something near two here.
    float range = 1.0f;

    // What the lowest band maps to, as a fraction of `range`. Zero sends
    // unlit surfaces to true black, which is right for a hard graphic look and
    // wrong for a character: the shadow side of a face becomes a silhouette
    // and the drawing inside it is gone. Around 0.4 is the usual ambient
    // shade of hand-drawn cel work.
    float shadowFloor = 0.0f;

    // Where the steps fall. Below 1 the bands crowd towards the dark end,
    // which is usually what a lit scene wants -- most of the interesting
    // detail lives in the shadows, not in the blown-out highlights.
    float bandGamma = 0.72f;

    // Illumination above this is left alone rather than banded, so a lamp or
    // a patch of sunlit water still blooms instead of flattening to the top
    // step. 0 disables the exemption.
    float highlightCeiling = 3.0f;

    // Outline thresholds. Depth is in blocks and is compared against a
    // gradient prediction, so a floor seen at a glancing angle does not
    // outline itself -- the same correction the denoiser needs, and for the
    // same reason.
    float depthThreshold = 0.35f;
    float normalThreshold = 0.35f;   // on 1 - dot(n0, n1)
    int   outlineWidth = 1;          // in pixels
    Vec3  outlineColor{0.0f, 0.0f, 0.0f};
    float outlineOpacity = 0.85f;

    // Applied to the banded illumination, not to the albedo, so the texture
    // keeps its own colour.
    float saturation = 1.0f;
};

// Needs the auxiliary buffers. Without them there is no albedo to demodulate
// by and no geometry to outline, so the input is returned unchanged rather
// than posterised blindly -- the same contract denoise() keeps.
Image celShade(const RenderTargets& targets, const CelSettings& settings = {});

// ---------------------------------------------------------------- pixelate
// Crush the frame onto a coarse grid, and optionally onto a coarse palette.
//
// Averaging each cell rather than point-sampling it is deliberate: a point
// sample of a path-traced frame carries that pixel's noise into the whole
// cell, and the result crawls with fireflies at exactly the scale the eye is
// now looking at. The average is free anti-aliasing for the block grid.
struct PixelateSettings {
    // Side of one output block, in pixels. The frame keeps its resolution;
    // it just stops carrying more detail than the grid can hold.
    int factor = 4;

    // Quantisation steps per channel after the crush, 0 to leave colour
    // alone. Applied to the tonemapped value rather than the linear one, so
    // the steps are spaced the way the eye sees them rather than the way the
    // renderer stores them.
    int levels = 0;
};
void pixelate(Image& image, const PixelateSettings& settings = {});

} // namespace blocky
