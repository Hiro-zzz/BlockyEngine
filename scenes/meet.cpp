// Steve and Alex meet: a wave, then a handshake.
//
// Two figures, one small plank pad, a couple of lanterns. The shot is the
// poses -- elbows, knees, a jaw, and the same joint matrix that flattens an
// entity also plants a poppy in Steve's outer hand so the gift travels with
// the forearm rather than hovering in world space.
//
// Inner arms do the talking. Steve stands on +X (left of the frame, because
// the camera looks toward +Z and +X is left), yawed toward Alex; Alex mirrors
// him. Steve's left arm and Alex's right arm are the ones that point at each
// other, so those are the ones that wave and then clasp. The outer arms stay
// out of the way -- and Steve's right fist keeps the flower.
//
// Signs, from docs/conventions.md: a limb hangs below its pivot, so +X swings
// it forward toward -Z and +Z swings it toward +X (outward for a right arm,
// across the body for a left). The torso and the head rise above their pivots
// and take the same numbers the other way. The jaw hangs below its hinge, so
// opening it is a negative X.
//
//   scene_meet              full quality
//   scene_meet draft        small and fast, the whole take in a few minutes
//   scene_meet inspect      the keyed poses, head on, plus the clasp from the side
//   scene_meet ones         a new pose every frame instead of on twos
//   scene_meet plain        no cel shading, to compare
//   scene_meet out=name     write out/<name>/ and out/<name>.png
#include "engine/anim/take.hpp"
#include "engine/anim/track.hpp"
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/attach.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/prop/item.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/scene/scene.hpp"
#include "engine/world/shapes.hpp"
#include "scenes/common/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace blocky;

namespace {

constexpr float kDuration = 6.0f;
constexpr float kPixelsPerBlock = 16.0f;

// Hero camera. One definition, used by the take and by inspect, so the stills
// cannot drift away from the animation they are meant to check.
const Vec3 kEye{0.10f, 1.48f, -5.45f};
const Vec3 kAt {0.00f, 1.12f,  0.10f};
const Vec3 kFocus{0.00f, 1.42f, 0.00f};

const Vec3 kStevePos{0.84f, 0.0f, 0.06f};
const Vec3 kAlexPos {-0.84f, 0.0f, -0.04f};
constexpr float kSteveYaw = 34.0f;
constexpr float kAlexYaw  = -34.0f;

struct Hinges {
    int rightElbow = -1, leftElbow = -1;
    int rightKnee  = -1, leftKnee  = -1;
    int jaw        = -1;
};

Hinges addHinges(EntityModel& model) {
    Hinges h;
    h.rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    h.leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    h.rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    h.leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");
    h.jaw        = rigging::addJaw(model, joint::Head, 3.0f, "jaw");
    return h;
}

bool loadFirst(AssetSource& source, const char* const* paths, int count, Skin& skin) {
    std::string error;
    for (int i = 0; i < count; ++i) {
        if (skin.loadFromSource(source, paths[i], &error)) return true;
    }
    return false;
}

// A recognisable Steve / Alex when the client jar is not around. Eyes sit on
// the vanilla texels so face::scanFace can still find them.
Skin paintPlayer(bool alex) {
    ImageU8 image(64, 64);
    using RGBA = ImageU8::RGBA;

    auto fill = [&](int x, int y, int w, int h, RGBA c) {
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) image.set(x + i, y + j, c);
    };

    const RGBA skin  = alex ? RGBA{226, 168, 140, 255} : RGBA{181, 132,  90, 255};
    const RGBA hair  = alex ? RGBA{191, 108,  42, 255} : RGBA{ 43,  30,  20, 255};
    const RGBA shirt = alex ? RGBA{123, 140,  48, 255} : RGBA{  0, 168, 168, 255};
    const RGBA pants = alex ? RGBA{ 90,  58,  40, 255} : RGBA{ 60,  68, 170, 255};
    const RGBA shoes = RGBA{ 54,  54,  58, 255};
    const RGBA white = RGBA{245, 245, 240, 255};
    const RGBA iris  = alex ? RGBA{ 58,  96,  58, 255} : RGBA{ 82,  61, 137, 255};
    const RGBA mouth = RGBA{ 90,  48,  42, 255};

