// Stage 6 scene: everything the engine can do, in one frame.
//
// Procedural terrain, a forest scattered by Poisson disk across three
// species, a ruin built from modelling primitives, a character, water,
// depth of field, denoising and a post stack.
//
//   scene_diorama          full quality
//   scene_diorama draft    small and fast, for iterating on the lighting
//   scene_diorama raw      also write the un-denoised frame, for comparison
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace blocky;

namespace {

constexpr int      kExtent = 54;      // half-width of the ground
constexpr int      kWaterLevel = 6;
constexpr uint32_t kSeed = 60606u;

// A hill with a hollow on one side that fills with water.
int groundHeight(int x, int z) {
    float fx = float(x), fz = float(z);

    float base = noise::fbm2(fx * 0.021f, fz * 0.021f, 5, kSeed) * 0.5f + 0.5f;
    float ridge = noise::ridged2(fx * 0.05f, fz * 0.05f, 3, kSeed + 31u);

    // The hill, centred a little off-origin so the composition is not
    // perfectly symmetric.
    float dx = fx + 14.0f, dz = fz + 6.0f;
    float distance = std::sqrt(dx * dx + dz * dz) / 40.0f;
    float hill = saturate(1.0f - distance * distance);
    hill = hill * hill;

    // A basin to the south-east, dug below the waterline.
    float bx = fx - 22.0f, bz = fz - 20.0f;
    float basin = saturate(1.0f - std::sqrt(bx * bx + bz * bz) / 22.0f);

    float height = base * 7.0f + hill * 22.0f + ridge * 5.0f * hill;
    height -= basin * basin * 13.0f;

    return int(height) + 4;
}

void generateTerrain(World& world) {
    for (int z = -kExtent; z <= kExtent; ++z) {
        for (int x = -kExtent; x <= kExtent; ++x) {
            int height = groundHeight(x, z);

            for (int y = 0; y <= height; ++y) {
                BlockId id;
                if (y == height) {
                    id = height <= kWaterLevel + 1 ? palette::Sand : palette::GrassBlock;
                } else if (y > height - 4) {
                    id = height <= kWaterLevel + 1 ? palette::Sand : palette::Dirt;
                } else {
                    id = palette::Stone;
                }
                world.set({x, y, z}, id);
            }
            for (int y = height + 1; y <= kWaterLevel; ++y) {
                world.set({x, y, z}, palette::Water);
            }
        }
    }
}

// A broken tower, built entirely from the shape primitives and then eroded
// so it reads as a ruin rather than as a cylinder.
void buildRuin(World& world, IVec3 base) {
    Vec3 foot{float(base.x) + 0.5f, float(base.y), float(base.z) + 0.5f};

    shape::cylinder(world, foot - Vec3{0.0f, 3.0f, 0.0f}, {0, 1, 0}, 7.0f, 4.0f, palette::Cobblestone);
    shape::cylinder(world, foot, {0, 1, 0}, 6.0f, 17.0f, palette::Cobblestone);
    shape::cylinder(world, foot + Vec3{0.0f, 1.0f, 0.0f}, {0, 1, 0}, 4.6f, 17.0f, palette::Air);

    // Window slits, cut with boxes.
    for (int i = 0; i < 4; ++i) {
        float angle = float(i) * kPi * 0.5f + 0.4f;
        Vec3 direction{std::cos(angle), 0.0f, std::sin(angle)};
        for (int level = 0; level < 3; ++level) {
            Vec3 centre = foot + direction * 6.0f + Vec3{0.0f, 4.0f + float(level) * 5.0f, 0.0f};
            shape::ellipsoid(world, centre, Vec3{2.2f, 1.6f, 2.2f}, palette::Air);
        }
    }

    // A ring of brick partway up, and a collapsed top.
    shape::torus(world, foot + Vec3{0.0f, 12.0f, 0.0f}, {0, 1, 0}, 6.2f, 0.9f, palette::Bricks);
    shape::ellipsoid(world, foot + Vec3{2.0f, 18.0f, -1.0f}, Vec3{7.0f, 5.0f, 7.0f}, palette::Air);

    // Erosion knocks the sharp edges off the break.
    shape::erode(world, base + IVec3{-9, 12, -9}, base + IVec3{9, 22, 9}, 3, 1);

    // A lantern inside, so light spills out of the slits.
    world.set(base + IVec3{0, 6, 0}, palette::Glowstone);
    world.set(base + IVec3{0, 13, 0}, palette::Glowstone);

    // A short path of gravel leading away from the door.
    shape::line(world, foot + Vec3{0.0f, 0.4f, 6.0f}, foot + Vec3{6.0f, -2.0f, 20.0f}, 1.9f,
                palette::Gravel);
}

void plantForest(World& world) {
    std::vector<Vec2> spots =
        poissonDisk({float(-kExtent) + 2.0f, float(-kExtent) + 2.0f},
                    {float(kExtent) - 2.0f, float(kExtent) - 2.0f}, 5.4f, kSeed + 7u);

    const TreeParams species[] = {tree::oak(palette::OakLog, palette::OakLeaves), tree::birch(palette::BirchLog, palette::BirchLeaves), tree::spruce(palette::SpruceLog, palette::SpruceLeaves)};
    int planted = 0;

    for (size_t i = 0; i < spots.size(); ++i) {
        int x = int(spots[i].x);
        int z = int(spots[i].y);

        shape::SurfacePoint surface = shape::findSurface(world, x, z, 60);
        if (!surface.found || surface.id != palette::GrassBlock) continue;
        if (surface.block.y <= kWaterLevel + 1) continue;

        // Nothing grows on a cliff edge.
        if (shape::surfaceRoughness(world, x, z, 60, 1) > 2) continue;

        // Keep the clearing around the ruin free.
        float dx = float(x) + 14.0f, dz = float(z) + 6.0f;
        if (dx * dx + dz * dz < 190.0f) continue;

        // Species by altitude: spruce high up, birch in the middle, oak low.
        int pick = surface.block.y > 22 ? 2 : (surface.block.y > 15 ? 1 : 0);
        // A little mixing, so the bands are not stripes.
        if (noise::hashToFloat(x, 0, z, kSeed + 55u) < 0.25f) pick = (pick + 1) % 3;

        growTree(world, surface.block, species[pick], kSeed + uint32_t(i) * 101u);
        ++planted;
    }
    std::printf("  %d trees from %zu candidate spots\n", planted, spots.size());
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false, alsoRaw = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        if (std::strcmp(argv[i], "raw") == 0) alsoRaw = true;
    }

