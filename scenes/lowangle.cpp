// A hero shot from the floor: worm's eye, dutch angle, cel shaded.
//
// The camera sits at ankle height and looks steeply up, so perspective does
// most of the work -- the legs are close and huge, the head is far and small,
// and the figure looms. Two things make that read as a deliberate shot rather
// than a mistake:
//
//   - The tilt. There is no roll parameter on the camera, and there should not
//     be: a camera has an up vector, and rolling it *is* leaning that vector.
//     rollUp() below turns degrees into the hint lookAt() wants.
//   - The lean. A figure standing straight above a low camera shows the
//     underside of its chin and nothing else. The torso tips towards the lens
//     and the head tips down to meet it, which is what puts the face in frame.
//
// Cel, on a flat plum backdrop, with no world at all -- the same reasoning as
// scene_portrait: a ray that hits nothing already comes back as the backdrop,
// and a cel frame wants no horizon and no contact shadow.
//
//   scene_lowangle             full quality
//   scene_lowangle draft       small and fast
//   scene_lowangle plain       the same frame without cel shading, to compare
//   scene_lowangle <png>       a different skin
//   scene_lowangle bg=32212E   backdrop colour, sRGB hex
//   scene_lowangle eye=9,12,2,3  place the entity's right eye by hand
//   scene_lowangle noeyes      skip the eye rig entirely
//   scene_lowangle out=name    write out/<name>.png
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
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultLook = "ochre";

// There used to be a `kDefaultEye` here, and what it was for is worth keeping
// even though the rectangle has gone.
//
// On the skin this scene used to point at, the scan found a two-by-two one
// texel down and to the left of the real eye -- the lash column rather than
// the eye -- and rebuilt a pair of black squares over two drawn ones. Exactly
// the case face.hpp keeps `rightEyeOverride` for: the scanner is honest about
// what it saw, and somebody who can look at the render knows better.
//
// It was tied to that one file and said so, so it went with it. `eye=x,y,w,h`
// is how to say the same thing about whatever skin is actually being rendered.

// Dark plum. Everything the figure is not.
const Vec3 kDefaultBackdrop{0.196f, 0.129f, 0.180f};

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

