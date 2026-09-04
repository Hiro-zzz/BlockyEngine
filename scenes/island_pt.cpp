// Stage 2 scene: the same island as first_world.cpp, path traced.
//
// What changes: the water is now a refracting medium rather than a blue
// surface, so the seabed shows through and goes blue with depth; light
// bounces off the terrain into shadowed areas; and the lantern and lava
// actually illuminate their surroundings.
#include "engine/core/png.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/island.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace blocky;

int main(int argc, char** argv) {
    // A draft switch, because iterating on lighting at full quality is a
    // waste of minutes: `scene_island_pt draft`.
    bool draft = argc > 1 && std::strcmp(argv[1], "draft") == 0;

    Scene scene(palette::registry());

    std::printf("generating...\n");
    island::build(scene.world);
    std::printf("  %llu blocks in %llu chunks\n",
                (unsigned long long)scene.world.blockCount(),
                (unsigned long long)scene.world.chunkCount());

    scene.sun.direction = normalize(Vec3{-0.52f, 0.66f, 0.36f});
    scene.sun.color = Vec3{1.0f, 0.90f, 0.74f};
    scene.sun.intensity = 13.0f;
    scene.sun.angularRadiusDegrees = 1.4f;

    scene.sky.zenith  = Vec3{0.14f, 0.28f, 0.60f};
    scene.sky.horizon = Vec3{0.68f, 0.76f, 0.90f};
    scene.sky.intensity = 1.05f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.55f, 0.68f, 0.82f});

    scene.frameAll(1.05f, 35.0f);

    PathSettings settings;
    settings.width  = draft ? 800 : 1600;
    settings.height = draft ? 500 : 1000;
    settings.samplesPerPixel = draft ? 24 : 220;
    settings.maxBounces = draft ? 4 : 8;

    std::printf("rendering %dx%d at %d spp, %d bounces...\n",
                settings.width, settings.height, settings.samplesPerPixel, settings.maxBounces);

    RenderStats stats;
    Image frame = renderPath(scene, settings, &stats);

    std::printf("  %.1f s on %d threads, %.1f M rays total (%.2f Mrays/s)\n",
                stats.seconds, stats.threadsUsed, double(stats.totalRays) / 1e6,
                double(stats.totalRays) / 1e6 / std::max(stats.seconds, 1e-9));

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = draft ? "out/island_pt_draft.png" : "out/island_pt.png";
    std::string error;
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
