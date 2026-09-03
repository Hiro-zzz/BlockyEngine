// The first animation: a character on a turntable, animated on twos.
//
// Two things are being shown at once, and they are the two ways a shot can be
// written.
//
//   The camera is a formula. A full turn over the take is one line of
//   trigonometry, and no keyframe system would improve on it.
//   The pose is a track. Three poses in a ring, each holding still and then
//   snapping to the next, which is what `ease::hold` is for.
//
// Both live inside one shot and neither knows about the other. That is the
// whole of the design: a shot is a function of time, and how you compute the
// scene from that time is your business.
//
// ---------------------------------------------------------------- on twos
//
// `stepEvery = 2` holds every pose for two frames. It is the classic twelve
// per second of limited animation, and it halves the render: the second frame
// of each pair is a byte copy of the first rather than a second trace. Pass
// `ones` to compare -- it takes twice as long and, on a blocky figure, mostly
// looks less deliberate.
//
// ------------------------------------------------------------ no rim light
//
// scene_charm stands a panel of glowing blocks off to one side, outside the
// cone the camera can see. That trick cannot survive a turntable: the camera
// goes all the way round, so every place the panel could stand is somewhere
// the camera eventually looks. A lamp for an orbiting shot has to be a sky,
// and here it is -- a coloured sky doing the fill and one sun doing the key.
//
//   scene_turntable            full quality
//   scene_turntable draft      small and fast, the whole take in seconds
//   scene_turntable ones       a new pose every frame instead of on twos
//   scene_turntable plain      no cel shading, to compare
//   scene_turntable inspect    the three keyed poses head on, one still each
//   scene_turntable <png>      a different skin
//   scene_turntable out=name   write out/<name>/ and out/<name>.png
#include "engine/anim/take.hpp"
#include "engine/anim/track.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultSkin = "assets/skins/charlie.png";

