// A miner mid-swing: iron pickaxe raised, diamond in the wall, cel shaded.
//
// The pose is the whole of the shot. A pickaxe is not a block and a swing is
// not a T-pose, so the arm gets an elbow, the pickaxe is parented to the
// forearm through the same joint matrix the tracer uses, and the camera sits
// on the pickaxe side so the weapon, the face and the diamond all share the
// frame. Cel wants a few large steps of light, which is why the roof is
// cracked: one shaft does the key, the rest of the mine lives in fill.
//
//   scene_strike                 full quality
//   scene_strike draft           small and fast
//   scene_strike inspect         brighter, for checking the pose
//   scene_strike plain           the same frame without cel
//   scene_strike view            fly the viewport, F prints the camera
//   scene_strike <png>           a different skin
//   scene_strike out=name        write out/<name>.png
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/attach.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/prop/item.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/render/gl/viewport.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/sprite/scatter.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultSkin = "C:/Users/yueiw/Downloads/d22e8cd1208f2767.png";
constexpr uint32_t kSeed = 190204u;

// Floor top. A block at y = 0 occupies [0, 1), so feet sit at 1.
constexpr float kFloor = 1.0f;

// Facing the vein. A few degrees of yaw toward +X is only enough to put
// the face in three-quarter, not enough to turn the head around.
const Vec3 kFigurePos{0.42f, kFloor, -2.80f};
constexpr float kFigureYaw = -14.0f;

// In the end wall of the chamber (air is hollowed to z = -5, so z = -6 is
// the face they are mining). Chest-to-head height, a little off centre.
const IVec3 kDiamond{0, 2, -6};

bool loadSkinFile(const std::string& path, Skin& skin, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

// ------------------------------------------------------------------ mine
//
// A short chamber rather than a long tunnel: the camera needs width on the
// pickaxe side, and a corridor three blocks across would put the lens in a
// wall. The end face is close enough that the diamond is a target, not a
// speck.
void buildMine(World& world, BlockId diamondBlock, BlockId diamondOre, BlockId coalOre,
               BlockId ironOre, BlockId lamp) {
    const int x0 = -6, x1 = 6;
    const int z0 = -5, z1 = 7;
    const int yCeil = 5;

    // Surrounding stone, then hollow the chamber. The extra shell is what
    // stops a camera ray from escaping into the sky around the roof crack.
    world.fillBox({x0 - 2, -2, z0 - 3}, {x1 + 2, yCeil + 3, z1 + 2}, palette::Stone);
    world.fillBox({x0, 1, z0}, {x1, yCeil - 1, z1}, palette::Air);

    // Floor mix: cobble where people walk, gravel at the edges, a little dirt
    // so it is not a pavement.
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            float h = noise::hashToFloat(x, 0, z, kSeed);
            BlockId id = palette::Cobblestone;
            if (h < 0.18f) id = palette::Gravel;
            else if (h < 0.28f) id = palette::Stone;
            else if (h < 0.34f) id = palette::Dirt;
            world.set({x, 0, z}, id);
            world.set({x, -1, z}, palette::Stone);
        }
    }

    // Ore in the walls. Hash per block, so veins clump a little when two
    // neighbours both pass a low threshold -- which is close enough.
    auto scatterOre = [&](int x, int y, int z) {
        if (world.get({x, y, z}) != palette::Stone) return;
        float h = noise::hashToFloat(x, y, z, kSeed + 9u);
        if (h < 0.045f) world.set({x, y, z}, coalOre);
        else if (h < 0.070f) world.set({x, y, z}, ironOre);
        else if (h < 0.082f) world.set({x, y, z}, diamondOre);
        else if (noise::hashToFloat(x, y, z, kSeed + 11u) < 0.12f) {
            world.set({x, y, z}, palette::Cobblestone);
        }
    };
    for (int y = 1; y <= yCeil; ++y) {
        for (int z = z0 - 1; z <= z1 + 1; ++z) {
            scatterOre(x0 - 1, y, z);
            scatterOre(x1 + 1, y, z);
        }
        for (int x = x0 - 1; x <= x1 + 1; ++x) {
            scatterOre(x, y, z0 - 1);
            scatterOre(x, y, z1 + 1);
        }
    }

    // The vein. A diamond block is the prize; ore around it is what makes it
    // a strike, not a cube glued to a wall.
    world.set(kDiamond, diamondBlock);
    world.set({kDiamond.x + 1, kDiamond.y, kDiamond.z}, diamondOre);
    world.set({kDiamond.x, kDiamond.y - 1, kDiamond.z}, diamondOre);
    world.set({kDiamond.x - 1, kDiamond.y - 1, kDiamond.z}, diamondOre);
    world.set({kDiamond.x + 1, kDiamond.y - 1, kDiamond.z}, diamondOre);
    world.set({kDiamond.x, kDiamond.y + 1, kDiamond.z}, diamondOre);
    world.set({kDiamond.x - 1, kDiamond.y, kDiamond.z}, diamondOre);

    // Wooden supports, every four blocks, plus a beam over the vein so the
    // end face reads as a worked heading rather than a box.
    for (int z = -4; z <= 6; z += 4) {
        world.fillBox({x0, 1, z}, {x0, yCeil - 1, z}, palette::OakLog);
        world.fillBox({x1, 1, z}, {x1, yCeil - 1, z}, palette::OakLog);
        world.fillBox({x0, yCeil - 1, z}, {x1, yCeil - 1, z}, palette::OakLog);
    }
    // Beam over the heading, no posts in the middle: a pillar behind the
    // head read as a second character and hid the vein.
    world.fillBox({-3, 4, z0}, {3, 4, z0}, palette::OakPlanks);

    // Roof crack. The shaft through this is the key light; without it the
    // mine is a cave and cel has nothing to step. Aimed at the heading so
    // the miner and the vein share the same band of light.
    shape::ellipsoid(world, {0.5f, float(yCeil) + 0.5f, -3.0f}, {2.4f, 2.0f, 2.2f},
                     palette::Air);

    // Warm fill from the right wall, hidden in a recess so the lamp is a
    // glow and not a white cube in frame. The lantern prop sits in front.
    world.set({-5, 3, -3}, lamp);
    world.set({-5, 3, -2}, palette::Air);
}