    // Head: top / bottom / left / front / right / back.
    fill(8,  0, 8, 8, hair);
    fill(16, 0, 8, 8, skin);
    fill(0,  8, 8, 8, skin);
    fill(8,  8, 8, 8, skin);
    fill(16, 8, 8, 8, skin);
    fill(24, 8, 8, 8, skin);
    fill(0,  8, 8, 3, hair);
    fill(8,  8, 8, 2, hair);
    fill(16, 8, 8, 3, hair);
    fill(24, 8, 8, 4, hair);

    // Vanilla eye pair at (9,12) and (13,12), two texels each, mirrored.
    fill(9,  12, 2, 1, white);
    fill(13, 12, 2, 1, white);
    image.set(10, 12, iris);
    image.set(13, 12, iris);
    fill(10, 15, 4, 1, mouth);

    fill(16, 16, 24, 16, shirt);   // body
    fill(40, 16, 16, 16, shirt);   // right arm
    fill(32, 48, 16, 16, shirt);   // left arm
    fill(0,  16, 16, 16, pants);   // right leg
    fill(16, 48, 16, 16, pants);   // left leg
    fill(0,  26, 16, 6, shoes);
    fill(16, 58, 16, 6, shoes);
    fill(40, 26, 16, 6, shirt);
    fill(32, 58, 16, 6, shirt);

    // Hands / cuffs.
    fill(40, 28, 16, 4, skin);
    fill(32, 60, 16, 4, skin);

    if (alex) {
        // Slim detection looks at two unused columns of the classic arm block.
        // Leave them transparent so buildPlayerModel picks the 3-pixel arms.
        RGBA clear{0, 0, 0, 0};
        for (int y = 20; y < 32; ++y) {
            image.set(54, y, clear);
            image.set(55, y, clear);
        }
        for (int y = 16; y < 20; ++y) {
            image.set(50, y, clear);
            image.set(51, y, clear);
        }
    }

    std::vector<uint8_t> png = pngEncode(image);
    Skin out;
    out.loadFromPng(png.data(), png.size(), nullptr);
    return out;
}

VoxelModel buildPoppy() {
    VoxelMaterial stem;
    stem.albedo = srgbToLinear(Vec3{0.20f, 0.46f, 0.14f});
    stem.roughness = 0.85f;
    VoxelMaterial petal;
    petal.albedo = srgbToLinear(Vec3{0.82f, 0.14f, 0.16f});
    petal.roughness = 0.55f;
    VoxelMaterial centre;
    centre.albedo = srgbToLinear(Vec3{0.18f, 0.10f, 0.05f});
    centre.roughness = 0.70f;

    return voxelize::fromLayers(
        {
            {"...", ".|.", "..."},
            {"...", ".|.", "..."},
            {"...", ".|.", "..."},
            {"...", ".|.", "..."},
            {".#.", "#@#", ".#."},
            {"...", ".#.", "..."},
        },
        {{'|', stem}, {'#', petal}, {'@', centre}});
}

void placeInFist(PropSet& props, const Entity& figure, int forearm, const VoxelModel& item,
                 float voxelScale, Vec3 eulerDegrees) {
    if (!figure.model) return;

    // jointTip gives the bottom of the forearm box; the nudge carries the
    // grip into the fist itself.
    Vec3 fist = jointTip(*figure.model, forearm);
    fist.y += 1.15f;

    VoxelAttachment held;
    held.model = &item;
    held.joint = forearm;
    held.offset = fist + Vec3{0.2f, 0.4f, 0.1f};
    held.anchor = gripVoxel(item);
    held.rotationDegrees = eulerDegrees;
    held.voxelScale = voxelScale;

    addAttachment(props, figure, held);
}

