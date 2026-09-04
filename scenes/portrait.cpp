// Character art: cel shaded, on a flat warm backdrop, with rigged eyes.
//
// Everything the engine has learned lately meets here. The material style
// turns every surface matte before a single ray is fired, because a highlight
// is a smooth gradient a few pixels wide and quantising one gives a staircase
// of hard rings. The eyes are found, painted out of the skin and rebuilt as
// geometry. Then the finished illumination is banded and the geometry inked.
//
// There is no world at all -- not even a floor. A cel frame on a flat colour
// wants no horizon and no contact shadow, and a ray that hits nothing already
// comes back as exactly the backdrop.
//
//   scene_portrait             full quality
//   scene_portrait draft       small and fast
//   scene_portrait plain       the same frame without cel shading, to compare
//   scene_portrait <png>         a different skin
//   scene_portrait bg=7EC8E3     backdrop colour, sRGB hex
//   scene_portrait eye=9,12,2,3  place the entity's right eye by hand
//   scene_portrait out=name      write out/<name>.png
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/skins.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultLook = "moss";

// Warm cream. The backdrop and nothing else -- with no world in the scene,
// every ray that misses returns exactly this.
const Vec3 kDefaultBackdrop{0.976f, 0.851f, 0.612f};

// `bg=RRGGBB`, in sRGB as it would be picked out of a reference image.
bool parseColour(const std::string& text, Vec3& out) {
    if (text.size() != 6) return false;
    uint32_t value = 0;
    for (char c : text) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = value * 16 + uint32_t(digit);
    }
    out = {float((value >> 16) & 0xFF) / 255.0f, float((value >> 8) & 0xFF) / 255.0f,
           float(value & 0xFF) / 255.0f};
    return true;
}

