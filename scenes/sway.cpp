// Hands on hips, weight rocking from one foot to the other.
//
// The simplest animation that is still animation: nothing enters or leaves,
// the camera does not move, and the whole shot is one figure shifting their
// weight. Which makes it the right thing to build second -- everything that
// can go wrong here is the pose, and the pose is the part with the sign
// conventions in it.
//
// -------------------------------------------------------------- the camera
//
// Fixed, and standing on -Z. An entity looks towards -Z, so that is the side
// its face is on; a camera on +Z photographs the back of its head. The
// turntable got this wrong by writing the obvious `cos`, and the way to not
// get it wrong again is to not compute it at all. There is one eye position,
// it is written down, and `inspect` renders stills from it before any
// animation is traced.
//
// ---------------------------------------------------------------- the sway
//
// A hip sway is two rotations that disagree, and the skeleton is what makes
// that expressible. The root pivot sits on the floor between the feet, so
// turning it about Z leans the whole figure without lifting either foot. The
// body pivot sits at the waist, so turning it back the other way brings the
// shoulders upright again. What is left over is the hips travelling sideways
// under a torso that stays put -- which is the whole of the move.
//
// Doing this without parenting would mean moving the pelvis and then hand
// correcting the head, both arms and the torso to follow it. That is the
// thing rig.hpp was written to stop.
//
// The arms are children of the body, so they come along for free. That is
// also why the hands stay on the hips through the whole sway without a single
// keyframe: they are attached to the thing that is moving.
//
// --------------------------------------------------------------- no track
//
// A rocking motion is a sine, and a sine is already an animation. There is
// nothing here for `Track` to improve -- keys would only be a coarser way of
// writing the same curve. `stepEvery = 2` is what makes it read as animation
// on twos rather than as a smooth wobble: the sine is sampled twelve times a
// second and holds in between.
//
//   scene_sway              full quality
//   scene_sway draft        small and fast, the whole take in seconds
//   scene_sway inspect      the pose at both extremes and at rest, head on
//   scene_sway ones         a new pose every frame instead of on twos
//   scene_sway plain        no cel shading, to compare
//   scene_sway <png>        a different skin
//   scene_sway out=name     write out/<name>/ and out/<name>.png
#include "engine/anim/take.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultSkin = "C:/Users/yueiw/Downloads/1407faed3b1db843.png";

// Where the camera stands. One definition, used by both the animation and the
// stills, so the two cannot drift apart.
const Vec3 kEye{0.25f, 1.30f, -4.60f};
const Vec3 kAt {0.00f, 1.02f,  0.00f};

