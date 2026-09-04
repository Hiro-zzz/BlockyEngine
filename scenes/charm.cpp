// A character standing still, shot the way a person would be: long lens,
// eye level, hands together.
//
// The lens is the whole point, and it is the opposite of scene_lowangle. A
// wide angle close to a two-block figure stretches whatever is nearest the
// camera and shrinks the rest, which on a box model reads as limbs turned
// into planks -- boxes have no anatomy to absorb the distortion. So: a narrow
// field of view from four blocks back. The figure fills the frame because the
// camera is far away and tight, not because it is near and wide.
//
// The pose is built to keep a silhouette. Every limb is turned a few degrees
// out of the plane of the body, because two boxes that touch and share a
// shading band stop being two boxes -- and a character whose arm has merged
// into their side looks broken long before anyone can say why.
//
//   scene_charm             full quality
//   scene_charm draft       small and fast
//   scene_charm plain       the same frame without cel shading, to compare
//   scene_charm eyes        rebuild the painted eyes as geometry
//   scene_charm probe       print the illumination the frame carries, and stop
//   scene_charm inspect     the same pose from the side, to check the arms
//   scene_charm <png>       a different skin
//   scene_charm bg=4E3A46   backdrop colour, sRGB hex
//   scene_charm out=name    write out/<name>.png
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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kDefaultLook = "slate";

// Dusty mauve: a shade lighter than the character's own darks, so a figure
// dressed in near-black still has an edge against it. A backdrop darker than
// the subject turns the subject into a hole.
const Vec3 kDefaultBackdrop{0.341f, 0.251f, 0.302f};

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

