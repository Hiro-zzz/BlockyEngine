// A night in the woods: one figure with their back against a log, a fire, and
// a sword laid down beside them.
//
// Three notes on how it is put together.
//
// The fire is a *prop*, not blocks. A world cell holds one block and renders
// as a full cube, so a campfire built out of blocks is four metre-wide logs.
// As a voxel model at a sixteenth of a block it is the size the game draws it,
// and it takes its colours from the game's own campfire textures.
//
// The light is still blocks. A prop glows but does not illuminate -- LightSet
// gathers emissive faces from the voxel world and nothing else -- so a bed of
// emissive blocks sits under the prop doing the lighting. That is the
// documented way round it, and it is why the shadows here are soft: they come
// from area sources, not from a shadow setting.
//
// The pose is what the rig was for. Sitting needs hips and knees, and knees
// are not a joint the player model comes with.
//
//   scene_campfire                  full quality
//   scene_campfire draft            small and fast
//   scene_campfire <skin.png>       a different skin
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/prop/item.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/sprite/scatter.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

constexpr uint32_t kSeed = 77123u;
constexpr int   kExtent = 34;
constexpr int   kGroundY = 4;            // top ground block; the surface is at kGroundY + 1
constexpr float kSurface = float(kGroundY) + 1.0f;
constexpr float kClearing = 9.0f;

float distanceToCentre(float x, float z) { return std::sqrt(x * x + z * z); }

int groundHeight(int x, int z) {
    float rough = noise::fbm2(float(x) * 0.055f, float(z) * 0.055f, 3, kSeed);
    float d = distanceToCentre(float(x), float(z)) / kClearing;
    float flat = saturate(1.0f - d);
    flat *= flat;
    return kGroundY + int(rough * 2.2f * (1.0f - flat));
}

void buildGround(World& world) {
    for (int z = -kExtent; z <= kExtent; ++z) {
        for (int x = -kExtent; x <= kExtent; ++x) {
            int height = groundHeight(x, z);
            for (int y = 0; y <= height; ++y) {
                BlockId id = palette::Stone;
                if (y == height) id = palette::GrassBlock;
                else if (y > height - 3) id = palette::Dirt;
                world.set({x, y, z}, id);
            }
        }
    }
}

// The bed of embers that actually lights the clearing, and a ring of stones
// around it. The prop that looks like a campfire sits on top of this.
void buildEmbers(World& world, BlockId embers) {
    world.set({0, kGroundY, 0}, embers);
    for (int z = -3; z <= 3; ++z) {
        for (int x = -3; x <= 3; ++x) {
            if (x == 0 && z == 0) continue;
            float d = distanceToCentre(float(x), float(z));
            if (d < 1.7f) world.set({x, kGroundY, z}, palette::Cobblestone);
        }
    }
}

// Four logs stacked in a square with embers between them, at a sixteenth of a
// block per voxel -- which is the scale the game's own campfire is drawn at.
VoxelModel buildCampfireModel(const Texture& logTexture, const Texture& litTexture) {
    VoxelMaterial wood;
    wood.albedo = logTexture.empty() ? srgbToLinear(Vec3{0.36f, 0.26f, 0.16f})
                                     : logTexture.averageColor();
    wood.roughness = 0.92f;

    VoxelMaterial lit;
    lit.albedo = litTexture.empty() ? srgbToLinear(Vec3{0.85f, 0.42f, 0.12f})
                                    : litTexture.averageColor();
    lit.emission = {4.5f, 1.5f, 0.35f};

    VoxelModel model;
    model.resize({16, 7, 16});
    const uint16_t woodSlot = model.addMaterial(wood);
    const uint16_t litSlot = model.addMaterial(lit);

    // Nothing in the middle, deliberately. The emissive block underneath is
    // what lights the clearing, and a floor of ember voxels standing on it
    // shadowed almost all of that light back into the ground -- the figure
    // three blocks away came out a silhouette.

    // Lower pair, running along X. The cut ends glow.
    for (int z : {2, 11}) {
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y <= 2; ++y) {
                for (int dz = 0; dz <= 2; ++dz) {
                    bool end = x <= 1 || x >= 14;
                    model.set({x, y, z + dz}, end ? litSlot : woodSlot);
                }
            }
        }
    }

    // Upper pair, crossed over them along Z.
    for (int x : {2, 11}) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 3; y <= 5; ++y) {
                for (int dx = 0; dx <= 2; ++dx) {
                    bool end = z <= 1 || z >= 14;
                    model.set({x + dx, y, z}, end ? litSlot : woodSlot);
                }
            }
        }
    }

    model.trim();
    return model;
}

