#pragma once
// Post effects, applied to the linear HDR framebuffer before tonemapping.
//
// Order matters and is not negotiable: bloom belongs in linear light before
// the curve, grading belongs after bloom but still in linear, and the
// vignette goes last. Doing any of it after the tonemap gives the muddy look
// of an effect applied to already-clipped values.
#include "engine/core/image.hpp"

namespace blocky {

// ------------------------------------------------------------------- bloom
// Light spilling out of very bright areas -- glowstone, lava, sun on water.
// Built as a pyramid: bright pass, halve repeatedly, then add the levels back
// up. Wide, cheap, and free of the ringing a single big blur produces.
struct BloomSettings {
    float threshold = 1.2f;   // luminance above which a pixel blooms
    float knee = 0.6f;        // softens the threshold instead of clipping at it
    float intensity = 0.05f;  // how much of the bloom is added back
    int   levels = 6;         // pyramid depth; each level doubles the reach
};
void applyBloom(Image& image, const BloomSettings& settings);

// ----------------------------------------------------------------- grading
struct GradeSettings {
    float exposure = 1.0f;      // multiplier, in stops use exp2
    float contrast = 1.0f;      // pivots around mid grey
    float saturation = 1.0f;
    float temperature = 0.0f;   // -1 cools towards blue, +1 warms towards amber
    Vec3  lift{0.0f, 0.0f, 0.0f};   // added to shadows
    Vec3  gain{1.0f, 1.0f, 1.0f};   // multiplies highlights
};
void applyGrade(Image& image, const GradeSettings& settings);

// ---------------------------------------------------------------- vignette
struct VignetteSettings {
    float amount = 0.3f;    // 0 = off, 1 = corners go black
    float radius = 0.75f;   // where the falloff starts, in half-diagonals
    float softness = 0.45f;
};
void applyVignette(Image& image, const VignetteSettings& settings);

// ------------------------------------------------------------------- grain
// A little noise breaks up banding in smooth gradients like the sky, and
// keeps a render from looking synthetic.
void applyGrain(Image& image, float amount, uint64_t seed = 1);

} // namespace blocky
