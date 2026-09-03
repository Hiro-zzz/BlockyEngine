// Two characters, close up, posed against a flat studio backdrop.
//
// No set, no props, no game assets: the whole scene is two rigged figures and
// one floor plane. The floor exists only to catch the contact shadow -- its
// material is matched to the background colour, so a ray that lands on it far
// from the figures comes back the same value as a ray that missed the world
// entirely, and the horizon disappears. What is left is the shadow.
//
// Both figures get elbows and knees cut in (rigging::addHinge), because the
// pose is the whole point of the scene and shoulders alone cannot make it.
//
//   scene_duo              full quality
//   scene_duo draft        small and fast, for tuning the pose
//   scene_duo inspect      a side angle at draft size, to check the poses
//   scene_duo <a> <b>      override the two skin paths
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kSkinA = "C:/Users/yueiw/Downloads/d22e8cd1208f2767.png";
const char* kSkinB = "C:/Users/yueiw/Downloads/3dea0b516cd7e48d.png";

// The backdrop colour, in sRGB as it would be picked out of the reference.
// Both the miss colour and the floor material come from this one value.
const Vec3 kBackdrop{0.255f, 0.235f, 0.290f};

// Skins here come from loose files rather than from a resource pack, so this
// goes through readFileBytes -- the project path has a Cyrillic character in
// it and the narrow CRT would mangle it.
bool loadSkinFile(const std::string& path, Skin& skin, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

// One arm gains an elbow, one leg a knee, on both sides. Returned in the
// order the poses below name them.
struct Hinges {
    int rightElbow = -1, leftElbow = -1;
    int rightKnee = -1, leftKnee = -1;
};

Hinges addHinges(EntityModel& model) {
    Hinges h;
    h.rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    h.leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    h.rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    h.leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");
    return h;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool inspect = false;
    std::vector<std::string> skinPaths;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else if (std::strcmp(argv[i], "inspect") == 0) inspect = draft = true;
        else skinPaths.push_back(argv[i]);
    }
    std::string pathA = skinPaths.size() > 0 ? skinPaths[0] : kSkinA;
    std::string pathB = skinPaths.size() > 1 ? skinPaths[1] : kSkinB;

    std::string error;
    Skin skinA, skinB;
    if (!loadSkinFile(pathA, skinA, &error)) {
        std::printf("could not load %s: %s\n", pathA.c_str(), error.c_str());
        return 1;
    }
    if (!loadSkinFile(pathB, skinB, &error)) {
        std::printf("could not load %s: %s\n", pathB.c_str(), error.c_str());
        return 1;
    }
    std::printf("[duo] skin A: %s%s\n", skinA.model() == SkinModel::Slim ? "slim" : "classic",
                skinA.wasLegacy() ? ", expanded from 64x32" : "");
    std::printf("[duo] skin B: %s%s\n", skinB.model() == SkinModel::Slim ? "slim" : "classic",
                skinB.wasLegacy() ? ", expanded from 64x32" : "");

    // ------------------------------------------------------------ the studio
    // A matte floor whose albedo is tuned so that, under this sun and sky, it
    // renders at about the backdrop's value. Anything else here would put a
    // horizon line straight across the frame behind the figures.
    BlockRegistry registry;
    BlockDef floorDef;
    floorDef.name = "studio_floor";
    floorDef.albedo = srgbToLinear(kBackdrop) * 0.58f;
    floorDef.roughness = 1.0f;
    const BlockId studioFloor = registry.add(floorDef);

    Scene scene(registry);
    scene.world.fillBox({-30, -1, -30}, {30, -1, 30}, studioFloor);

    // ------------------------------------------------------------- the cast
    EntityModel modelA = buildPlayerModel(skinA);
    EntityModel modelB = buildPlayerModel(skinB);
    const Hinges a = addHinges(modelA);
    const Hinges b = addHinges(modelB);

    // Sign reminders, all from docs/conventions.md:
    //   a limb hangs below its pivot  -> +X swings it forward (-Z),
    //                                    +Z swings it towards +X
    //   the torso rises above the waist -> folding it forward is -X
    //   the head rises above the neck   -> looking up is +X
    // The entity's own right is +X, and the camera sits at -Z, so +X is on
    // the *left* of the finished image.

    // Figure A, on the left of the frame: folded forward at the waist, right
    // arm thrown up with the elbow folded back over the head, left arm
    // hanging loose.
    Pose poseA;
    poseA[joint::Body].rotationDegrees     = {-16.0f,  13.0f,   6.0f};
    poseA[joint::Head].rotationDegrees     = { 12.0f, -11.0f,  -4.0f};
    // Nearly straight up at the shoulder, then folded hard at the elbow so
    // the forearm comes back inwards over the crown -- the hand ends behind
    // the head rather than beside it. The elbow's -X is what carries it
    // *behind*: without it the forearm folds down across the face instead.
    poseA[joint::RightArm].rotationDegrees = {  8.0f,   0.0f, 150.0f};
    // -50 and not -25: the torso is folded 16 degrees forward, and that fold
    // carries the whole arm with it. An elbow that clears the head at rest
    // lands square on the face once the body leans, so the swing back has to
    // pay for the lean as well as for itself.
    poseA[a.rightElbow].rotationDegrees    = {-50.0f,   0.0f,  95.0f};
    poseA[joint::LeftArm].rotationDegrees  = {-12.0f,   0.0f, -24.0f};
    poseA[a.leftElbow].rotationDegrees     = { 42.0f, -14.0f,   0.0f};
    poseA[joint::RightLeg].rotationDegrees = {  9.0f,   0.0f,   3.0f};
    poseA[a.rightKnee].rotationDegrees     = {-11.0f,   0.0f,   0.0f};
    poseA[joint::LeftLeg].rotationDegrees  = { -7.0f,   0.0f,  -4.0f};
    poseA[a.leftKnee].rotationDegrees      = { -6.0f,   0.0f,   0.0f};

    // Figure B, on the right: nearly upright, both arms bent forward at the
    // elbow so the hands come together in front of the waist, head dipped.
    Pose poseB;
    poseB[joint::Body].rotationDegrees     = { -7.0f,  -9.0f,  -3.0f};
    poseB[joint::Head].rotationDegrees     = { -5.0f,  13.0f,   2.0f};
    // Shoulder plus elbow must add up to about 90 for a level forearm. The
    // sideways lean of the hands is Y on the elbow, not Z: once the forearm
    // points along -Z a rotation about Z barely moves it at all.
    poseB[joint::RightArm].rotationDegrees = { 24.0f,   0.0f,   9.0f};
    poseB[b.rightElbow].rotationDegrees    = { 60.0f,  18.0f,   0.0f};
    poseB[joint::LeftArm].rotationDegrees  = { 20.0f,   0.0f, -10.0f};
    poseB[b.leftElbow].rotationDegrees     = { 58.0f, -20.0f,   0.0f};
    poseB[joint::RightLeg].rotationDegrees = {  5.0f,   0.0f,   2.0f};
    poseB[b.rightKnee].rotationDegrees     = { -7.0f,   0.0f,   0.0f};
    poseB[joint::LeftLeg].rotationDegrees  = { -4.0f,   0.0f,  -2.0f};
    poseB[b.leftKnee].rotationDegrees      = { -5.0f,   0.0f,   0.0f};

    // The set stores pointers only: the models and skins must outlive it.
    EntitySet entities;
    {
        Entity figure;
        figure.model = &modelA;
        figure.skin = &skinA;
        figure.position = {0.72f, 0.0f, 0.12f};
        figure.yawDegrees = 31.0f;   // turned towards the other figure
        figure.pose = poseA;
        entities.add(figure);

        figure.model = &modelB;
        figure.skin = &skinB;
        figure.position = {-0.74f, 0.0f, -0.10f};
        figure.yawDegrees = -27.0f;
        figure.pose = poseB;
        entities.add(figure);
    }
    scene.entities = &entities;
    std::printf("[duo] %zu figures, %zu boxes, %zu joints each\n", entities.entityCount(),
                entities.boxCount(), modelA.skeleton.size());

    // ------------------------------------------------------------- lighting
    // A soft key from above and in front, on the left of the frame (+X), and
    // a cool wash from the sky for fill. Nothing warm and nothing hard: the
    // reference is a studio render, not an outdoor one.
    scene.sun.direction = normalize(Vec3{0.46f, 0.72f, -0.52f});
    scene.sun.color = Vec3{1.0f, 0.97f, 0.93f};
    scene.sun.intensity = 4.2f;
    scene.sun.angularRadiusDegrees = 7.0f;   // wide, for a soft contact shadow

    scene.sky.zenith  = Vec3{0.26f, 0.24f, 0.33f};
    scene.sky.horizon = Vec3{0.34f, 0.31f, 0.38f};
    scene.sky.ground  = Vec3{0.22f, 0.20f, 0.26f};
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 1.0f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(kBackdrop);

    // --------------------------------------------------------------- camera
    // Chest height, a long-ish lens so the two figures do not splay outwards
    // at the edges of a square frame.
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(31.0f);
    scene.camera.lookAt({0.05f, 1.26f, -5.35f}, {-0.02f, 1.00f, 0.0f});
    scene.camera.aperture = 0.035f;
    scene.camera.focusOn({0.0f, 1.35f, 0.0f});

    // `inspect` steps round to the side and backs off. A pose is checked in
    // silhouette, and the hero angle is the one angle that cannot show it:
    // an arm pointing at the camera is a box, whatever it is doing.
    if (inspect) {
        scene.camera.fovY = radians(38.0f);
        scene.camera.lookAt({4.60f, 1.85f, -5.00f}, {-0.10f, 1.05f, 0.0f});
        scene.camera.aperture = 0.0f;
    }

    PathSettings settings;
    settings.width  = draft ? 700 : 1400;
    settings.height = draft ? 700 : 1400;
    settings.samplesPerPixel = draft ? 24 : 96;
    settings.maxBounces = draft ? 4 : 8;

    std::printf("[duo] rendering %dx%d at %d spp...\n", settings.width, settings.height,
                settings.samplesPerPixel);

    RenderStats stats;
    RenderTargets targets;
    Image frame = renderPath(scene, settings, &stats, &targets);

    std::printf("  %.1f s on %d threads, %.1f M rays (%.2f Mrays/s)\n", stats.seconds,
                stats.threadsUsed, double(stats.totalRays) / 1e6,
                double(stats.totalRays) / 1e6 / (stats.seconds > 0.0 ? stats.seconds : 1.0));

    // ----------------------------------------------------------- post, in linear
    frame = denoise(targets, {});

    BloomSettings bloom;
    bloom.threshold = 1.6f;
    bloom.intensity = 0.030f;   // barely there: nothing in this scene emits
    applyBloom(frame, bloom);

    GradeSettings grade;
    grade.contrast = 1.05f;
    grade.saturation = 1.06f;
    applyGrade(frame, grade);

    VignetteSettings vignette;
    vignette.amount = 0.12f;   // the backdrop must stay flat
    applyVignette(frame, vignette);
    applyGrain(frame, 0.008f);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = inspect ? "out/duo_inspect.png"
                               : (draft ? "out/duo_draft.png" : "out/duo.png");
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[duo] wrote %s\n", path);
    return 0;
}