void plantForest(World& world) {
    std::vector<Vec2> spots = poissonDisk({float(-kExtent) + 3.0f, float(-kExtent) + 3.0f},
                                          {float(kExtent) - 3.0f, float(kExtent) - 3.0f}, 5.0f,
                                          kSeed + 11u);

    const TreeParams species[] = {tree::spruce(palette::SpruceLog, palette::SpruceLeaves), tree::spruce(palette::SpruceLog, palette::SpruceLeaves), tree::oak(palette::OakLog, palette::OakLeaves)};

    for (size_t i = 0; i < spots.size(); ++i) {
        int x = int(spots[i].x);
        int z = int(spots[i].y);

        float d = distanceToCentre(float(x), float(z));
        if (d < kClearing) continue;
        if (d < kClearing + 3.0f && noise::hashToFloat(x, 0, z, kSeed) < 0.30f) continue;

        shape::SurfacePoint surface = shape::findSurface(world, x, z, 40);
        if (!surface.found || surface.id != palette::GrassBlock) continue;
        if (shape::surfaceRoughness(world, x, z, 40, 1) > 2) continue;

        int pick = int(noise::hashToFloat(x, 1, z, kSeed + 3u) * 3.0f) % 3;
        growTree(world, surface.block, species[pick], kSeed + uint32_t(i) * 97u);
    }

    std::vector<Vec2> shrubs = poissonDisk({-22.0f, -22.0f}, {22.0f, 22.0f}, 4.6f, kSeed + 41u);
    for (size_t i = 0; i < shrubs.size(); ++i) {
        int x = int(shrubs[i].x);
        int z = int(shrubs[i].y);
        if (distanceToCentre(float(x), float(z)) < kClearing - 1.0f) continue;

        shape::SurfacePoint surface = shape::findSurface(world, x, z, 40);
        if (!surface.found || surface.id != palette::GrassBlock) continue;
        growTree(world, surface.block, tree::bush(palette::OakLog, palette::OakLeaves), kSeed + uint32_t(i) * 53u + 7u);
    }
}

