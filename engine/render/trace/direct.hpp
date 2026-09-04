#pragma once
// Direct-lighting renderer: one camera ray per sample, one shadow ray towards
// the sun, sky as ambient, plus Minecraft-style corner ambient occlusion.
//
// This is the fast preview path. It has no indirect bounces -- that is what
// the path tracer at stage 2 adds -- but it already resolves geometry,
// shadows and material colour, so it is what tells us the world layer works.
#include "engine/core/image.hpp"
#include "engine/render/trace/stats.hpp"
#include "engine/scene/scene.hpp"

#include <cstdint>

namespace blocky {

struct DirectSettings {
    int width  = 1280;
    int height = 720;

    // Jittered supersampling. 1 gives hard aliased edges; 4 is usually plenty
    // for voxel geometry, which has no curved silhouettes to resolve.
    int samplesPerPixel = 4;

    float maxDistance = 2048.0f;

    // Corner AO, computed from the eight blocks around each face corner and
    // interpolated across the face -- the same trick Minecraft calls smooth
    // lighting. Darkens ambient only; direct sunlight is shadow-rayed.
    bool  ambientOcclusion = true;
    float aoStrength = 1.0f;

    // Give the sun its angular size, so shadow edges soften with distance.
    bool softShadows = true;

    // Drop lighting entirely: each face gets one fixed brightness by its
    // normal, the way the game shades a world with no shader pack. No shadow
    // rays and no sky samples are fired at all, so this is the cheapest thing
    // the engine can draw -- and it is the "no shaders" look in its own right,
    // not merely a degraded preview. `ambientOcclusion` and `softShadows` are
    // ignored while it is on.
    bool flatLighting = false;

    int      threads = 0;  // 0 = one per hardware thread
    uint64_t seed = 1;
};

Image renderDirect(const Scene& scene, const DirectSettings& settings, RenderStats* stats = nullptr);

} // namespace blocky
