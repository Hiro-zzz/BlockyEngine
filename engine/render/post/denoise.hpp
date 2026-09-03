#pragma once
// Edge-avoiding a-trous wavelet denoiser (Dammertz et al. 2010).
//
// Path tracing converges as 1/sqrt(N), so buying a clean image with samples
// alone is brutally expensive. This buys most of it back: the filter smooths
// the *lighting* while the surface normals, depths and albedos tell it where
// the real edges are, so it does not smear across them.
//
// The key move is demodulation. Dividing the render by the albedo leaves just
// the illumination, which is smooth and safe to blur; multiplying the texture
// back afterwards means the filter never had a chance to erase it.
#include "engine/core/image.hpp"
#include "engine/render/trace/pathtrace.hpp"

namespace blocky {

struct DenoiseSettings {
    // Each iteration doubles the filter's reach: five covers a 5x5 kernel
    // spread over 80 pixels without ever touching more than 25 taps.
    int iterations = 5;

    // Edge tolerances. Smaller keeps more detail and more noise.
    float colorSigma  = 6.0f;
    float normalSigma = 0.25f;
    // In blocks. Kept deliberately tight: two terraces of the same grass with
    // the same upward normal differ only in depth, and a loose tolerance
    // blurs them into each other.
    float depthSigma  = 0.6f;
    float albedoSigma = 0.14f;

    // How much of the filtered result to keep, 0..1. Below 1 leaves a little
    // grain, which often reads better than a perfectly flat image.
    float strength = 1.0f;
};

// Returns a denoised copy of `targets.color`. If the auxiliary buffers are
// missing the input is returned unchanged rather than blurred blindly.
Image denoise(const RenderTargets& targets, const DenoiseSettings& settings = {});

} // namespace blocky
