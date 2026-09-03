// Stage 4 scene: characters standing in a lit set.
//
// Five entities, each with a different vanilla skin and a different pose,
// two of them on the slim model. They are lit by the same sun, sky and
// glowstone as the blocks around them, cast shadows onto the floor and onto
// each other, and bounce colour off the walls.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace blocky;

namespace {

struct Character {
    const char* skinPath;
    Vec3  position;
    float yawDegrees;
    Pose  pose;
};

void buildSet(World& world) {
    // Floor: polished stone with a strip of planks down the middle.
    world.fillBox({-10, -1, -10}, {10, -1, 10}, palette::Stone);
    world.fillBox({-10, -1, -2}, {10, -1, 2}, palette::OakPlanks);

    // Back wall, to catch shadows and bounce light forwards.
    world.fillBox({-10, 0, 6}, {10, 6, 6}, palette::Cobblestone);
    world.fillBox({-3, 1, 6}, {3, 3, 6}, palette::Bricks);

    // Two side pillars with lamps on top.
    for (int side = -1; side <= 1; side += 2) {
        int x = side * 7;
        world.fillBox({x, 0, 3}, {x, 3, 3}, palette::Cobblestone);
        world.set({x, 4, 3}, palette::Glowstone);
    }

    // A gold block and an iron block on the floor, to show the metal lobes
    // reflecting the characters.
    world.fillBox({-5, 0, -4}, {-4, 0, -3}, palette::GoldBlock);
    world.fillBox({4, 0, -4}, {5, 0, -3}, palette::IronBlock);
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    std::string assetPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else assetPath = argv[i];
    }
    if (assetPath.empty()) assetPath = AssetSource::findClientJar();

    AssetSource source;
    std::string error;
    if (assetPath.empty() || !source.open(assetPath, &error)) {
        std::printf("this scene needs game assets for its skins.\n");
        std::printf("pass a client jar or a resource pack as an argument.\n");
        return 0;
    }
    std::printf("source: %s\n", source.description().c_str());

    Scene scene(palette::registry());
    buildSet(scene.world);

    BlockTextureLibrary blockTextures;
    if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), &error)) {
        scene.blockTextures = &blockTextures;
        std::printf("textures: %zu images for %zu blocks\n",
                    blockTextures.textureCount(), blockTextures.texturedBlockCount());
    }

    // ---- the cast
    const Character cast[] = {
        {"assets/minecraft/textures/entity/player/wide/steve.png", {-3.8f, 0.0f, 0.0f},  14.0f, Pose::standing()},
        {"assets/minecraft/textures/entity/player/slim/alex.png",  {-1.9f, 0.0f, 0.6f},  -6.0f, Pose::waving()},
        {"assets/minecraft/textures/entity/player/wide/zuri.png",  { 0.0f, 0.0f, 0.0f},   0.0f, Pose::tPose()},
        {"assets/minecraft/textures/entity/player/slim/efe.png",   { 1.9f, 0.0f, 0.5f},  10.0f, Pose::striding(32.0f)},
        {"assets/minecraft/textures/entity/player/wide/kai.png",   { 3.8f, 0.0f, 0.0f}, -18.0f, Pose::striding(-18.0f)},
    };

    // The skins and models must outlive the set, which only stores pointers.
    std::vector<Skin> skins(std::size(cast));
    std::vector<EntityModel> models(std::size(cast));
    EntitySet entities;

    for (size_t i = 0; i < std::size(cast); ++i) {
        if (!skins[i].loadFromSource(source, cast[i].skinPath, &error)) {
            std::printf("  skipped %s: %s\n", cast[i].skinPath, error.c_str());
            continue;
        }
        models[i] = buildPlayerModel(skins[i]);

        Entity entity;
        entity.model = &models[i];
        entity.skin = &skins[i];
        entity.position = cast[i].position;
        entity.yawDegrees = cast[i].yawDegrees;
        entity.pose = cast[i].pose;
        entities.add(entity);

        std::printf("  %-52s %s\n", cast[i].skinPath,
                    skins[i].model() == SkinModel::Slim ? "slim" : "classic");
    }
    scene.entities = &entities;
    std::printf("entities: %zu characters, %zu boxes\n", entities.entityCount(), entities.boxCount());

    // ---- lighting: a low warm key from the left, cool sky fill
    scene.sun.direction = normalize(Vec3{-0.42f, 0.58f, -0.70f});
    scene.sun.color = Vec3{1.0f, 0.93f, 0.82f};
    scene.sun.intensity = 11.0f;
    scene.sun.angularRadiusDegrees = 1.1f;

    scene.sky.zenith  = Vec3{0.16f, 0.30f, 0.60f};
    scene.sky.horizon = Vec3{0.66f, 0.74f, 0.88f};
    scene.sky.intensity = 0.9f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.42f, 0.52f, 0.66f});

    // Eye-level three-quarter view: close enough that the faces read.
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(42.0f);
    scene.camera.lookAt({0.3f, 1.72f, -7.8f}, {0.0f, 1.05f, 0.4f});

    PathSettings settings;
    settings.width  = draft ? 800 : 1600;
    settings.height = draft ? 450 : 900;
    settings.samplesPerPixel = draft ? 24 : 300;
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

    const char* path = draft ? "out/characters_draft.png" : "out/characters.png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
