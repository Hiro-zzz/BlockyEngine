// Stage 3 scene: the island, path traced, with real Minecraft block textures.
//
// Run with no argument to auto-detect an installation, or pass a jar, a zip
// resource pack, or an unpacked pack folder. Without any of those the scene
// still renders, just with flat palette colours.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/island.hpp"

#include <cstdio>
#include <cstring>

using namespace blocky;

int main(int argc, char** argv) {
    bool draft = false;
    std::string assetPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else assetPath = argv[i];
    }
    if (assetPath.empty()) assetPath = AssetSource::findClientJar();

    Scene scene(palette::registry());
    island::build(scene.world);

    // ---- game assets, optional
    AssetSource source;
    BlockTextureLibrary blockTextures;
    std::string error;

    if (!assetPath.empty() && source.open(assetPath, &error)) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), &error)) {
            scene.blockTextures = &blockTextures;
            std::printf("textures: %zu images for %zu blocks, from %s\n",
                        blockTextures.textureCount(), blockTextures.texturedBlockCount(),
                        source.description().c_str());
        } else {
            std::printf("textures: %s -- rendering with flat colours\n", error.c_str());
        }
    } else {
        std::printf("textures: no game assets found -- rendering with flat colours\n");
    }

    std::printf("world: %llu blocks in %llu chunks\n",
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

    std::printf("rendering %dx%d at %d spp...\n",
                settings.width, settings.height, settings.samplesPerPixel);

    RenderStats stats;
    Image frame = renderPath(scene, settings, &stats);

    std::printf("  %.1f s on %d threads, %.1f M rays (%.2f Mrays/s)\n",
                stats.seconds, stats.threadsUsed, double(stats.totalRays) / 1e6,
                double(stats.totalRays) / 1e6 / std::max(stats.seconds, 1e-9));

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = draft ? "out/textured_island_draft.png" : "out/textured_island.png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