bool loadSkinFile(const std::string& path, Skin& skin, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    bool ones  = false;
    bool inspect = false;
    std::string skinPath;
    std::string name = "turntable";

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
    std::printf("[turntable] skin %s (%s)\n", skinPath.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    // ---- the rig
    EntityModel model = buildPlayerModel(skin);
    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");

    // ---- three poses in a ring
    //
    // Signs, from docs/conventions.md: a limb hangs below its pivot, so +X
    // swings it forward towards -Z and +Z swings it towards +X -- outwards for
    // the right arm, across the body for the left. The torso rises above its
    // pivot and so takes the same sign the other way.
    //
    // Every pose keeps the arms a few degrees clear of the body. Two boxes
    // that touch share a shading band and stop reading as two boxes, and on a
    // turntable there is always some angle where a limb is edge-on to the
    // camera and has nothing but that gap to give it away.

    Pose rest;
    rest[joint::RightArm].rotationDegrees = {0.0f, 0.0f, 5.0f};
    rest[joint::LeftArm].rotationDegrees  = {0.0f, 0.0f, -5.0f};

    Pose shifted;  // weight onto one hip, head coming back the other way
    shifted[joint::Body].rotationDegrees     = {-6.0f, 9.0f, 4.0f};
    shifted[joint::Head].rotationDegrees     = {5.0f, -15.0f, -8.0f};
    shifted[joint::RightArm].rotationDegrees = {-12.0f, 0.0f, 7.0f};
    shifted[rightElbow].rotationDegrees      = {-18.0f, 0.0f, -12.0f};
    shifted[joint::LeftArm].rotationDegrees  = {8.0f, 0.0f, -6.0f};
    shifted[joint::RightLeg].rotationDegrees = {5.0f, 6.0f, -2.0f};
    shifted[joint::LeftLeg].rotationDegrees  = {-3.0f, -2.0f, 2.0f};

    Pose wave;  // right arm up and out; the body leans away to make room
    wave[joint::Body].rotationDegrees     = {-3.0f, -7.0f, -3.0f};
    wave[joint::Head].rotationDegrees     = {-6.0f, 12.0f, 5.0f};
    wave[joint::RightArm].rotationDegrees = {0.0f, 0.0f, 138.0f};
    wave[rightElbow].rotationDegrees      = {0.0f, 0.0f, 22.0f};
    wave[joint::LeftArm].rotationDegrees  = {5.0f, 0.0f, -8.0f};
    wave[leftElbow].rotationDegrees       = {-10.0f, 0.0f, 6.0f};
    wave[joint::RightLeg].rotationDegrees = {-2.0f, -4.0f, 2.0f};
    wave[joint::LeftLeg].rotationDegrees  = {4.0f, 3.0f, -2.0f};

    // Four keys, first and last the same, so the take loops without a seam.
    // `ease::hold` keeps each pose still for most of its segment and then
    // throws it across in what is left -- on twos that lands as two or three
    // frames of movement between two beats of stillness.
    const float duration = 4.0f;
    Track<Pose> poses;
    poses.key(0.0f,             rest,    ease::hold(0.55f));
    poses.key(duration * 0.34f, shifted, ease::hold(0.60f));
    poses.key(duration * 0.66f, wave,    ease::hold(0.50f));
    poses.key(duration,         rest);

    // ---- the scene: everything static, built once
    Scene scene(palette::registry());

    scene.sun.direction = normalize(Vec3{0.55f, 0.62f, -0.40f});
    scene.sun.color = Vec3{1.0f, 0.95f, 0.88f};
    scene.sun.intensity = 3.4f;
    scene.sun.angularRadiusDegrees = 5.0f;

    // The sky is the fill, and on an orbiting shot it is the only fill there
    // can be. Cool against the warm key, so the side the sun misses still has
    // an edge instead of going flat.
    scene.sky.zenith  = Vec3{0.34f, 0.36f, 0.52f};
    scene.sky.horizon = Vec3{0.44f, 0.42f, 0.50f};
    scene.sky.ground  = Vec3{0.26f, 0.23f, 0.28f};
    scene.sky.intensity = 0.85f;
    scene.ambientStrength = 0.85f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.325f, 0.243f, 0.290f});

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(31.0f);
    scene.camera.aperture = 0.010f;

    EntitySet entities;
    Entity figure;
    figure.model = &model;
    figure.skin = &skin;
    figure.position = {0.0f, 0.0f, 0.0f};

    // ---- inspect: the keyed poses, head on, one still each
    //
    // A turntable cannot show you this. The camera angle and the pose advance
    // together, so whichever way the figure is facing when a pose arrives is
    // the only way you ever see it -- and a limb that has swung somewhere
    // wrong is invisible from three quarters of an orbit. These are the same
    // three poses from the front, which is where a bad sign shows up.
    if (inspect) {
        PathSettings probe;
        probe.width = 420;
        probe.height = 560;
        probe.samplesPerPixel = 24;
        probe.maxBounces = 4;
        probe.progress = false;

        scene.camera.lookAt({0.0f, 1.28f, -4.55f}, {0.0f, 1.02f, 0.0f});
        scene.camera.focusOn(Vec3{0.0f, 1.50f, 0.0f});

        const std::pair<const char*, const Pose*> keyed[] = {
            {"rest", &rest}, {"shifted", &shifted}, {"wave", &wave}};

        for (const auto& [label, pose] : keyed) {
            figure.yawDegrees = 0.0f;
            figure.pose = *pose;
            entities.clear();
            entities.add(figure);
            scene.entities = &entities;

            RenderTargets targets;
            renderPath(scene, probe, nullptr, &targets);
            const std::string path = "out/" + name + "_" + label + ".png";
            ToneParams tone;
            tone.curve = Tonemap::ACES;
            if (!pngSave(path, targets.color, tone, &error)) {
                std::printf("save failed: %s\n", error.c_str());
                return 1;
            }
            std::printf("[turntable] wrote %s\n", path.c_str());
        }
        return 0;
    }

    // ---- the take
    Take take;
    // Draft and full quality get their own sequences. They are different
    // pictures at different sizes, and out/<name>_draft.png alongside
    // out/<name>.png is what the rest of the scenes here already do.
    take.name = draft ? name + "_draft" : name;
    take.timing.duration = duration;
    take.timing.fps = 24;
    take.timing.stepEvery = ones ? 1 : 2;

    take.settings.width  = draft ? 420 : 900;
    take.settings.height = draft ? 560 : 1200;
    take.settings.samplesPerPixel = draft ? 24 : 96;
    take.settings.maxBounces = draft ? 4 : 8;

    take.tone.curve = Tonemap::ACES;
    take.scene = &scene;

    take.shot = [&](Scene& s, const Frame& f) {
        // A full turn over the take. The last step lands exactly one step
        // short of 360 degrees, which is where a loop wants it.
        //
        // The turn starts on the face and not behind the head, which is why
        // the Z is negated: an entity looks towards -Z, so the camera that
        // sees its front stands there too. Getting this backwards is not
        // subtle -- the take opens on the back of a skull -- but it is easy,
        // because the obvious `cos` is the wrong sign.
        // The radius is set by the widest moment, not by the resting one. A
        // raised arm swings a long way out in screen space, and at 4.55 the
        // hand came within two pixels of the right edge a third of the way
        // round -- which a still of the pose cannot show you, because the
        // pose is only ever seen from the angle the orbit happens to be at
        // when it arrives. Framing an animated shot means framing its widest
        // frame, and the way to find that one is to measure them all.
        const float angle = f.t01 * kTwoPi;
        const float radius = 5.30f;
        const Vec3 eye{std::sin(angle) * radius, 1.28f, -std::cos(angle) * radius};
        const Vec3 at{0.0f, 1.02f, 0.0f};

        s.camera.lookAt(eye, at);
        s.camera.focusOn(Vec3{0.0f, 1.50f, 0.0f});   // the face, not the middle

        // The figure turns a little against the camera, so the orbit does not
        // read as a model on a plate. A quarter turn over the whole take.
        figure.yawDegrees = degrees(angle) * -0.25f;
        figure.pose = poses.at(f.time);

        // EntitySet::add flattens the pose into world-space boxes there and
        // then, so a new pose needs a new set. This is the rebuild take.hpp
        // warns about.
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
            cel.shadowFloor = 0.20f;
            cel.depthThreshold = 0.32f;
            cel.normalThreshold = 0.28f;
            cel.outlineWidth = draft ? 1 : 2;
            cel.outlineColor = srgbToLinear(Vec3{0.09f, 0.05f, 0.07f});
            cel.outlineOpacity = 0.90f;
            cel.saturation = 1.10f;

            Image frame = celShade(targets, cel);

            GradeSettings grade;
            grade.contrast = 1.04f;
            grade.saturation = 1.05f;
            applyGrade(frame, grade);
            applyVignette(frame, {0.22f, 0.70f, 0.55f});
            return frame;
        };
    }

    TakeStats stats;
    if (!renderTake(take, &stats, &error)) {
        std::printf("[turntable] failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