bool loadSkinFile(const std::string& path, Skin& skin, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

// Joint indices the pose needs beyond the fixed humanoid seven.
struct Arms {
    int rightElbow = -1;
    int leftElbow  = -1;
};

// The pose at a given point in the rock. `s` runs -1 .. +1 and is the only
// thing that changes over the whole take.
//
// Signs, from docs/conventions.md and its second note: a limb hangs *below*
// its pivot, so +Z swings it towards +X -- outwards for the right arm, across
// the body for the left. A part that rises *above* its pivot takes the same
// sign the other way, which is why the root and the body read as leaning
// opposite each other here when their numbers have opposite signs.
Pose swayPose(const Arms& arms, float s) {
    Pose pose;

    // ---- hands on hips
    //
    // The shoulder takes the elbow out to the side and the elbow folds the
    // forearm back in, so the hand lands on the waist rather than out in the
    // air. Both halves are needed: a shoulder rotation alone puts the whole
    // arm out like a teapot, and an elbow rotation alone leaves the elbow
    // pinned to the ribs.
    //
    // The elbow angle is close to minus the shoulder angle for a reason. The
    // hinge is a child of the shoulder, so its rotation composes on top:
    // cancelling most of the shoulder's swing is what points the forearm back
    // down and inwards towards the hip.
    pose[joint::RightArm].rotationDegrees   = {-8.0f, 0.0f,  38.0f};
    pose[arms.rightElbow].rotationDegrees   = {14.0f, 0.0f, -79.0f + 5.0f * s};
    pose[joint::LeftArm].rotationDegrees    = {-8.0f, 0.0f, -38.0f};
    pose[arms.leftElbow].rotationDegrees    = {14.0f, 0.0f,  79.0f + 5.0f * s};

    // ---- the rock
    //
    // Root leans, body leans back harder, head keeps itself roughly level.
    // The body's number is the larger of the two on purpose: cancelling the
    // lean exactly would leave a figure whose shoulders are nailed in place,
    // and a little overshoot at the top reads as the torso riding the hips
    // rather than resisting them.
    pose[joint::Root].rotationDegrees = {0.0f, 0.0f, 9.0f * s};
    pose[joint::Body].rotationDegrees = {0.0f, 0.0f, -10.5f * s};
    pose[joint::Head].rotationDegrees = {0.0f, -6.0f * s, 3.5f * s};

    // A shallow dip at each extreme, in model pixels. Sixteen to a block, so
    // a third of a pixel is two hundredths of a block -- far too small to
    // notice as a movement and just enough to stop the head tracking a
    // perfectly flat line, which is the thing that reads as mechanical.
    pose[joint::Root].offset = {0.0f, -0.30f * std::fabs(s), 0.0f};

    // The legs take the weight: the loaded one straightens under the hip that
    // has moved over it.
    pose[joint::RightLeg].rotationDegrees = {0.0f, 0.0f, -2.5f * s};
    pose[joint::LeftLeg].rotationDegrees  = {0.0f, 0.0f, -2.5f * s};

    return pose;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    bool ones = false;
    bool inspect = false;
    std::string skinPath;
    std::string name = "sway";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "plain") plain = true;
        else if (arg == "ones") ones = true;
        else if (arg == "inspect") inspect = true;
        else if (arg.rfind("out=", 0) == 0) name = arg.substr(4);
        else skinPath = arg;
    }
    if (skinPath.empty()) skinPath = kDefaultSkin;

    std::string error;
    Skin skin;
    if (!loadSkinFile(skinPath, skin, &error)) {
        std::printf("could not load %s: %s\n", skinPath.c_str(), error.c_str());
        return 1;
    }
    std::printf("[sway] skin %s (%s)\n", skinPath.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel model = buildPlayerModel(skin);
    Arms arms;
    arms.rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    arms.leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    if (arms.rightElbow < 0 || arms.leftElbow < 0) {
        std::printf("[sway] could not add elbow hinges\n");
        return 1;
    }

    // ---- the scene, built once
    // Only one block in it: this scene builds nothing else out of the world.
    BlockRegistry registry;
    Scene scene(registry);

    // A rim light, the same trick scene_charm uses and for the same reason:
    // this costume is nearly black from collar to hem, and against a backdrop
    // dark enough to suit it there is no edge on the side the sun misses.
    //
    // The turntable could not do this -- an orbiting camera eventually looks
    // everywhere a lamp could stand. A fixed camera can, because there is
    // exactly one cone to stay out of, and above and behind is outside it.
    BlockDef lamp;
    lamp.name = "studio_rim";
    lamp.albedo = Vec3{0.0f};
    lamp.emission = srgbToLinear(Vec3{0.72f, 0.80f, 1.0f}) * 42.0f;
    const BlockId rimLamp = registry.add(lamp);

    scene.world.fillBox({-3, 4, 1}, {-2, 5, 3}, rimLamp);

    scene.sun.direction = normalize(Vec3{0.62f, 0.52f, -0.58f});
    scene.sun.color = Vec3{1.0f, 0.95f, 0.90f};
    scene.sun.intensity = 3.6f;
    scene.sun.angularRadiusDegrees = 6.0f;

    scene.sky.zenith  = Vec3{0.42f, 0.38f, 0.46f};
    scene.sky.horizon = Vec3{0.48f, 0.41f, 0.45f};
    scene.sky.ground  = Vec3{0.30f, 0.24f, 0.26f};
    scene.sky.intensity = 0.55f;
    scene.ambientStrength = 0.70f;

    // Dusty mauve, a shade lighter than the costume's own darks. A backdrop
    // darker than the subject turns the subject into a hole.
    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.341f, 0.251f, 0.302f});

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(31.0f);
    scene.camera.aperture = 0.010f;
    scene.camera.lookAt(kEye, kAt);
    scene.camera.focusOn(Vec3{0.0f, 1.55f, 0.0f});   // the face, not the middle

    EntitySet entities;
    Entity figure;
    figure.model = &model;
    figure.skin = &skin;
    figure.position = {0.0f, 0.0f, 0.0f};
    figure.yawDegrees = 0.0f;   // zero means looking towards -Z, at the camera

    // ---- inspect: the two extremes and the middle, before anything is traced
    if (inspect) {
        PathSettings probe;
        probe.width = 420;
        probe.height = 560;
        probe.samplesPerPixel = 32;
        probe.maxBounces = 4;
        probe.progress = false;

        const std::pair<const char*, float> stops[] = {
            {"left", -1.0f}, {"middle", 0.0f}, {"right", 1.0f}};

        for (const auto& [label, s] : stops) {
            figure.pose = swayPose(arms, s);
            entities.clear();
            entities.add(figure);
            scene.entities = &entities;

            RenderTargets targets;
            renderPath(scene, probe, nullptr, &targets);

            ToneParams tone;
            tone.curve = Tonemap::ACES;
            const std::string path = "out/" + name + "_" + label + ".png";
            if (!pngSave(path, targets.color, tone, &error)) {
                std::printf("save failed: %s\n", error.c_str());
                return 1;
            }
            std::printf("[sway] wrote %s\n", path.c_str());
        }
        return 0;
    }

    // ---- the take
    const float duration = 4.0f;

    Take take;
    take.name = draft ? name + "_draft" : name;
    take.timing.duration = duration;
    take.timing.fps = 24;
    take.timing.stepEvery = ones ? 1 : 2;

    take.settings.width  = draft ? 420 : 900;
    take.settings.height = draft ? 560 : 1200;
    take.settings.samplesPerPixel = draft ? 24 : 96;
    take.settings.maxBounces = draft ? 4 : 8;

    take.tone.curve = Tonemap::ACES;
    take.writeMp4 = true;
    take.scene = &scene;

    take.shot = [&](Scene& s, const Frame& f) {
        // Two full rocks over the take, so the last frame lands one step short
        // of the start and the loop closes without a seam.
        const float sway = std::sin(f.t01 * kTwoPi * 2.0f);

        figure.pose = swayPose(arms, sway);

        // EntitySet::add flattens the pose into world-space boxes on the spot,
        // so a new pose needs a new set.
        entities.clear();
        entities.add(figure);
        s.entities = &entities;
    };

    if (!plain) {
        take.finish = [&](RenderTargets& targets, const Frame&) {
            CelSettings cel;
            cel.bands = 3;
            cel.range = 1.15f;
            cel.bandGamma = 1.0f;
            cel.highlightCeiling = 3.0f;
            cel.shadowFloor = 0.19f;
            cel.depthThreshold = 0.32f;
            cel.normalThreshold = 0.28f;
            cel.outlineWidth = draft ? 1 : 2;
            cel.outlineColor = srgbToLinear(Vec3{0.09f, 0.05f, 0.07f});
            cel.outlineOpacity = 0.90f;
            cel.saturation = 1.12f;

            Image frame = celShade(targets, cel);

            GradeSettings grade;
            grade.contrast = 1.05f;
            grade.saturation = 1.06f;
            applyGrade(frame, grade);
            applyVignette(frame, {0.26f, 0.68f, 0.55f});
            return frame;
        };
    }

    TakeStats stats;
    if (!renderTake(take, &stats, &error)) {
        std::printf("[sway] failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