// The up hint that rolls the camera about its own view axis. Positive leans
// the frame's content clockwise, which is the direction this shot wants.
Vec3 rollUp(Vec3 eye, Vec3 at, float degrees) {
    Vec3 forward = normalize(at - eye);
    return normalize(transformDir(rotateAxis(forward, radians(degrees)), Vec3{0.0f, 1.0f, 0.0f}));
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    bool noEyes = false;
    std::string skinPath;
    std::string outputName = "lowangle";
    Vec3 backdrop = kDefaultBackdrop;
    SkinRect eyeOverride{};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "plain") plain = true;
        else if (arg == "noeyes") noEyes = true;
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
        std::printf("[lowangle] %s\n", note.c_str());
        return 1;
    }
    std::printf("[lowangle] %s (%s)\n", note.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel model = buildPlayerModel(skin);

    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    const int rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    const int leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");

    // ---- eyes
    //
    // Unlike scene_portrait this does not insist: the face is small in this
    // framing, and a skin the scanner will not vouch for is better rendered
    // with its painted eyes than not rendered at all.
    face::EyeRig rig;
    if (!noEyes) {
        face::EyeRigOptions eyes;
        if (eyeOverride.valid()) eyes.scan.rightEyeOverride = eyeOverride;
        rig = face::buildEyeRig(model, skin, joint::Head, eyes);
        if (rig.built) {
            std::printf("[lowangle] %s eyes %dx%d at (%d,%d), joints %d/%d of %zu\n",
                        face::shapeName(rig.scan.shape), rig.scan.right.width,
                        rig.scan.right.height, rig.scan.right.x, rig.scan.right.y, rig.right,
                        rig.left, model.skeleton.size());
        } else {
            std::printf("[lowangle] no eye rig (%s) -- painted eyes it is\n", rig.rejection);
        }
    }

    // ---- the pose
    //
    // Signs, from docs/conventions.md and the inset in api-rig.md: a limb
    // hangs *below* its pivot, so +X swings it forward to -Z. The torso and
    // the head rise *above* theirs, so for them the same sign leans back --
    // which is why both of the numbers that tip this figure towards the lens
    // are negative.
    Pose pose;
    pose[joint::Body].rotationDegrees = {-5.0f, 10.0f, 3.0f};
    pose[joint::Head].rotationDegrees = {-8.0f, -20.0f, 9.0f};

    // Arms hang, turned a little out of the body and trailing behind, with a
    // soft bend at the elbow. Straight arms read as a doll from this angle.
    pose[joint::RightArm].rotationDegrees = {-6.0f, 0.0f, 7.0f};
    pose[rightElbow].rotationDegrees      = {10.0f, 0.0f, 4.0f};
    pose[joint::LeftArm].rotationDegrees  = {-9.0f, 0.0f, -9.0f};
    pose[leftElbow].rotationDegrees       = {12.0f, 0.0f, -4.0f};

    // Weight on the near leg, the other trailing: enough asymmetry that the
    // silhouette is not two parallel columns.
    pose[joint::RightLeg].rotationDegrees = {5.0f, 0.0f, 2.0f};
    pose[rightKnee].rotationDegrees       = {-7.0f, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees  = {-6.0f, 0.0f, -3.0f};
    pose[leftKnee].rotationDegrees        = {-3.0f, 0.0f, 0.0f};

    // Down into the lens. The head is already tipped that way; this is the
    // last few degrees, and it costs no head movement at all.
    if (rig.built) face::gaze(rig, pose, 0.35f, -0.65f);

    Scene scene(palette::registry());

    EntitySet entities;
    {
        Entity figure;
        figure.model = &model;
        figure.skin = &skin;
        figure.position = {0.0f, 0.0f, 0.0f};
        figure.yawDegrees = 10.0f;
        figure.pose = pose;
        entities.add(figure);
    }
    scene.entities = &entities;

    // ---- light
    //
    // Key from the upper left of frame -- which is +X, since the viewer sees
    // +X on their left -- and well in front, so the lit side is the side
    // turned towards us. Broad, because cel shading throws away the gradient
    // of a hard key and keeps only where the band falls.
    // Below the horizon, which is not a mistake. From ankle height most of
    // what the camera can see is *underside* -- the bottoms of the arms, of
    // the skirt, of the chin -- and a key from above leaves every one of them
    // on ambient alone. Measured, that was three quarters of the figure
    // sitting at an illumination of 0.07 while the bands were being fitted to
    // a window ten times wider. A low shot wants a low key.
    scene.sun.direction = normalize(Vec3{0.78f, -0.30f, -0.55f});
    scene.sun.color = Vec3{1.0f, 0.93f, 0.90f};
    scene.sun.intensity = 1.15f;
    scene.sun.angularRadiusDegrees = 7.0f;

    // A cool plum ambient, dim: the frame is meant to be low key, and the
    // shadow side lives on this rather than on the sun.
    scene.sky.zenith  = Vec3{0.26f, 0.20f, 0.34f};
    scene.sky.horizon = Vec3{0.33f, 0.24f, 0.36f};
    // Warm, so the undersides the low camera keeps showing pick up a hint of
    // bounce instead of going flat violet.
    scene.sky.ground  = Vec3{0.26f, 0.15f, 0.16f};
    scene.sky.intensity = 0.25f;
    scene.ambientStrength = 0.25f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(backdrop);

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    // ---- camera: ankle height, steeply up, and rolled
    const Vec3 eye{0.22f, 0.17f, -1.90f};
    const Vec3 at{0.00f, 1.40f, 0.05f};
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(54.0f);
    scene.camera.lookAt(eye, at, rollUp(eye, at, -11.0f));

    PathSettings settings;
    settings.width  = draft ? 512 : 1400;
    settings.height = draft ? 512 : 1400;
    settings.samplesPerPixel = draft ? 24 : 96;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = false;

    std::printf("[lowangle] rendering %dx%d at %d spp%s...\n", settings.width, settings.height,
                settings.samplesPerPixel, plain ? "" : ", cel");

    RenderStats stats;
    RenderTargets targets;
    renderPath(scene, settings, &stats, &targets);
    std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

    // Denoise before banding, always: a band is chosen per pixel, so leftover
    // noise puts neighbours on different steps.
    targets.color = denoise(targets, {});

    Image frame;
    if (plain) {
        frame = targets.color;
    } else {
        CelSettings cel;
        cel.bands = 3;
        // Measured against this scene's own light rather than guessed. The
        // key is deliberately modest: this skin is nearly white, and under a
        // key of four every band lands past 1.0, where the tonemap squeezes
        // them back together -- three bands go in and one flat white comes
        // out. Exposure has to leave the frame room to have steps in.
        // The three clusters this lighting actually produces, measured: the
        // undersides at 0.11, the fronts at 0.20, the lit sides at 0.29. (An
        // illumination is color/albedo and so carries the 1/pi of the diffuse
        // lobe -- a key of 1.15 lands at 0.29, not at 1.15, and a window fitted
        // to the key rather than to the frame misses by that factor.) With an
        // Three steps at 0.09, 0.30 and 0.50, with the boundaries between
        // them landing in the two gaps of that measurement.
        //
        // The key is deliberately modest for the same reason the window is
        // narrow: this skin is nearly white, and a brighter one pushes every
        // band past 1.0 where the tonemap squeezes them back together --
        // four steps go in and one flat cut-out comes out.
        cel.range = 0.50f;
        cel.bandGamma = 1.0f;
        cel.highlightCeiling = 3.0f;
        // A fraction of `range`, not of the frame: widening the window above
        // raises this floor with it, and a floor above the scene's own
        // ambient makes the shadows brighter than the light that cast them.
        cel.shadowFloor = 0.18f;
        // Loosened back off 0.20, which inked a hatch of thin diagonals
        // across the arms: a face seen at a glancing angle changes depth fast
        // enough per pixel to look like an edge, and the gradient prediction
        // only narrows that, it does not remove it.
        cel.depthThreshold = 0.32f;
        cel.normalThreshold = 0.28f;
        cel.outlineWidth = draft ? 1 : 2;
        cel.outlineColor = srgbToLinear(Vec3{0.07f, 0.05f, 0.07f});
        cel.outlineOpacity = 0.92f;
        cel.saturation = 1.12f;
        frame = celShade(targets, cel);
    }

    GradeSettings grade;
    grade.exposure = 0.92f;
    grade.contrast = 1.08f;
    grade.saturation = 1.06f;
    grade.temperature = -0.06f;   // the plum backdrop reads warmer than it is
    applyGrade(frame, grade);

    // The reference falls off into its corners; with no world in the scene
    // there is nothing to do that but the vignette.
    applyVignette(frame, {0.30f, 0.62f, 0.55f});

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const std::string suffix = plain ? "_plain" : (draft ? "_draft" : "");
    const std::string path = "out/" + outputName + suffix + ".png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[lowangle] wrote %s\n", path.c_str());
    return 0;
}