// ------------------------------------------------------------------ pickaxe
//
// Game item if the jar is there; otherwise a T-shaped fallback that still
// reads as a pick from the camera we use.
VoxelModel buildFallbackPickaxe() {
    VoxelMaterial wood;
    wood.albedo = srgbToLinear(Vec3{0.45f, 0.30f, 0.16f});
    wood.roughness = 0.88f;

    VoxelMaterial iron;
    iron.albedo = srgbToLinear(Vec3{0.78f, 0.80f, 0.84f});
    iron.roughness = 0.28f;
    iron.metallic = 1.0f;

    VoxelModel model;
    model.resize({9, 16, 3});
    const uint16_t woodSlot = model.addMaterial(wood);
    const uint16_t ironSlot = model.addMaterial(iron);

    for (int y = 0; y <= 11; ++y) {
        for (int z = 0; z < 3; ++z) model.set({4, y, z}, woodSlot);
    }
    for (int x = 0; x < 9; ++x) {
        for (int y = 12; y <= 15; ++y) {
            for (int z = 0; z < 3; ++z) {
                bool blade = y >= 13 || x <= 1 || x >= 7;
                if (blade) model.set({x, y, z}, ironSlot);
            }
        }
    }
    model.trim();
    return model;
}

VoxelModel buildLantern() {
    VoxelMaterial iron;
    iron.albedo = srgbToLinear(Vec3{0.42f, 0.44f, 0.48f});
    iron.roughness = 0.35f;
    iron.metallic = 1.0f;

    VoxelMaterial glass;
    glass.albedo = srgbToLinear(Vec3{1.0f, 0.86f, 0.55f});
    glass.emission = {10.0f, 6.4f, 2.2f};

    return voxelize::fromLayers(
        {
            {".###.", "#####", "#####", "#####", ".###."},
            {".....", ".#~#.", ".~~~.", ".#~#.", "....."},
            {".....", ".#~#.", ".~~~.", ".#~#.", "....."},
            {".....", ".#~#.", ".~~~.", ".#~#.", "....."},
            {".###.", "#####", "#####", "#####", ".###."},
            {".....", "..#..", ".#.#.", "..#..", "....."},
        },
        {{'#', iron}, {'~', glass}});
}

}  // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool inspect = false;
    bool plain = false;
    bool view = false;
    std::string skinPath;
    std::string outputName = "strike";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "inspect") inspect = draft = true;
        else if (arg == "plain") plain = true;
        else if (arg == "view") view = true;
        else if (arg.rfind("out=", 0) == 0) outputName = arg.substr(4);
        else skinPath = arg;
    }
    if (skinPath.empty()) skinPath = kDefaultSkin;

    // ---------------------------------------------------------- palette
    // A copy of the scenes' palette, plus this scene's own blocks on the
    // end. Copied rather than shared because adding to the shared one would
    // hand every other scene a block it has no texture for.
    BlockRegistry registry = palette::registry();

    BlockDef diamondDef;
    diamondDef.name = "diamond_block";
    diamondDef.albedo = srgbToLinear(Vec3{0.18f, 0.90f, 0.82f});
    diamondDef.roughness = 0.22f;
    const BlockId diamondBlock = registry.add(diamondDef);

    BlockDef diamondOreDef;
    diamondOreDef.name = "diamond_ore";
    diamondOreDef.albedo = srgbToLinear(Vec3{0.55f, 0.72f, 0.70f});
    const BlockId diamondOre = registry.add(diamondOreDef);

    BlockDef coalDef;
    coalDef.name = "coal_ore";
    coalDef.albedo = srgbToLinear(Vec3{0.42f, 0.42f, 0.42f});
    const BlockId coalOre = registry.add(coalDef);

    BlockDef ironDef;
    ironDef.name = "iron_ore";
    ironDef.albedo = srgbToLinear(Vec3{0.72f, 0.64f, 0.56f});
    const BlockId ironOre = registry.add(ironDef);

    BlockDef lampDef;
    lampDef.name = "mine_lamp";
    lampDef.albedo = srgbToLinear(Vec3{1.0f, 0.82f, 0.52f});
    lampDef.emission = {16.0f, 10.0f, 3.4f};
    const BlockId lamp = registry.add(lampDef);

    Scene scene(registry);
    buildMine(scene.world, diamondBlock, diamondOre, coalOre, ironOre, lamp);

    // ----------------------------------------------------------- assets
    AssetSource source;
    BlockTextureLibrary blockTextures;
    Texture dustTexture;

    std::string jar = AssetSource::findClientJar();
    const bool haveAssets = !jar.empty() && source.open(jar, nullptr);
    if (haveAssets) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
            blockTextures.bind(source, diamondBlock, "diamond_block", "diamond_block",
                               "diamond_block");
            blockTextures.bind(source, diamondOre, "diamond_ore", "diamond_ore", "diamond_ore");
            blockTextures.bind(source, coalOre, "coal_ore", "coal_ore", "coal_ore");
            blockTextures.bind(source, ironOre, "iron_ore", "iron_ore", "iron_ore");
            scene.blockTextures = &blockTextures;
        }
        std::vector<uint8_t> bytes;
        if (source.read("assets/minecraft/textures/particle/generic_0.png", bytes, nullptr)) {
            dustTexture.loadFromPng(bytes.data(), bytes.size(), nullptr);
        }
    }
    std::printf("[strike] assets %s\n", haveAssets ? "found" : "missing");

    // ------------------------------------------------------------- skin
    Skin skin;
    std::string error;
    if (!loadSkinFile(skinPath, skin, &error)) {
        std::printf("could not load %s: %s\n", skinPath.c_str(), error.c_str());
        return 1;
    }
    std::printf("[strike] skin %s (%s)\n", skinPath.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel model = buildPlayerModel(skin);
    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    const int rightKnee = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    const int leftKnee = rigging::addHinge(model, joint::LeftLeg, "leftKnee");

    // This skin's eyes are drawn, large, and not the vanilla 2x2 the scanner
    // is exact on. A wrong rebuild paints tiny squares over a face that was
    // already finished -- so we leave the painted ones alone.
    face::EyeRig eyes{};

    // ---- the pose
    //
    // Wind-up of an overhand strike. Signs from conventions.md: a limb hangs
    // *below* its pivot, so +X swings it forward to -Z; the torso rises
    // *above* the waist, so the same sign leans it back. The right arm is
    // thrown up and slightly back, elbow bent so the fist sits above the
    // shoulder, pickaxe continuing the forearm. Weight on the front (left)
    // foot, rear foot trailing.
    Pose pose;
    // Torso leans into the strike. Head follows the body and looks at the
    // diamond -- the previous shot had the camera behind them, so the face
    // never appeared and the head read as being on backwards.
    pose[joint::Body].rotationDegrees = {-8.0f, -6.0f, 4.0f};
    pose[joint::Head].rotationDegrees = {8.0f, 8.0f, -3.0f};

    // Apex of the swing: arm up and *forward* toward the vein, not back
    // over the shoulder. From a camera in front, a back-swing hides both
    // the pickaxe and the face.
    pose[joint::RightArm].rotationDegrees = {86.0f, 4.0f, 24.0f};
    pose[rightElbow].rotationDegrees = {22.0f, 0.0f, 6.0f};

    pose[joint::LeftArm].rotationDegrees = {22.0f, -6.0f, -18.0f};
    pose[leftElbow].rotationDegrees = {28.0f, 0.0f, -6.0f};

    pose[joint::RightLeg].rotationDegrees = {-10.0f, 6.0f, 5.0f};
    pose[rightKnee].rotationDegrees = {-8.0f, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees = {16.0f, -5.0f, -4.0f};
    pose[leftKnee].rotationDegrees = {-12.0f, 0.0f, 0.0f};

    Entity figure;
    figure.model = &model;
    figure.skin = &skin;
    figure.position = kFigurePos;
    figure.yawDegrees = kFigureYaw;
    figure.pose = pose;

    EntitySet entities;
    entities.add(figure);
    scene.entities = &entities;

    // ---------------------------------------------------------- pickaxe
    VoxelModel pickaxeModel;
    bool havePickaxe = false;
    if (haveAssets) {
        item::ItemOptions options;
        options.roughness = 0.30f;
        options.metallic = 1.0f;
        std::string pickaxeError;
        havePickaxe = item::loadByName(source, "iron_pickaxe", pickaxeModel, options, &pickaxeError);
        if (!havePickaxe) std::printf("[strike] no iron_pickaxe: %s\n", pickaxeError.c_str());
    }
    if (!havePickaxe) {
        pickaxeModel = buildFallbackPickaxe();
        std::printf("[strike] fallback pickaxe, %zu voxels\n", pickaxeModel.solidCount());
    }

    VoxelModel lantern = buildLantern();

    PropSet props;
    {
        // jointTip is the bottom of the forearm box; the nudge puts the grip
        // inside the fist rather than on the wrist's lower edge.
        Vec3 fist = jointTip(model, rightElbow);
        fist.y += 1.15f;

        // Item +Y is the blade. The forearm points at the fingers along rest
        // -Y, so 180 about X lines the handle up with the arm. 90 about Y
        // turns the extruded sprite's face toward +X -- otherwise the camera,
        // which looks along -X, sees the two-texel edge and the pickaxe
        // disappears into a dark sliver. A little extra Z-roll cocks the head
        // the way a real swing holds it, rather than as a pole glued to the
        // wrist.
        VoxelAttachment held;
        held.model = &pickaxeModel;
        held.joint = rightElbow;
        held.offset = fist + Vec3{1.15f, 0.55f, 0.15f};
        held.anchor = gripVoxel(pickaxeModel, 0.28f);
        held.rotationDegrees = {180.0f, 90.0f, 18.0f};

        addAttachment(props, figure, held);

        Prop hanging;
        hanging.model = &lantern;
        hanging.position = {-4.35f, kFloor + 2.20f, -3.40f};
        hanging.voxelSize = 0.12f;
        hanging.anchor = Prop::Anchor::Centre;
        hanging.yawDegrees = -22.0f;
        props.add(hanging);

        props.build();
        scene.props = &props;

        Mat4 elbowToWorld = Mat4::identity();
        entityJointToWorld(figure, rightElbow, elbowToWorld);
        const Vec3 fistWorld = transformPoint(elbowToWorld, fist);
        std::printf("[strike] fist at (%.2f, %.2f, %.2f), pickaxe %zu voxels\n", fistWorld.x,
                    fistWorld.y, fistWorld.z, pickaxeModel.solidCount());
    }

    // ------------------------------------------------------------- camera
    // Three-quarter from in *front*, pickaxe side. The miner faces the
    // vein (-Z); standing behind them showed the back of the head and made
    // the hood read as a face looking the wrong way. From here the face
    // looks at the diamond, the pickaxe sits in the foreground, and the
    // block is in the same frame.
    const Vec3 eye{4.35f, kFloor + 1.18f, -3.28f};
    const Vec3 at{0.18f, kFloor + 1.32f, -3.95f};
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(34.0f);
    scene.camera.lookAt(eye, at);
    if (!draft && !view) {
        scene.camera.aperture = 0.040f;
        scene.camera.focusOn({0.40f, kFloor + 1.30f, -3.00f});
    }

    // ----------------------------------------------------------- sprites
    SpriteSet sprites;
    {
        scatter::ParticleStyle dust;
        dust.size = {0.055f, 0.055f};
        dust.sizeJitter = 0.55f;
        dust.texture = dustTexture.empty() ? nullptr : &dustTexture;
        dust.tint = {1.0f, 0.94f, 0.82f};
        dust.randomYaw = false;

        // Shaft through the roof crack, aimed at the diamond so the motes
        // sit on the same line as the swing.
        const Vec3 hole{0.5f, 5.6f, -3.0f};
        const Vec3 shaftDir = Vec3{0.1f, kFloor + 1.2f, -4.4f} - hole;
        std::vector<Sprite> motes =
            scatter::inBeam(hole, shaftDir, 7.2f, 1.35f, draft ? 220 : 900, kSeed + 3u, dust);
        scatter::removeInsideSolid(motes, scene.world);
        aimAt(motes, eye);
        sprites.add(motes);

        scatter::ParticleStyle haze;
        haze.size = {0.05f, 0.05f};
        haze.sizeJitter = 0.5f;
        haze.tint = {0.62f, 0.58f, 0.50f};
        haze.randomYaw = false;
        std::vector<Sprite> room =
            scatter::inBox({-4.5f, kFloor, -4.0f}, {4.5f, 4.6f, 5.0f}, draft ? 80 : 240,
                           kSeed + 4u, haze);
        scatter::removeInsideSolid(room, scene.world);
        aimAt(room, eye);
        sprites.add(room);
    }
    sprites.build();
    scene.sprites = &sprites;

    // -------------------------------------------------------------- light
    // Direction *toward* the sun, which sits in the roof crack. Broad, because
    // cel throws away the gradient of a hard key and keeps only the band.
    // Key from above and from the vein, not from the pickaxe side: a key
    // along +X puts the tool between the sun and the face, and the face
    // goes to the bottom cel band.
    scene.sun.direction = normalize(Vec3{0.40f, 0.74f, -0.54f});
    scene.sun.color = Vec3{1.0f, 0.95f, 0.86f};
    scene.sun.intensity = 4.0f;
    scene.sun.angularRadiusDegrees = 7.0f;

    scene.sky.zenith = Vec3{0.38f, 0.36f, 0.34f};
    scene.sky.horizon = Vec3{0.24f, 0.22f, 0.20f};
    scene.sky.ground = Vec3{0.14f, 0.12f, 0.10f};
    scene.sky.intensity = 0.40f;
    scene.ambientStrength = 0.48f;

    if (inspect) {
        scene.sun.intensity = 7.0f;
        scene.sky.intensity = 0.55f;
        scene.ambientStrength = 0.55f;
    }

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    if (view) {
        ViewportSettings vp;
        vp.title = "strike";
        vp.width = 1280;
        vp.height = 800;
        vp.moveSpeed = 4.0f;
        return runViewport(scene, vp);
    }

    // ------------------------------------------------------------- render
    PathSettings settings;
    settings.width = draft ? 720 : 1600;
    settings.height = draft ? 450 : 1000;
    settings.samplesPerPixel = draft ? 20 : 96;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = !draft;

    std::printf("[strike] rendering %dx%d at %d spp%s...\n", settings.width, settings.height,
                settings.samplesPerPixel, plain ? "" : (inspect ? ", inspect" : ", cel"));

    RenderStats stats;
    RenderTargets targets;
    renderPath(scene, settings, &stats, &targets);
    std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

    targets.color = denoise(targets, {});

    Image frame;
    if (plain || inspect) {
        frame = targets.color;
    } else {
        CelSettings cel;
        cel.bands = 4;
        cel.range = 1.40f;
        cel.shadowFloor = 0.36f;
        cel.bandGamma = 0.84f;
        cel.highlightCeiling = 3.0f;
        cel.depthThreshold = 0.42f;
        cel.normalThreshold = 0.30f;
        cel.outlineWidth = draft ? 1 : 2;
        cel.outlineColor = srgbToLinear(Vec3{0.06f, 0.05f, 0.05f});
        cel.outlineOpacity = 0.88f;
        cel.saturation = 1.14f;
        frame = celShade(targets, cel);
    }

    GradeSettings grade;
    grade.exposure = inspect ? 1.05f : 1.00f;
    grade.contrast = 1.08f;
    grade.saturation = 1.06f;
    grade.temperature = 0.02f;
    applyGrade(frame, grade);

    VignetteSettings vignette;
    vignette.amount = 0.38f;
    vignette.radius = 0.62f;
    applyVignette(frame, vignette);
    if (!draft) applyGrain(frame, 0.010f);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* suffix = inspect ? "_inspect" : (plain ? "_plain" : (draft ? "_draft" : ""));
    const std::string path = "out/" + outputName + suffix + ".png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[strike] wrote %s\n", path.c_str());
    return 0;
}
