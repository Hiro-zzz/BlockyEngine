// Voxel Cornell box -- the standard way to prove a path tracer is actually
// transporting light rather than faking it.
//
// The only light source is a panel in the ceiling. If indirect illumination
// works, three things must appear: the underside of the boxes is lit at all,
// the left of the white geometry picks up red and the right picks up green,
// and the shadows have soft penumbrae because the emitter has area.
#include "engine/core/png.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>

using namespace blocky;

namespace {

// Pure matte surfaces, so nothing but transport affects the result.
BlockDef matte(const char* name, Vec3 srgbColor) {
    BlockDef def;
    def.name = name;
    def.albedo = srgbToLinear(srgbColor);
    def.roughness = 1.0f;
    return def;
}

} // namespace

int main() {
    // The scene owns its palette rather than borrowing the global one.
    BlockRegistry registry;
    BlockId white = registry.add(matte("cornell_white", {0.73f, 0.73f, 0.73f}));
    BlockId red   = registry.add(matte("cornell_red",   {0.65f, 0.06f, 0.05f}));
    BlockId green = registry.add(matte("cornell_green", {0.13f, 0.45f, 0.09f}));

    BlockDef lampDef = matte("cornell_lamp", {1.0f, 1.0f, 1.0f});
    lampDef.emission = Vec3{22.0f, 17.6f, 12.8f};
    BlockId lamp = registry.add(lampDef);

    Scene scene(registry);
    World& world = scene.world;

    constexpr int kSize = 40;  // interior spans 0..39 on every axis

    world.fillBox({-1, -1, -1}, {kSize, -1, kSize}, white);          // floor
    world.fillBox({-1, kSize, -1}, {kSize, kSize, kSize}, white);    // ceiling
    world.fillBox({-1, -1, -1}, {kSize, kSize, -1}, white);          // back wall
    world.fillBox({-1, -1, -1}, {-1, kSize, kSize}, red);            // left wall
    world.fillBox({kSize, -1, -1}, {kSize, kSize, kSize}, green);    // right wall
    // The front, at z = kSize, is left open for the camera.

    // Emissive panel set into the ceiling.
    world.fillBox({13, kSize, 13}, {26, kSize, 26}, lamp);

    // Two blocks, the taller one set back and to the left.
    world.fillBox({6, 0, 6}, {16, 23, 16}, white);
    world.fillBox({22, 0, 21}, {32, 11, 31}, white);

    // The box is the entire light transport problem: no sun, no sky, and a
    // black background so anything visible had to bounce.
    scene.sun.intensity = 0.0f;
    scene.sky.intensity = 0.0f;
    scene.overrideBackground = true;
    scene.background = Vec3{0.0f};

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(39.0f);
    scene.camera.lookAt({19.5f, 19.5f, 118.0f}, {19.5f, 19.0f, 19.5f});

    PathSettings settings;
    settings.width = 900;
    settings.height = 900;
    settings.samplesPerPixel = 400;
    settings.maxBounces = 12;

    std::printf("cornell box: %d blocks, rendering %dx%d at %d spp\n",
                int(world.blockCount()), settings.width, settings.height, settings.samplesPerPixel);

    RenderStats stats;
    Image frame = renderPath(scene, settings, &stats);

    std::printf("  %.2f s on %d threads, %.1f M rays total (%.2f Mrays/s)\n",
                stats.seconds, stats.threadsUsed, double(stats.totalRays) / 1e6,
                double(stats.totalRays) / 1e6 / std::max(stats.seconds, 1e-9));

    ToneParams tone;
    tone.curve = Tonemap::Reinhard;  // faithful to the values, unlike a filmic curve
    tone.exposure = 1.0f;

    std::string error;
    if (!pngSave("out/cornell.png", frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote out/cornell.png\n");
    return 0;
}
