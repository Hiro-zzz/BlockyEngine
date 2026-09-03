// Stage 5 scene: the interactive viewport.
//
//   scene_viewport                  fly around the island
//   scene_viewport cast             fly around the character set
//   scene_viewport snapshot <png>   render one frame and exit
//   scene_viewport snapshot <png> trace   ... and path trace the same camera,
//                                   which is how the preview gets checked
//                                   against what it is meant to predict
//
// The point of flying around is to find a shot. Press F and the viewport
// prints the camera as C++ you can paste straight into a path-traced scene.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/render/gl/viewport.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/island.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace blocky;

namespace {

void buildStage(World& world) {
    world.fillBox({-10, -1, -10}, {10, -1, 10}, palette::Stone);
    world.fillBox({-10, -1, -2}, {10, -1, 2}, palette::OakPlanks);
    world.fillBox({-10, 0, 6}, {10, 6, 6}, palette::Cobblestone);
    world.fillBox({-3, 1, 6}, {3, 3, 6}, palette::Bricks);

    for (int side = -1; side <= 1; side += 2) {
        int x = side * 7;
        world.fillBox({x, 0, 3}, {x, 3, 3}, palette::Cobblestone);
        world.set({x, 4, 3}, palette::Glowstone);
    }
    world.fillBox({-5, 0, -4}, {-4, 0, -3}, palette::GoldBlock);
    world.fillBox({4, 0, -4}, {5, 0, -3}, palette::IronBlock);
}

const char* kCast[] = {
    "assets/minecraft/textures/entity/player/wide/steve.png",
    "assets/minecraft/textures/entity/player/slim/alex.png",
    "assets/minecraft/textures/entity/player/wide/zuri.png",
};

} // namespace

int main(int argc, char** argv) {
    bool castScene = false;
    bool alsoTrace = false;
    std::string snapshotPath;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "cast") == 0) {
            castScene = true;
        } else if (std::strcmp(argv[i], "trace") == 0) {
            alsoTrace = true;
        } else if (std::strcmp(argv[i], "snapshot") == 0 && i + 1 < argc) {
            snapshotPath = argv[++i];
        }
    }

    Scene scene(palette::registry());

    // ---- game assets, optional as always
    AssetSource source;
    BlockTextureLibrary blockTextures;
    std::string error;

    std::string assetPath = AssetSource::findClientJar();
    bool haveAssets = !assetPath.empty() && source.open(assetPath, &error);
    if (haveAssets && blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), &error)) {
        scene.blockTextures = &blockTextures;
        std::printf("textures: %zu images for %zu blocks\n",
                    blockTextures.textureCount(), blockTextures.texturedBlockCount());
    } else {
        std::printf("textures: none, showing flat palette colours\n");
    }

    // These must outlive the entity set, which only stores pointers.
    std::vector<Skin> skins;
    std::vector<EntityModel> models;
    EntitySet entities;

    if (castScene) {
        buildStage(scene.world);

        if (haveAssets) {
            size_t count = sizeof(kCast) / sizeof(kCast[0]);
            skins.resize(count);
            models.resize(count);

            const Pose poses[] = {Pose::standing(), Pose::waving(), Pose::tPose()};
            for (size_t i = 0; i < count; ++i) {
                if (!skins[i].loadFromSource(source, kCast[i], &error)) continue;
                models[i] = buildPlayerModel(skins[i]);

                Entity entity;
                entity.model = &models[i];
                entity.skin = &skins[i];
                entity.position = {float(i) * 2.2f - 2.2f, 0.0f, 0.0f};
                entity.yawDegrees = float(i) * 12.0f - 12.0f;
                entity.pose = poses[i];
                entities.add(entity);
            }
            scene.entities = &entities;
        }

        scene.camera.lookAt({0.3f, 1.72f, -7.8f}, {0.0f, 1.05f, 0.4f});
        scene.camera.fovY = radians(42.0f);
        scene.sun.direction = normalize(Vec3{-0.42f, 0.58f, -0.70f});
        scene.sun.intensity = 11.0f;
        scene.sky.intensity = 0.9f;
        scene.ambientStrength = 1.0f;
        scene.overrideBackground = true;
        scene.background = srgbToLinear(Vec3{0.42f, 0.52f, 0.66f});
    } else {
        island::build(scene.world);

        scene.camera.lookAt({52.0f, 34.0f, 52.0f}, {0.0f, 12.0f, 0.0f});
        scene.camera.fovY = radians(50.0f);
        scene.sun.direction = normalize(Vec3{-0.52f, 0.66f, 0.36f});
        scene.sun.color = Vec3{1.0f, 0.90f, 0.74f};
        scene.sun.intensity = 13.0f;
        scene.sky.zenith  = Vec3{0.14f, 0.28f, 0.60f};
        scene.sky.horizon = Vec3{0.68f, 0.76f, 0.90f};
        scene.sky.intensity = 1.05f;
        scene.ambientStrength = 0.9f;
    }

    std::printf("world: %llu blocks in %llu chunks\n",
                (unsigned long long)scene.world.blockCount(),
                (unsigned long long)scene.world.chunkCount());

    ViewportSettings settings;
    settings.title = castScene ? "BlockyEngine viewport - cast" : "BlockyEngine viewport - island";
    settings.snapshotPath = snapshotPath;
    settings.moveSpeed = castScene ? 4.0f : 18.0f;
    if (!snapshotPath.empty()) {
        settings.width = 1280;
        settings.height = 720;
    }

    int result = runViewport(scene, settings);
    if (result != 0 || !alsoTrace || snapshotPath.empty()) return result;

    // The same scene and the same camera, through the path tracer. Comparing
    // the two is the only way to tell whether the preview is actually
    // previewing anything.
    scene.camera.aspect = float(settings.width) / float(settings.height);

    PathSettings trace;
    trace.width = settings.width;
    trace.height = settings.height;
    trace.samplesPerPixel = 96;
    trace.maxBounces = 6;

    std::printf("path tracing the same camera at %d spp...%s", trace.samplesPerPixel, "\n");
    RenderStats stats;
    Image frame = renderPath(scene, trace, &stats);
    std::printf("  %.1f s%s", stats.seconds, "\n");

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    std::string tracedPath = snapshotPath;
    size_t dot = tracedPath.rfind('.');
    tracedPath = (dot == std::string::npos ? tracedPath : tracedPath.substr(0, dot)) + "_traced.png";

    std::string saveError;
    if (!pngSave(tracedPath, frame, tone, &saveError)) {
        std::printf("save failed: %s%s", saveError.c_str(), "\n");
        return 1;
    }
    std::printf("wrote %s%s", tracedPath.c_str(), "\n");
    return 0;
}
