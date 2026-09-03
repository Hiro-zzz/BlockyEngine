#pragma once
// Unidirectional path tracer with next-event estimation.
//
// Compared with the direct renderer this adds: indirect bounces (colour
// bleeding, light reaching places the sun never sees), explicit sampling of
// emissive blocks, and refracting media so that water and glass behave like
// dielectrics instead of tinted walls.
#include "engine/core/image.hpp"
#include "engine/render/trace/stats.hpp"
#include "engine/scene/scene.hpp"

#include <cstdint>
#include <vector>

namespace blocky {

struct PathSettings {
    int width  = 1280;
    int height = 720;

    // Path tracing converges as 1/sqrt(N): going from 64 to 256 samples
    // halves the noise, it does not quarter it.
    int samplesPerPixel = 64;

    // Hard ceiling on path length. Russian roulette usually terminates paths
    // well before this.
    int maxBounces = 8;
    int rouletteStartBounce = 4;

    float maxDistance = 4096.0f;

    // Clamp indirect contributions to kill fireflies -- single samples that
    // are thousands of times brighter than their neighbours and would take
    // millions of samples to average out. Slightly darkens caustics. 0 = off.
    float clampIndirect = 12.0f;

    int      threads = 0;  // 0 = one per hardware thread
    uint64_t seed = 1;
    bool     progress = true;
};

// Auxiliary buffers describing the first surface each pixel saw. A denoiser
// needs these to tell a geometric edge from noise, and demodulating the
// lighting by the albedo is what lets it smooth without erasing texture.
struct RenderTargets {
    Image color;
    Image albedo;              // surface colour, before any lighting
    Image normal;              // world-space normal
    std::vector<float> depth;  // distance to the surface, negative on a miss
    int width = 0, height = 0;

    bool valid() const { return width > 0 && height > 0 && !albedo.empty(); }
};

// `aovs`, when given, is filled alongside the returned image.
Image renderPath(const Scene& scene, const PathSettings& settings, RenderStats* stats = nullptr,
                 RenderTargets* aovs = nullptr);

} // namespace blocky