// `eye=x,y,w,h` -- where the entity's *right* eye sits in the skin image.
// Only needed when the automatic scan gets it wrong; see face.hpp.
bool parseRect(const std::string& text, SkinRect& out) {
    int v[4] = {0, 0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        size_t comma = text.find(',', start);
        if (i < 3 && comma == std::string::npos) return false;
        std::string field = text.substr(start, comma - start);
        if (field.empty()) return false;
        v[i] = std::atoi(field.c_str());
        start = comma + 1;
    }
    if (v[2] <= 0 || v[3] <= 0) return false;
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    std::string skinPath;
    std::string outputName = "portrait";
    Vec3 backdrop = kDefaultBackdrop;
    SkinRect eyeOverride{};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "plain") plain = true;
        else if (arg.rfind("bg=", 0) == 0) {
            if (!parseColour(arg.substr(3), backdrop)) {
                std::printf("bad colour: %s (want bg=RRGGBB)\n", arg.c_str());
                return 1;
            }
        } else if (arg.rfind("eye=", 0) == 0) {
            if (!parseRect(arg.substr(4), eyeOverride)) {
                std::printf("bad rect: %s (want eye=x,y,w,h)\n", arg.c_str());
                return 1;
            }
        } else if (arg.rfind("out=", 0) == 0) {
            outputName = arg.substr(4);
        } else {
            skinPath = arg;
        }
    }

    Skin skin;
    std::string error;
    std::string note;
    if (!skins::loadOrDraw(skin, skinPath, kDefaultLook, &note)) {
        std::printf("[portrait] %s\n", note.c_str());
        return 1;
    }
    std::printf("[portrait] %s (%s)\n", note.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel model = buildPlayerModel(skin);

    // ---- elbows, so the pose can bend where the reference bends
    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    const int rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    const int leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");

    // ---- eyes
    //
    // Left to itself the scan is exact on every vanilla skin and on most drawn
    // ones. `eye=x,y,w,h` is for the rest: the pale skin this scene defaults to
    // is one where the scan finds the position but stops after the top row, so
    // a three-texel eye comes back one tall and is filed as wide rather than
    // narrow. The scanner is honest about what it found; a caller who can see
    // the render knows better.
    face::EyeRigOptions eyes;
    if (eyeOverride.valid()) eyes.scan.rightEyeOverride = eyeOverride;

    face::EyeRig rig = face::buildEyeRig(model, skin, joint::Head, eyes);
    if (!rig.built) {
        std::printf("[portrait] eye rig failed: %s\n", rig.rejection);
        return 1;
    }
    std::printf("[portrait] %s eyes %dx%d at (%d,%d), joints %d/%d of %zu\n",
                face::shapeName(rig.scan.shape), rig.scan.right.width, rig.scan.right.height,
                rig.scan.right.x, rig.scan.right.y, rig.right, rig.left, model.skeleton.size());

    // ---- the pose
    //
    // Signs, from docs/conventions.md: a limb hangs below its pivot, so +Z
    // swings it towards +X. The raised arm in the reference is on the viewer's
    // right, and the viewer sees +X on their left -- so it is the entity's
    // *left* arm, and swinging it outwards is negative.
    Pose pose;
    pose[joint::Body].rotationDegrees = {-4.0f, -14.0f, 2.0f};
    pose[joint::Head].rotationDegrees = {-2.0f, 10.0f, -3.0f};

    // Left arm: up and out at the shoulder, folded up at the elbow so the fist
    // finishes level with the head.
    pose[joint::LeftArm].rotationDegrees = {-6.0f, 0.0f, -100.0f};
    pose[leftElbow].rotationDegrees      = {-12.0f, 0.0f, -70.0f};

    // Right arm: hanging, turned a little out of the body and softly bent.
    pose[joint::RightArm].rotationDegrees = {-6.0f, 0.0f, 13.0f};
    pose[rightElbow].rotationDegrees      = {24.0f, 6.0f, 8.0f};

    pose[joint::RightLeg].rotationDegrees = {4.0f, 0.0f, 2.0f};
    pose[rightKnee].rotationDegrees       = {-6.0f, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees  = {-3.0f, 0.0f, -3.0f};
    pose[leftKnee].rotationDegrees        = {-4.0f, 0.0f, 0.0f};

    // Look back into the camera. The body, the torso and the head between
    // them turn the face about fifteen degrees towards +X -- the viewer's
    // left -- so the pupils have to travel the other way to meet the lens.
    // Nothing about the head moves to do it.
    face::gaze(rig, pose, -0.9f, -0.1f);

    Scene scene(palette::registry());

    EntitySet entities;
    {
        Entity figure;
        figure.model = &model;
        figure.skin = &skin;
        figure.position = {0.0f, 0.0f, 0.0f};
        figure.yawDegrees = -11.0f;
        figure.pose = pose;
        entities.add(figure);
    }
    scene.entities = &entities;

    // ---- light
    //
    // Cel shading wants light that already reads in a few large steps, so one
    // broad key from the front and a strong even fill. A raking sun would give
    // a beautiful gradient and then be thrown away by the bands.
    scene.sun.direction = normalize(Vec3{0.38f, 0.46f, -0.80f});
    scene.sun.color = Vec3{1.0f, 0.96f, 0.88f};
    scene.sun.intensity = 3.2f;
    scene.sun.angularRadiusDegrees = 8.0f;

    scene.sky.zenith  = Vec3{0.62f, 0.60f, 0.56f};
    scene.sky.horizon = Vec3{0.72f, 0.66f, 0.55f};
    scene.sky.ground  = Vec3{0.55f, 0.50f, 0.42f};
    scene.sky.intensity = 0.78f;
    scene.ambientStrength = 0.9f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(backdrop);

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    // ---- camera: head to hip, portrait format, like the reference
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(30.0f);
    scene.camera.lookAt({-0.06f, 1.50f, -4.10f}, {-0.16f, 1.35f, 0.0f});

    PathSettings settings;
    settings.width  = draft ? 640 : 1280;
    settings.height = draft ? 800 : 1600;
    settings.samplesPerPixel = draft ? 24 : 96;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = false;

    std::printf("[portrait] rendering %dx%d at %d spp%s...\n", settings.width, settings.height,
                settings.samplesPerPixel, plain ? "" : ", cel");

    RenderStats stats;
    RenderTargets targets;
    renderPath(scene, settings, &stats, &targets);
    std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

    // Denoise before banding, always. A band is chosen per pixel, so leftover
    // noise puts neighbours on different steps -- speckle with hard edges,
    // much worse than the noise it came from.
    targets.color = denoise(targets, {});

    Image frame;
    if (plain) {
        frame = targets.color;
    } else {
        CelSettings cel;
        cel.bands = 3;
        cel.range = 1.55f;         // this scene is lit well above unit exposure
        cel.shadowFloor = 0.30f;   // a shade, not a silhouette
        cel.bandGamma = 0.85f;
        cel.depthThreshold = 0.28f;
        cel.normalThreshold = 0.30f;
        cel.outlineWidth = draft ? 1 : 2;
        cel.outlineColor = srgbToLinear(Vec3{0.10f, 0.07f, 0.06f});
        cel.outlineOpacity = 0.92f;
        cel.saturation = 1.10f;
        frame = celShade(targets, cel);
    }

    GradeSettings grade;
    grade.contrast = 1.04f;
    grade.saturation = 1.04f;
    applyGrade(frame, grade);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const std::string suffix = plain ? "_plain" : (draft ? "_draft" : "");
    const std::string path = "out/" + outputName + suffix + ".png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[portrait] wrote %s\n", path.c_str());
    return 0;
}