// What illumination the frame actually carries, in the units celShade bands.
//
// A cel window has to be fitted to the frame and not to the key: an
// illumination is color/albedo and so already carries the 1/pi of the diffuse
// lobe, which puts a key of 3 at about 0.7. Guessing that factor wrong fits
// the bands to a window the frame never enters, and three steps come back as
// one flat tone.
void reportIllumination(const RenderTargets& targets) {
    std::vector<float> levels;
    for (size_t i = 0; i < targets.depth.size(); ++i) {
        if (targets.depth[i] < 0.0f) continue;  // background carries no shading
        Vec3 albedo = maxv(targets.albedo.data()[i], Vec3{0.02f, 0.02f, 0.02f});
        Vec3 illumination = targets.color.data()[i] / albedo;
        levels.push_back(dot(illumination, Vec3{0.2126f, 0.7152f, 0.0722f}));
    }
    if (levels.empty()) return;
    std::sort(levels.begin(), levels.end());
    auto at = [&](float fraction) { return levels[size_t(fraction * float(levels.size() - 1))]; };
    std::printf("[charm] illumination  p05 %.2f  p25 %.2f  p50 %.2f  p75 %.2f  p95 %.2f  max %.2f\n",
                at(0.05f), at(0.25f), at(0.50f), at(0.75f), at(0.95f), levels.back());
    std::printf("        put the band boundaries in the gaps of that spread.\n");
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    bool plain = false;
    bool rigEyes = false;
    bool probe = false;
    bool inspect = false;
    std::string skinPath;
    std::string outputName = "charm";
    Vec3 backdrop = kDefaultBackdrop;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "plain") plain = true;
        else if (arg == "eyes") rigEyes = true;
        else if (arg == "probe") probe = true;
        else if (arg == "inspect") inspect = true;
        else if (arg.rfind("bg=", 0) == 0) {
            if (!parseColour(arg.substr(3), backdrop)) {
                std::printf("bad colour: %s (want bg=RRGGBB)\n", arg.c_str());
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
        std::printf("[charm] %s\n", note.c_str());
        return 1;
    }
    std::printf("[charm] %s (%s)\n", note.c_str(),
                skin.model() == SkinModel::Slim ? "slim" : "classic");

    EntityModel model = buildPlayerModel(skin);

    const int rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
    const int rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
    const int leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");

    // ---- eyes: painted, unless asked otherwise
    //
    // The rig earns its keep when a character has to look somewhere the head
    // is not pointing. This one is looking straight down the lens, so it buys
    // nothing here -- and it costs: buildEyeRig paints the drawn eyes out and
    // redraws them from a sclera and an iris, which on a face drawn in a style
    // of its own replaces the artist's eye with the scanner's idea of one.
    face::EyeRig rig;
    if (rigEyes) {
        rig = face::buildEyeRig(model, skin);
        std::printf("[charm] eye rig %s\n", rig.built ? "built" : rig.rejection);
    }

    // ---- the pose
    //
    // Signs, from docs/conventions.md: a limb hangs below its pivot, so +X
    // swings it forward towards -Z and +Z swings it towards +X -- outwards for
    // the right arm, across the body for the left. The torso and head rise
    // above their pivots and so take the same sign the other way.
    Pose pose;

    // Weight on one hip: the body leans a few degrees and the head comes back
    // the other way. Two small opposed angles are what separate standing from
    // being stood up.
    pose[joint::Body].rotationDegrees = {-7.0f, 10.0f, 4.0f};
    pose[joint::Head].rotationDegrees = {6.0f, -16.0f, -11.0f};

    // Hands behind the back, and the reason is legibility rather than
    // manners. This costume is nearly black from collar to hem and the hair is
    // nearly black too, so anything held up near the head -- a wave, a hand at
    // the cheek -- is a dark box against a dark box and the pose stops reading
    // at all. Put the arms behind instead and the silhouette is one clean
    // shape with a face on top of it.
    //
    // Signs: a limb hangs below its pivot, so -X swings it backwards to +Z,
    // and +Z swings it towards +X -- outwards for the right arm, across the
    // body for the left. The elbows turn the forearms inwards so the hands
    // meet at the small of the back.
    pose[joint::RightArm].rotationDegrees = {-24.0f, 0.0f, 5.0f};
    pose[rightElbow].rotationDegrees      = {-14.0f, 0.0f, -32.0f};
    pose[joint::LeftArm].rotationDegrees  = {-21.0f, 0.0f, -5.0f};
    pose[leftElbow].rotationDegrees       = {-12.0f, 0.0f, 30.0f};

    // One foot forward and turned in, the other taking the weight. The turned
    // in toe is what reads as shy rather than as at ease.
    pose[joint::RightLeg].rotationDegrees = {6.0f, 7.0f, -2.0f};
    pose[rightKnee].rotationDegrees       = {-6.0f, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees  = {-3.0f, -2.0f, 2.0f};
    pose[leftKnee].rotationDegrees        = {-2.0f, 0.0f, 0.0f};

    if (rig.built) face::gaze(rig, pose, 0.55f, 0.15f);

    // Only one block in it: this scene builds nothing else out of the world.
    BlockRegistry registry;
    Scene scene(registry);

    // ---- a rim light, built out of the one thing the tracer already treats
    // as a light source: an emissive block.
    //
    // A character dressed in near-black has no shadow-side edge against any
    // backdrop dark enough to suit them, and the sun can only be in one place.
    // So a panel of glowing blocks stands behind the figure and off to their
    // left and *above*, outside the camera's cone, raking the side the sun
    // cannot reach. Above for a practical reason: the cone widens with
    // distance, so a panel level with the figure and behind it has to stand
    // absurdly far out to clear the frame -- the first two attempts simply
    // photographed the lamp -- while the top edge of the frame is only a
    // block or so above the head -- which is also where a hair light belongs.
    //
    // It is a studio flag, and it costs nothing the renderer did not already
    // do: LightSet gathers emissive voxel faces and the path tracer samples
    // them explicitly.
    BlockDef lamp;
    lamp.name = "studio_rim";
    lamp.albedo = Vec3{0.0f};
    // Cool, against a warm key. That contrast is doing two jobs at once: it
    // separates the rim from the sunlit side by hue as well as by value, and
    // on a costume this red it keeps the edge from reading as more costume.
    lamp.emission = srgbToLinear(Vec3{0.72f, 0.80f, 1.0f}) * 45.0f;
    const BlockId rimLamp = registry.add(lamp);

    scene.world.fillBox({-3, 5, 1}, {-2, 6, 3}, rimLamp);

    EntitySet entities;
    {
        Entity figure;
        figure.model = &model;
        figure.skin = &skin;
        figure.position = {0.0f, 0.0f, 0.0f};
        figure.yawDegrees = 19.0f;
        figure.pose = pose;
        entities.add(figure);
    }
    scene.entities = &entities;

    // ---- light
    //
    // Key from above and well round to the left, not from behind the lens: a
    // frontal key lands on every face the camera can see at about the same
    // angle, and bands with nothing to separate come back as one tone. From
    // here the fronts and the lit sides sit in different steps and the figure
    // has a light side and a shadow side.
    scene.sun.direction = normalize(Vec3{0.66f, 0.48f, -0.58f});
    scene.sun.color = Vec3{1.0f, 0.95f, 0.90f};
    scene.sun.intensity = 3.6f;
    scene.sun.angularRadiusDegrees = 6.0f;

    scene.sky.zenith  = Vec3{0.42f, 0.38f, 0.46f};
    scene.sky.horizon = Vec3{0.48f, 0.41f, 0.45f};
    scene.sky.ground  = Vec3{0.30f, 0.24f, 0.26f};
    scene.sky.intensity = 0.55f;
    scene.ambientStrength = 0.70f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(backdrop);

    if (!plain) scene.materialStyle = MaterialStyle::matte();

    // ---- camera: eye level, long lens, four blocks back
    //
    // Slightly below the eyes rather than above them, so the figure stands a
    // little over the viewer. The roll is three degrees: enough that the frame
    // is not a passport photo, small enough that nobody reads it as a tilt.
    // `inspect` looks along +X instead: an arm folded behind the back is
    // exactly the thing a front view cannot confirm, because the difference
    // between behind the back and jammed into the hip is invisible from
    // there and obvious from the side.
    const Vec3 eye = inspect ? Vec3{-4.90f, 1.20f, 0.30f} : Vec3{1.18f, 1.24f, -4.86f};
    const Vec3 at  = inspect ? Vec3{0.0f, 1.06f, 0.0f} : Vec3{0.08f, 1.06f, 0.0f};
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(30.5f);
    scene.camera.lookAt(eye, at,
                        inspect ? Vec3{0.0f, 1.0f, 0.0f} : normalize(Vec3{0.052f, 1.0f, 0.0f}));
    scene.camera.aperture = 0.012f;
    scene.camera.focusOn(Vec3{0.0f, 1.55f, 0.0f});   // the face, not the middle

    PathSettings settings;
    settings.width  = draft ? 480 : 1200;
    settings.height = draft ? 600 : 1500;
    settings.samplesPerPixel = draft ? 24 : 112;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = false;

    std::printf("[charm] rendering %dx%d at %d spp%s...\n", settings.width, settings.height,
                settings.samplesPerPixel, plain ? "" : ", cel");

    RenderStats stats;
    RenderTargets targets;
    renderPath(scene, settings, &stats, &targets);
    std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

    if (probe) {
        reportIllumination(targets);
        return 0;
    }

    // Denoise before banding, always: a band is chosen per pixel, so leftover
    // noise puts neighbours on different steps -- speckle with hard edges.
    targets.color = denoise(targets, {});

    Image frame;
    if (plain) {
        frame = targets.color;
    } else {
        CelSettings cel;
        cel.bands = 3;

        // Fitted to what `probe` measured on this scene: a shadow side near
        // 0.10, the fronts and tops around 0.45, the lit sides around 0.67.
        // With an even gamma the boundaries land at a third and two thirds of
        // the window, which drops one into each gap.
        cel.range = 1.15f;
        cel.bandGamma = 1.0f;
        cel.highlightCeiling = 3.0f;

        // A fraction of `range`, not of the frame -- widening the window above
        // raises this floor with it. A shadow at a fifth of the key is the
        // usual ambient shade of hand-drawn cel work; zero would send the
        // whole shadow side to black and take the drawing on it along.
        cel.shadowFloor = 0.19f;

        // Loose enough not to ink a face seen at a glancing angle: depth
        // changes fast per pixel there, and the gradient prediction narrows
        // that without removing it.
        cel.depthThreshold = 0.32f;
        cel.normalThreshold = 0.28f;
        cel.outlineWidth = draft ? 1 : 2;
        cel.outlineColor = srgbToLinear(Vec3{0.09f, 0.05f, 0.07f});
        cel.outlineOpacity = 0.90f;
        cel.saturation = 1.12f;
        frame = celShade(targets, cel);
    }

    GradeSettings grade;
    grade.contrast = 1.05f;
    grade.saturation = 1.06f;
    applyGrade(frame, grade);
    applyVignette(frame, {0.26f, 0.68f, 0.55f});

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const std::string suffix =
        inspect ? "_inspect" : (plain ? "_plain" : (draft ? "_draft" : ""));
    const std::string path = "out/" + outputName + suffix + ".png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[charm] wrote %s\n", path.c_str());
    return 0;
}