void buildPad(World& world) {
    world.fillBox({-10, -1, -8}, {10, -1, 8}, palette::GrassBlock);
    world.fillBox({-4, -1, -3}, {4, -1, 3}, palette::OakPlanks);

    // Kerb on the back and sides only. A wall on -Z sits between the camera
    // and the figures and eats the lower half of the frame.
    world.fillBox({-5, 0, 4}, {5, 0, 4}, palette::Cobblestone);
    world.fillBox({-5, 0, -3}, {-5, 0, 4}, palette::Cobblestone);
    world.fillBox({5, 0, -3}, {5, 0, 4}, palette::Cobblestone);

    // Lantern posts behind them. A prop glows but does not light, so the
    // emitters have to be real blocks.
    world.fillBox({-4, 0, 4}, {-4, 2, 4}, palette::OakLog);
    world.set({-4, 3, 4}, palette::Glowstone);
    world.fillBox({4, 0, 4}, {4, 2, 4}, palette::OakLog);
    world.set({4, 3, 4}, palette::Glowstone);
}

// --------------------------------------------------------------- poses
//
// Steve's inner arm is the left, Alex's is the right. Both hang at rest a few
// degrees off the ribs -- two boxes that share a shading band stop reading as
// two boxes, and on a handshake there is a moment where a limb is edge-on.