    Scene scene(palette::registry());
    std::string error;

    std::printf("generating...\n");
    generateTerrain(scene.world);

    shape::SurfacePoint ruinGround = shape::findSurface(scene.world, -14, -6, 60);
    IVec3 ruinBase = ruinGround.found ? ruinGround.block : IVec3{-14, 10, -6};
    buildRuin(scene.world, ruinBase);
    plantForest(scene.world);

    std::printf("  %llu blocks in %llu chunks\n",
                (unsigned long long)scene.world.blockCount(),
                (unsigned long long)scene.world.chunkCount());

    // ---- assets
    AssetSource source;
    BlockTextureLibrary blockTextures;
    std::vector<Skin> skins(1);
    std::vector<EntityModel> models(1);
    EntitySet entities;

    std::string assetPath = AssetSource::findClientJar();
    if (!assetPath.empty() && source.open(assetPath, &error)) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), &error)) {
            scene.blockTextures = &blockTextures;
            std::printf("  textures: %zu images\n", blockTextures.textureCount());
        }
        if (skins[0].loadFromSource(source, "assets/minecraft/textures/entity/player/slim/alex.png",
                                    &error)) {
            models[0] = buildPlayerModel(skins[0]);

            shape::SurfacePoint stand = shape::findSurface(scene.world, -2, 8, 60);
            Entity hero;
            hero.model = &models[0];
            hero.skin = &skins[0];
            hero.position = {-1.5f, float(stand.found ? stand.block.y + 1 : 12), 8.5f};
            hero.yawDegrees = 205.0f;
            hero.pose = Pose::striding(14.0f);
            entities.add(hero);
            scene.entities = &entities;
        }
    } else {
        std::printf("  no game assets, rendering with flat colours\n");
    }

    // ---- lighting: low sun raking across the hill
    scene.sun.direction = normalize(Vec3{0.62f, 0.42f, -0.34f});
    scene.sun.color = Vec3{1.0f, 0.86f, 0.66f};
    scene.sun.intensity = 12.0f;
    scene.sun.angularRadiusDegrees = 1.3f;

    scene.sky.zenith  = Vec3{0.10f, 0.22f, 0.52f};
    scene.sky.horizon = Vec3{0.56f, 0.58f, 0.66f};
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 0.85f;
    scene.overrideBackground = false;

    // ---- camera: low, close, with the far hill thrown out of focus
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(34.0f);
    scene.camera.lookAt({50.0f, 39.0f, 56.0f}, {-12.0f, 31.0f, -2.0f});
    scene.camera.aperture = draft ? 0.0f : 0.5f;
    scene.camera.focusOn({-12.0f, 34.0f, -4.0f});

    PathSettings settings;
    settings.width  = draft ? 800 : 1600;
    settings.height = draft ? 450 : 900;
    // Far fewer samples than stage 2 needed, because the denoiser finishes
    // the job that brute force used to have to do alone.
    settings.samplesPerPixel = draft ? 16 : 64;
    settings.maxBounces = draft ? 4 : 7;

    std::printf("rendering %dx%d at %d spp...\n",
                settings.width, settings.height, settings.samplesPerPixel);

    RenderStats stats;
    RenderTargets targets;
    Image raw = renderPath(scene, settings, &stats, &targets);

    std::printf("  %.1f s on %d threads, %.1f M rays\n",
                stats.seconds, stats.threadsUsed, double(stats.totalRays) / 1e6);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    if (alsoRaw) {
        pngSave(draft ? "out/diorama_raw_draft.png" : "out/diorama_raw.png", raw, tone, &error);
        std::printf("  wrote the un-denoised frame for comparison\n");
    }

    // ---- post, in the order that matters
    Image frame = denoise(targets, {});

    BloomSettings bloom;
    bloom.threshold = 1.4f;
    bloom.intensity = 0.055f;
    applyBloom(frame, bloom);

    GradeSettings grade;
    grade.contrast = 1.06f;
    grade.saturation = 1.08f;
    grade.temperature = 0.12f;
    applyGrade(frame, grade);

    VignetteSettings vignette;
    vignette.amount = 0.28f;
    applyVignette(frame, vignette);

    applyGrain(frame, 0.012f);

    const char* path = draft ? "out/diorama_draft.png" : "out/diorama.png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