bool readTexture(const AssetSource& source, const char* path, Texture& out) {
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, nullptr)) return false;
    if (!out.loadFromPng(bytes.data(), bytes.size(), nullptr)) return false;
    out.cropToFirstSquareFrame();   // several of these ship as animation strips
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool inspect = false;   // daylight, for checking the pose and the blocking
    std::string skinPath = "assets/skins/charlie.png";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else if (std::strcmp(argv[i], "inspect") == 0) { inspect = true; draft = true; }
        else skinPath = argv[i];
    }

    // A private palette, so the embers can be the colour of a fire that has
    // been burning a while. It has to outlive the world that points at it.
    // A copy of the scenes' palette, plus this scene's own blocks on the
    // end. Copied rather than shared because adding to the shared one would
    // hand every other scene a block it has no texture for.
    BlockRegistry registry = palette::registry();
    BlockDef emberDef;
    emberDef.name = "campfire_embers";
    emberDef.albedo = srgbToLinear(Vec3{0.20f, 0.08f, 0.04f});
    // One block doing all the lighting, so it has to be bright. It is never
    // seen directly -- the campfire prop stands on top of it.
    emberDef.emission = {26.0f, 8.5f, 1.8f};
    const BlockId embers = registry.add(emberDef);

    Scene scene(registry);
    buildGround(scene.world);
    buildEmbers(scene.world, embers);
    plantForest(scene.world);

    // -------------------------------------------------------------- assets
    AssetSource source;
    BlockTextureLibrary blockTextures;
    Texture campfireLogTexture, campfireLitTexture, fireTexture, dustTexture;

    std::string jar = AssetSource::findClientJar();
    const bool haveAssets = !jar.empty() && source.open(jar, nullptr);
    if (haveAssets) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
            scene.blockTextures = &blockTextures;
        }
        readTexture(source, "assets/minecraft/textures/block/campfire_log.png",
                    campfireLogTexture);
        readTexture(source, "assets/minecraft/textures/block/campfire_log_lit.png",
                    campfireLitTexture);
        readTexture(source, "assets/minecraft/textures/block/campfire_fire.png", fireTexture);
        readTexture(source, "assets/minecraft/textures/particle/generic_0.png", dustTexture);
    }
    std::printf("[campfire] assets %s, campfire textures %s\n", haveAssets ? "found" : "missing",
                campfireLogTexture.empty() ? "no" : "yes");

    // ---------------------------------------------------------------- skin
    Skin skin;
    std::string error;
    bool haveSkin = false;
    {
        std::vector<uint8_t> bytes;
        if (readFileBytes(skinPath, bytes, &error)) {
            haveSkin = skin.loadFromPng(bytes.data(), bytes.size(), &error);
        }
    }
    if (!haveSkin) {
        std::printf("[campfire] could not load %s: %s\n", skinPath.c_str(), error.c_str());
        return 1;
    }
    std::printf("[campfire] skin %s (%s)\n", skinPath.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    // ---------------------------------------------------------- the figure
    EntityModel model = buildPlayerModel(skin);
    const int rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    const int leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");
    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");

    // Back against the log, one knee up, the other leg stretched to the fire.
    //
    // Signs: a limb hangs below its pivot, so forward is positive. The torso
    // and head rise above theirs, so leaning *back* is positive for them.
    Pose pose;
    pose[joint::RightLeg].rotationDegrees = {88.0f, 13.0f, 0.0f};
    pose[rightKnee].rotationDegrees = {-88.0f, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees = {72.0f, -10.0f, 0.0f};
    pose[leftKnee].rotationDegrees = {-22.0f, 0.0f, 0.0f};

    pose[joint::Body].rotationDegrees = {19.0f, 4.0f, 0.0f};

    pose[joint::RightArm].rotationDegrees = {52.0f, 0.0f, 10.0f};
    pose[rightElbow].rotationDegrees = {28.0f, 0.0f, 0.0f};
    pose[joint::LeftArm].rotationDegrees = {-31.0f, 0.0f, -23.0f};
    pose[leftElbow].rotationDegrees = {14.0f, 0.0f, 0.0f};

    pose[joint::Head].rotationDegrees = {-11.0f, 12.0f, 0.0f};

    // Feet on the ground puts the hips a shin above it, and the model's origin
    // a hip-height below that.
    const Vec3 seat{-1.45f, kSurface - 0.46f, 2.65f};
    const float sitterYaw = -42.0f;   // turns them onto the fire

    Entity sitter;
    sitter.model = &model;
    sitter.skin = &skin;
    sitter.position = seat;
    sitter.yawDegrees = sitterYaw;   // zero faces -Z; this turns them onto the fire
    sitter.pose = pose;

    EntitySet entities;
    entities.add(sitter);
    scene.entities = &entities;


    // ----------------------------------------------------------- the props
    VoxelModel campfire = buildCampfireModel(campfireLogTexture, campfireLitTexture);
    VoxelModel swordModel;

    // The log they lean on, modelled with the ordinary shape primitives in a
    // scratch world and then lifted off the lattice. As blocks it would be a
    // row of metre cubes beside a figure a metre tall; at a sixteenth of a
    // block per voxel it is a log. Its colours come from the real oak texture.
    VoxelModel backrest;
    {
        World scratch(palette::registry());
        shape::cylinder(scratch, {6.5f, 6.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, 5.4f, 42.0f,
                        palette::OakLog);
        voxelize::CaptureOptions capture;
        capture.textures = scene.blockTextures;
        backrest = voxelize::fromWorld(scratch, {0, 0, 0}, {13, 13, 42}, capture);
    }

    PropSet props;
    {
        const float yaw = radians(sitterYaw);
        const Vec3 facing{-std::sin(yaw), 0.0f, -std::cos(yaw)};

        Prop log;
        log.model = &backrest;
        log.position = Vec3{seat.x, kSurface, seat.z} - facing * 0.66f;
        log.voxelSize = 1.0f / 16.0f;
        log.anchor = Prop::Anchor::BottomCentre;
        log.yawDegrees = sitterYaw + 90.0f;   // square across their back
        props.add(log);

        Prop fire;
        fire.model = &campfire;
        fire.position = {0.5f, kSurface, 0.5f};
        fire.voxelSize = 1.0f / 16.0f;
        fire.anchor = Prop::Anchor::BottomCentre;
        fire.yawDegrees = 12.0f;
        props.add(fire);

        if (haveAssets) {
            item::ItemOptions options;
            options.roughness = 0.20f;
            std::string swordError;
            if (item::loadByName(source, "diamond_sword", swordModel, options, &swordError)) {
                // Laid flat on the grass beside them. A sixteenth of a block
                // per voxel makes it the length the game draws it -- at a
                // tenth it was half again as long as the figure is tall.
                //
                // Pitch, not roll: the extruded sprite stands in the XY plane,
                // so a quarter turn about X is what lays it down.
                Prop sword;
                sword.model = &swordModel;
                sword.position = {-2.20f, kSurface + 0.06f, 1.50f};
                sword.voxelSize = 1.0f / 16.0f;
                sword.anchor = Prop::Anchor::Centre;
                sword.yawDegrees = 62.0f;
                sword.pitchDegrees = 90.0f;
                props.add(sword);
            } else {
                std::printf("[campfire] no sword: %s\n", swordError.c_str());
            }
        }
        props.build();
        scene.props = &props;
    }
    std::printf("[campfire] props: %zu, %llu voxels\n", props.size(),
                (unsigned long long)props.voxelCount());

    // --------------------------------------------------------------- camera
    // Set off to the side of the line between the figure and the fire, so
    // neither hides the other, and near enough that the figure is most of the
    // frame's height.
    // Square to the pair, the log behind their back appeared beside them
    // instead. This sits about thirty degrees off the way they are looking,
    // which puts the log behind them and still leaves the fire in frame.
    const Vec3 eye{3.55f, kSurface + 1.30f, 1.22f};
    const Vec3 target{-0.75f, kSurface + 0.52f, 2.15f};

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(48.0f);
    scene.camera.lookAt(eye, target);
    scene.camera.aperture = draft ? 0.0f : 0.055f;
    scene.camera.focusOn({-1.45f, kSurface + 0.6f, 2.65f});

    // -------------------------------------------------------------- sprites
    SpriteSet sprites;
    {
        scatter::ParticleStyle flame;
        flame.size = {0.20f, 0.26f};
        flame.sizeJitter = 0.45f;
        flame.texture = fireTexture.empty() ? nullptr : &fireTexture;
        flame.tint = {1.0f, 0.66f, 0.26f};
        flame.emission = {5.2f, 1.8f, 0.42f};
        flame.randomYaw = false;
        flame.alphaCutoff = 0.35f;

        std::vector<Sprite> tongues =
            scatter::inBeam({0.5f, kSurface + 0.10f, 0.5f}, {0.0f, 1.0f, 0.0f}, 0.95f, 0.34f,
                            draft ? 26 : 70, kSeed + 5u, flame);
        aimAt(tongues, eye);
        sprites.add(tongues);

        scatter::ParticleStyle spark;
        spark.size = {0.045f, 0.045f};
        spark.sizeJitter = 0.6f;
        spark.texture = dustTexture.empty() ? nullptr : &dustTexture;
        spark.tint = {1.0f, 0.60f, 0.26f};
        spark.emission = {4.0f, 1.4f, 0.30f};
        spark.randomYaw = false;

        std::vector<Sprite> sparks =
            scatter::inBeam({0.5f, kSurface + 0.5f, 0.5f}, {0.12f, 1.0f, 0.05f}, 7.0f, 1.3f,
                            draft ? 120 : 340, kSeed + 6u, spark);
        scatter::removeInsideSolid(sparks, scene.world);
        aimAt(sparks, eye);
        sprites.add(sparks);

        scatter::ParticleStyle haze;
        haze.size = {0.045f, 0.045f};
        haze.sizeJitter = 0.6f;
        haze.texture = nullptr;
        haze.tint = {0.70f, 0.64f, 0.56f};
        haze.randomYaw = false;

        std::vector<Sprite> motes =
            scatter::inBox({-9.0f, kSurface, -9.0f}, {9.0f, kSurface + 6.0f, 9.0f},
                           draft ? 220 : 650, kSeed + 7u, haze);
        scatter::removeInsideSolid(motes, scene.world);
        aimAt(motes, eye);
        sprites.add(motes);
    }
    sprites.build();
    scene.sprites = &sprites;

    // --------------------------------------------------------------- light
    // The fire does the work. The moon exists only so the far trunks are not
    // solid black.
    scene.sun.direction = normalize(Vec3{-0.32f, 0.86f, -0.40f});
    scene.sun.color = {0.62f, 0.72f, 1.0f};
    scene.sun.intensity = 0.70f;
    scene.sun.angularRadiusDegrees = 2.4f;

    scene.sky.zenith = {0.008f, 0.012f, 0.030f};
    scene.sky.horizon = {0.013f, 0.017f, 0.032f};
    scene.sky.ground = {0.008f, 0.008f, 0.010f};
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 0.85f;

    if (inspect) {
        // Not a look, a measuring stick: a night scene hides everything that
        // is wrong with a pose, and guessing at silhouettes in the dark is a
        // slow way to find out the legs are through the floor.
        scene.sun.color = {1.0f, 0.98f, 0.94f};
        scene.sun.intensity = 6.0f;
        scene.sky.zenith = {0.24f, 0.38f, 0.68f};
        scene.sky.horizon = {0.62f, 0.70f, 0.86f};
        scene.sky.ground = {0.20f, 0.19f, 0.18f};
    }

    // -------------------------------------------------------------- render
    PathSettings settings;
    settings.width = draft ? 760 : 1500;
    settings.height = draft ? 460 : 910;
    // One small source means most of the frame is reached only by bounced
    // light, and that needs paths.
    settings.samplesPerPixel = draft ? 40 : 280;
    settings.maxBounces = draft ? 5 : 9;
    settings.clampIndirect = 9.0f;

    RenderStats stats;
    RenderTargets targets;
    Image frame = renderPath(scene, settings, &stats, &targets);
    frame = denoise(targets, {});

    BloomSettings bloom;
    bloom.threshold = 1.15f;
    bloom.intensity = 0.07f;
    bloom.levels = 7;
    applyBloom(frame, bloom);

    GradeSettings grade;
    grade.contrast = 1.09f;
    grade.saturation = 1.05f;
    grade.temperature = 0.12f;
    grade.lift = {0.004f, 0.003f, 0.004f};
    applyGrade(frame, grade);

    VignetteSettings vignette;
    vignette.amount = 0.44f;
    vignette.radius = 0.64f;
    applyVignette(frame, vignette);
    applyGrain(frame, 0.012f);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = draft ? "out/campfire_draft.png" : "out/campfire.png";
    pngSave(path, frame, tone, nullptr);
    std::printf("[campfire] wrote %s\n", path);
    return 0;
}
