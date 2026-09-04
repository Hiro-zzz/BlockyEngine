// Stage 1 scene: the island rendered with the fast direct-lighting path.
// Compare against island_pt.cpp, which renders the same world with the path
// tracer -- the difference is what indirect light buys you.
#include "engine/core/png.hpp"
#include "engine/render/trace/direct.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/island.hpp"

#include <cstdio>

using namespace blocky;

int main() {
    Scene scene(palette::registry());

    std::printf("generating...\n");
    island::build(scene.world);

    std::printf("  %llu blocks in %llu chunks\n",
                (unsigned long long)scene.world.blockCount(),
                (unsigned long long)scene.world.chunkCount());

    // Late afternoon sun, low and warm, for long shadows across the island.
    scene.sun.direction = normalize(Vec3{-0.52f, 0.66f, 0.36f});
    scene.sun.color = Vec3{1.0f, 0.90f, 0.74f};
    scene.sun.intensity = 13.0f;
    scene.sun.angularRadiusDegrees = 1.4f;

    scene.sky.zenith  = Vec3{0.14f, 0.28f, 0.60f};
    scene.sky.horizon = Vec3{0.68f, 0.76f, 0.90f};
    scene.sky.intensity = 1.05f;
    scene.ambientStrength = 0.85f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.55f, 0.68f, 0.82f});

    scene.frameAll(1.05f, 35.0f);

    DirectSettings settings;
    settings.width = 1600;
    settings.height = 1000;
    settings.samplesPerPixel = 16;

    std::printf("rendering %dx%d at %d spp...\n",
                settings.width, settings.height, settings.samplesPerPixel);

    RenderStats stats;
    Image frame = renderDirect(scene, settings, &stats);

    std::printf("  %.2f s on %d threads, %.1f M primary rays (%.2f Mrays/s)\n",
                stats.seconds, stats.threadsUsed, double(stats.primaryRays) / 1e6,
                double(stats.primaryRays) / 1e6 / std::max(stats.seconds, 1e-9));

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    std::string error;
    if (!pngSave("out/first_world.png", frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote out/first_world.png\n");
    return 0;
}