Pose steveRest(const Hinges& h) {
    Pose p;
    p[joint::Head].rotationDegrees     = {  4.0f,  16.0f,  0.0f};
    p[joint::Body].rotationDegrees     = { -3.0f,   6.0f,  2.0f};
    p[joint::RightArm].rotationDegrees = { -8.0f,   0.0f,  8.0f};
    p[h.rightElbow].rotationDegrees    = { 12.0f,   0.0f,  0.0f};
    p[joint::LeftArm].rotationDegrees  = { -6.0f,   0.0f, -8.0f};
    p[h.leftElbow].rotationDegrees     = { 10.0f,   0.0f,  0.0f};
    p[joint::RightLeg].rotationDegrees = {  3.0f,   4.0f,  2.0f};
    p[h.rightKnee].rotationDegrees     = { -6.0f,   0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = { -4.0f,  -2.0f, -2.0f};
    p[h.leftKnee].rotationDegrees      = { -4.0f,   0.0f,  0.0f};
    return p;
}

Pose steveWave(const Hinges& h) {
    Pose p = steveRest(h);
    p[joint::Head].rotationDegrees     = {  6.0f,  10.0f, -4.0f};
    p[joint::Body].rotationDegrees     = { -2.0f,   8.0f,  4.0f};
    // Straight up and out, then a little flop at the elbow. The first draft
    // pointed the fist at Alex's face and read as a punch.
    p[joint::LeftArm].rotationDegrees  = {  6.0f,   0.0f, -148.0f};
    p[h.leftElbow].rotationDegrees     = { 18.0f,   0.0f, -42.0f};
    p[joint::RightLeg].rotationDegrees = {  5.0f,   6.0f,  3.0f};
    return p;
}

Pose steveReach(const Hinges& h) {
    Pose p = steveRest(h);
    p[joint::Head].rotationDegrees     = {  8.0f,  12.0f,  0.0f};
    p[joint::Body].rotationDegrees     = { -4.0f,   4.0f,  7.0f};  // lean toward Alex
    // Horizontal-ish, a little forward so the clasp sits in front of the
    // chests rather than in the gap behind them.
    p[joint::LeftArm].rotationDegrees  = { 28.0f,  12.0f, -78.0f};
    p[h.leftElbow].rotationDegrees     = { 22.0f,  16.0f,   4.0f};
    p[joint::RightLeg].rotationDegrees = {  6.0f,   4.0f,  3.0f};
    p[joint::LeftLeg].rotationDegrees  = { -6.0f,  -2.0f, -3.0f};
    return p;
}

Pose steveClasp(const Hinges& h) {
    Pose p = steveReach(h);
    p[joint::Head].rotationDegrees     = { 10.0f,   8.0f,  2.0f};
    p[joint::Body].rotationDegrees     = { -5.0f,   2.0f,  9.0f};
    p[joint::LeftArm].rotationDegrees  = { 32.0f,  10.0f, -82.0f};
    p[h.leftElbow].rotationDegrees     = { 18.0f,  14.0f,   6.0f};
    p[joint::Root].offset              = { 0.0f,  0.0f, -0.4f};  // a half-pixel step in
    return p;
}

Pose alexRest(const Hinges& h) {
    Pose p;
    p[joint::Head].rotationDegrees     = {  3.0f, -18.0f,  0.0f};
    p[joint::Body].rotationDegrees     = { -2.0f,  -5.0f, -2.0f};
    p[joint::RightArm].rotationDegrees = { -6.0f,   0.0f,  8.0f};
    p[h.rightElbow].rotationDegrees    = { 10.0f,   0.0f,  0.0f};
    p[joint::LeftArm].rotationDegrees  = { -8.0f,   0.0f, -8.0f};
    p[h.leftElbow].rotationDegrees     = { 12.0f,   0.0f,  0.0f};
    p[joint::RightLeg].rotationDegrees = { -3.0f,   2.0f,  2.0f};
    p[h.rightKnee].rotationDegrees     = { -5.0f,   0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = {  4.0f,  -3.0f, -2.0f};
    p[h.leftKnee].rotationDegrees      = { -4.0f,   0.0f,  0.0f};
    return p;
}

Pose alexHi(const Hinges& h) {
    Pose p = alexRest(h);
    p[joint::Head].rotationDegrees     = {  8.0f, -12.0f,  4.0f};
    p[h.jaw].rotationDegrees           = {-11.0f,   0.0f,  0.0f};
    p[joint::RightArm].rotationDegrees = {  6.0f,  -4.0f,  18.0f};
    return p;
}

Pose alexWave(const Hinges& h) {
    Pose p = alexRest(h);
    p[joint::Head].rotationDegrees     = {  6.0f, -10.0f,  5.0f};
    p[joint::Body].rotationDegrees     = { -2.0f,  -6.0f, -4.0f};
    p[joint::RightArm].rotationDegrees = {  6.0f,   0.0f, 148.0f};
    p[h.rightElbow].rotationDegrees    = { 18.0f,   0.0f,  42.0f};
    p[joint::LeftLeg].rotationDegrees  = {  6.0f,  -4.0f, -3.0f};
    return p;
}

Pose alexReach(const Hinges& h) {
    Pose p = alexRest(h);
    p[joint::Head].rotationDegrees     = {  8.0f, -12.0f,  0.0f};
    p[joint::Body].rotationDegrees     = { -4.0f,  -4.0f, -7.0f};
    p[joint::RightArm].rotationDegrees = { 28.0f, -12.0f,  78.0f};
    p[h.rightElbow].rotationDegrees    = { 22.0f, -16.0f,  -4.0f};
    p[joint::RightLeg].rotationDegrees = { -6.0f,   2.0f,  3.0f};
    p[joint::LeftLeg].rotationDegrees  = {  6.0f,  -3.0f, -3.0f};
    return p;
}

Pose alexClasp(const Hinges& h) {
    Pose p = alexReach(h);
    p[joint::Head].rotationDegrees     = { 10.0f,  -8.0f, -2.0f};
    p[joint::Body].rotationDegrees     = { -5.0f,  -2.0f, -9.0f};
    p[joint::RightArm].rotationDegrees = { 32.0f, -10.0f,  82.0f};
    p[h.rightElbow].rotationDegrees    = { 18.0f, -14.0f,  -6.0f};
    p[joint::Root].offset              = { 0.0f,  0.0f, -0.4f};
    return p;
}

Pose breathe(float time, float sign) {
    Pose p;
    const float s = std::sin(time * kTwoPi / kDuration) * sign;
    p[joint::Root].rotationDegrees = {0.0f, 0.0f, 2.2f * s};
    p[joint::Body].rotationDegrees = {0.0f, 0.0f, -1.8f * s};
    p[joint::Head].rotationDegrees = {0.0f, -1.5f * s, 0.8f * s};
    p[joint::Root].offset = {0.0f, -0.18f * std::fabs(s), 0.0f};
    return p;
}

float shakeAmount(float time) {
    constexpr float kLo = 2.95f;
    constexpr float kHi = 4.35f;
    if (time < kLo || time > kHi) return 0.0f;
    const float u = (time - kLo) / (kHi - kLo);
    const float envelope = std::sin(u * kPi);
    return envelope * 8.5f * std::sin(u * kTwoPi * 2.5f);
}

void applyGaze(const face::EyeRig& rig, Pose& pose, float x, float y) {
    if (rig.built) face::gaze(rig, pose, x, y);
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    bool ones = false;
    bool inspect = false;
    std::string name = "meet";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "plain") plain = true;
        else if (arg == "ones") ones = true;
        else if (arg == "inspect") inspect = true;
        else if (arg.rfind("out=", 0) == 0) name = arg.substr(4);
        else {
            std::printf("[meet] unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }

    Scene scene(palette::registry());
    buildPad(scene.world);

    AssetSource source;
    BlockTextureLibrary blockTextures;
    const std::string jar = AssetSource::findClientJar();
    const bool haveAssets = !jar.empty() && source.open(jar, nullptr);
    if (haveAssets && blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
        scene.blockTextures = &blockTextures;
    }
    std::printf("[meet] assets %s\n", haveAssets ? "game" : "flat palette");

    Skin skinSteve, skinAlex;
    bool steveFromJar = false, alexFromJar = false;
    if (haveAssets) {
        const char* stevePaths[] = {
            "assets/minecraft/textures/entity/player/wide/steve.png",
            "assets/minecraft/textures/entity/steve.png",
        };
        const char* alexPaths[] = {
            "assets/minecraft/textures/entity/player/slim/alex.png",
            "assets/minecraft/textures/entity/alex.png",
        };
        steveFromJar = loadFirst(source, stevePaths, 2, skinSteve);
        alexFromJar  = loadFirst(source, alexPaths,  2, skinAlex);
    }
    if (!steveFromJar) skinSteve = paintPlayer(false);
    if (!alexFromJar)  skinAlex  = paintPlayer(true);

    std::printf("[meet] steve %s (%s)\n", steveFromJar ? "jar" : "painted",
                skinSteve.model() == SkinModel::Slim ? "slim" : "classic");
    std::printf("[meet] alex  %s (%s)\n", alexFromJar ? "jar" : "painted",
                skinAlex.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel modelSteve = buildPlayerModel(skinSteve);
    EntityModel modelAlex  = buildPlayerModel(skinAlex);
    const Hinges hs = addHinges(modelSteve);
    const Hinges ha = addHinges(modelAlex);
    if (hs.leftElbow < 0 || ha.rightElbow < 0 || hs.jaw < 0 || ha.jaw < 0) {
        std::printf("[meet] could not cut hinges\n");
        return 1;
    }

    face::EyeRig eyesSteve = face::buildEyeRig(modelSteve, skinSteve);
    face::EyeRig eyesAlex  = face::buildEyeRig(modelAlex,  skinAlex);
    std::printf("[meet] eyes steve %s, alex %s\n",
                eyesSteve.built ? "rigged" : eyesSteve.rejection,
                eyesAlex.built  ? "rigged" : eyesAlex.rejection);

    VoxelModel poppy;
    float poppyScale = 0.95f;
    if (haveAssets) {
        item::ItemOptions opt;
        opt.roughness = 0.55f;
        const char* flowerNames[] = {"poppy", "red_tulip", "dandelion"};
        for (const char* flower : flowerNames) {
            if (item::loadByName(source, flower, poppy, opt, nullptr)) {
                poppyScale = 0.55f;
                std::printf("[meet] %s from jar, %zu voxels\n", flower, poppy.solidCount());
                break;
            }
        }
    }
    if (poppy.empty()) {
        poppy = buildPoppy();
        std::printf("[meet] drawn poppy, %zu voxels\n", poppy.solidCount());
    }

    // ---- tracks. First and last keys match so the take loops.
    const auto hold = ease::hold(0.55f);
    const auto snap = ease::snap(0.12f, 1.18f);

    Track<Pose> steveTrack;
    steveTrack.key(0.00f,            steveRest(hs),  hold);
    steveTrack.key(0.80f,            steveWave(hs),  snap);
    steveTrack.key(2.10f,            steveReach(hs), hold);
    steveTrack.key(2.85f,            steveClasp(hs), hold);
    steveTrack.key(4.45f,            steveClasp(hs), hold);
    steveTrack.key(5.30f,            steveRest(hs),  hold);
    steveTrack.key(kDuration,        steveRest(hs));

    Track<Pose> alexTrack;
    alexTrack.key(0.00f,             alexRest(ha),  hold);
    alexTrack.key(0.45f,             alexHi(ha),    hold);
    alexTrack.key(0.95f,             alexWave(ha),  snap);
    alexTrack.key(2.20f,             alexReach(ha), hold);
    alexTrack.key(2.85f,             alexClasp(ha), hold);
    alexTrack.key(4.45f,             alexClasp(ha), hold);
    alexTrack.key(5.30f,             alexRest(ha),  hold);
    alexTrack.key(kDuration,         alexRest(ha));

    // ---- light and camera, built once
    scene.sun.direction = normalize(Vec3{0.48f, 0.64f, -0.60f});
    scene.sun.color = Vec3{1.0f, 0.94f, 0.82f};
    scene.sun.intensity = 6.2f;
    scene.sun.angularRadiusDegrees = 2.4f;

    scene.sky.zenith  = Vec3{0.18f, 0.32f, 0.62f};
    scene.sky.horizon = Vec3{0.70f, 0.72f, 0.78f};
    scene.sky.ground  = Vec3{0.22f, 0.24f, 0.16f};
    scene.sky.intensity = 0.95f;
    scene.ambientStrength = 0.85f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.55f, 0.68f, 0.82f});

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(32.0f);
    scene.camera.aperture = 0.018f;
    scene.camera.lookAt(kEye, kAt);
    scene.camera.focusOn(kFocus);

    EntitySet entities;
    PropSet props;

    Entity steve;
    steve.model = &modelSteve;
    steve.skin = &skinSteve;
    steve.position = kStevePos;
    steve.yawDegrees = kSteveYaw;

    Entity alex;
    alex.model = &modelAlex;
    alex.skin = &skinAlex;
    alex.position = kAlexPos;
    alex.yawDegrees = kAlexYaw;

    auto poseAt = [&](float time, Pose& outSteve, Pose& outAlex) {
        outSteve = poseAdd(steveTrack.at(time), breathe(time, 1.0f));
        outAlex  = poseAdd(alexTrack.at(time),  breathe(time, -1.0f));

        const float shake = shakeAmount(time);
        outSteve[hs.leftElbow].rotationDegrees.x  += shake;
        outAlex [ha.rightElbow].rotationDegrees.x += shake;

        // Look at each other most of the time; glance down at the hands
        // while they are clasped.
        float down = 0.0f;
        if (time > 2.5f && time < 4.7f) {
            const float u = saturate((time - 2.5f) / 0.4f) *
                            (1.0f - saturate((time - 4.3f) / 0.4f));
            down = u;
        }
        applyGaze(eyesSteve, outSteve, -0.55f, -0.65f * down);
        applyGaze(eyesAlex,  outAlex,   0.55f, -0.65f * down);
    };

    auto assemble = [&](Scene& s, float time) {
        Pose ps, pa;
        poseAt(time, ps, pa);
        steve.pose = ps;
        alex.pose  = pa;

        entities.clear();
        entities.add(steve);
        entities.add(alex);
        s.entities = &entities;

        props.clear();
        // Stem along the forearm: item +Y is up in rest, fingers point rest
        // -Y, so 180 about X lines them up. A little extra roll keeps the
        // bloom facing the camera rather than edge-on.
        placeInFist(props, steve, hs.rightElbow, poppy, poppyScale, {180.0f, 90.0f, 18.0f});

        Prop planted;
        planted.model = &poppy;
        planted.position = {0.05f, 0.0f, 1.35f};
        planted.voxelSize = poppyScale / kPixelsPerBlock;
        planted.yawDegrees = 22.0f;
        props.add(planted);

        props.build();
        s.props = &props;
    };

    if (inspect) {
        PathSettings probe;
        probe.width = 640;
        probe.height = 360;
        probe.samplesPerPixel = 16;
        probe.maxBounces = 4;
        probe.progress = false;

        const std::pair<const char*, float> beats[] = {
            {"rest",  0.10f},
            {"wave",  1.30f},
            {"reach", 2.40f},
            {"clasp", 3.40f},
        };

        ToneParams tone;
        tone.curve = Tonemap::ACES;

        for (const auto& [label, time] : beats) {
            assemble(scene, time);
            RenderTargets targets;
            renderPath(scene, probe, nullptr, &targets);
            Image frame = denoise(targets, {});
            const std::string path = "out/" + name + "_" + label + ".png";
            std::string error;
            if (!pngSave(path, frame, tone, &error)) {
                std::printf("save failed: %s\n", error.c_str());
                return 1;
            }
            std::printf("[meet] wrote %s\n", path.c_str());
        }

        // Silhouette of the clasp: the hero angle cannot tell you whether
        // the hands actually meet, because an arm pointing at the camera
        // is a box whatever it is doing.
        scene.camera.fovY = radians(40.0f);
        scene.camera.lookAt({4.85f, 1.55f, -3.40f}, {0.0f, 1.05f, 0.05f});
        scene.camera.aperture = 0.0f;
        assemble(scene, 3.40f);
        RenderTargets targets;
        renderPath(scene, probe, nullptr, &targets);
        Image frame = denoise(targets, {});
        std::string error;
        const std::string path = "out/" + name + "_clasp_side.png";
        if (!pngSave(path, frame, tone, &error)) {
            std::printf("save failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("[meet] wrote %s\n", path.c_str());
        return 0;
    }

    Take take;
    take.name = draft ? name + "_draft" : name;
    take.timing.duration = kDuration;
    take.timing.fps = 24;
    take.timing.stepEvery = ones ? 1 : 2;

    // H.264 4:2:0 needs even dimensions. 720x405 is exact 16:9 and looks
    // right, but the encoder will refuse it -- 406 keeps the same framing
    // to a pixel and lets writeMp4 succeed.
    take.settings.width  = draft ? 720 : 1280;
    take.settings.height = draft ? 406 : 720;
    take.settings.samplesPerPixel = draft ? 16 : 48;
    take.settings.maxBounces = draft ? 4 : 8;

    take.tone.curve = Tonemap::ACES;
    take.writeMp4 = true;
    take.scene = &scene;

    take.shot = [&](Scene& s, const Frame& f) {
        assemble(s, f.time);
    };

    if (!plain) {
        take.finish = [&](RenderTargets& targets, const Frame&) {
            CelSettings cel;
            cel.bands = 4;
            cel.range = 1.35f;
            cel.bandGamma = 1.0f;
            cel.highlightCeiling = 3.0f;
            cel.shadowFloor = 0.18f;
            cel.depthThreshold = 0.34f;
            cel.normalThreshold = 0.30f;
            cel.outlineWidth = draft ? 1 : 2;
            cel.outlineColor = srgbToLinear(Vec3{0.08f, 0.06f, 0.05f});
            cel.outlineOpacity = 0.88f;
            cel.saturation = 1.08f;

            Image frame = celShade(targets, cel);

            GradeSettings grade;
            grade.contrast = 1.05f;
            grade.saturation = 1.06f;
            grade.temperature = 0.08f;
            applyGrade(frame, grade);
            applyVignette(frame, {0.22f, 0.70f, 0.50f});
            return frame;
        };
    }

    std::string error;
    TakeStats stats;
    if (!renderTake(take, &stats, &error)) {
        std::printf("[meet] failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
