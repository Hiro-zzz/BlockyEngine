// Eyes given joints: found, painted out, rebuilt as geometry.
//
// Three heads of the same character, glancing left, straight ahead and right.
// Nothing about the head moves between them -- only two joints that did not
// exist until face::buildEyeRig cut them in.
//
// The glance is a *translation*, not a rotation. An eye is one or two pixels
// across; turning it about the head's pivot carries it off the cheek entirely,
// while sliding it along the face is what a glance actually looks like.
//
//   scene_eyes                 full quality
//   scene_eyes draft           small and fast
//   scene_eyes <png>           a different skin
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/face.hpp"
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

const char* kDefaultSkin = "C:/Users/yueiw/Downloads/d22e8cd1208f2767.png";
const Vec3 kBackdrop{0.245f, 0.230f, 0.275f};

bool loadSkinFile(const std::string& path, Skin& skin, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    std::string skinPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else skinPath = argv[i];
    }
    if (skinPath.empty()) skinPath = kDefaultSkin;

    std::string error;
    Skin source;
    if (!loadSkinFile(skinPath, source, &error)) {
        std::printf("could not load %s: %s\n", skinPath.c_str(), error.c_str());
        return 1;
    }

    // What the scanner made of the face, before anything is changed.
    face::EyeScan preview = face::scanFace(source);
    if (!preview.found) {
        std::printf("[eyes] no eyes found: %s\n", preview.rejection);
        std::printf("       pass FaceScanOptions::rightEyeOverride to place them by hand.\n");
        return 1;
    }
    std::printf("[eyes] %s eyes, %dx%d at (%d,%d), confidence %.2f\n",
                face::shapeName(preview.shape), preview.right.width, preview.right.height,
                preview.right.x, preview.right.y, preview.confidence);
    std::printf("       sclera %02X%02X%02X  iris %02X%02X%02X  fill %02X%02X%02X\n",
                preview.sclera.r, preview.sclera.g, preview.sclera.b, preview.iris.r,
                preview.iris.g, preview.iris.b, preview.surround.r, preview.surround.g,
                preview.surround.b);

    // ---- the studio, as in scene_duo: one floor, matched to the backdrop.
    BlockRegistry registry;
    BlockDef floorDef;
    floorDef.name = "studio_floor";
    floorDef.albedo = srgbToLinear(kBackdrop) * 0.55f;
    const BlockId studioFloor = registry.add(floorDef);

    Scene scene(registry);
    scene.world.fillBox({-30, -1, -30}, {30, -1, 30}, studioFloor);

    // ---- three characters, three glances
    //
    // Each needs its own skin: buildEyeRig repaints the image, and the three
    // would otherwise share one and fight over it.
    struct Glance {
        const char* label;
        float eyeShift;   // model pixels, +X is the entity's own right
        float yaw;
    };
    const Glance glances[] = {
        {"left",   -0.85f, -13.0f},
        {"ahead",   0.00f,   4.0f},
        {"right",   0.85f,  16.0f},
    };
    const size_t count = std::size(glances);

    std::vector<Skin> skins(count);
    std::vector<EntityModel> models(count);
    EntitySet entities;

    for (size_t i = 0; i < count; ++i) {
        skins[i] = source;
        models[i] = buildPlayerModel(skins[i]);

        face::EyeRig rig = face::buildEyeRig(models[i], skins[i]);
        if (!rig.built) {
            std::printf("  %s: rig failed (%s)\n", glances[i].label, rig.rejection);
            return 1;
        }

        Pose pose;
        // Both eyes travel together, and both by the same amount: they are
        // parented to the head, so this is a glance and not a squint.
        face::gaze(rig, pose, glances[i].eyeShift, 0.0f);
        // A little life: the head tips a shade the other way from the glance.
        pose[joint::Head].rotationDegrees = {-3.0f, glances[i].eyeShift * 4.0f, 0.0f};

        Entity entity;
        entity.model = &models[i];
        entity.skin = &skins[i];
        entity.position = {(1.0f - float(i)) * 0.98f, 0.0f, 0.0f};
        entity.yawDegrees = glances[i].yaw;
        entity.pose = pose;
        entities.add(entity);

        std::printf("  %-6s eye joints %d/%d, %zu joints total\n", glances[i].label, rig.right,
                    rig.left, models[i].skeleton.size());
    }
    scene.entities = &entities;

    // ---- light: soft key from the front left, cool fill
    scene.sun.direction = normalize(Vec3{0.42f, 0.55f, -0.72f});
    scene.sun.color = Vec3{1.0f, 0.97f, 0.94f};
    scene.sun.intensity = 4.4f;
    scene.sun.angularRadiusDegrees = 6.0f;

    scene.sky.zenith  = Vec3{0.26f, 0.25f, 0.32f};
    scene.sky.horizon = Vec3{0.34f, 0.32f, 0.38f};
    scene.sky.ground  = Vec3{0.20f, 0.19f, 0.24f};
    scene.ambientStrength = 1.0f;

    scene.overrideBackground = true;
    scene.background = srgbToLinear(kBackdrop);

    // ---- camera: right up at the heads. A two-pixel eye is invisible from
    // anywhere else -- the same lesson the jaw taught in scene_rigging.
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(17.0f);
    scene.camera.lookAt({0.0f, 1.76f, -4.30f}, {0.0f, 1.75f, 0.0f});
    scene.camera.aperture = 0.02f;
    scene.camera.focusOn({0.0f, 1.75f, 0.0f});

    PathSettings settings;
    settings.width  = draft ? 900 : 1800;
    settings.height = draft ? 400 : 800;
    settings.samplesPerPixel = draft ? 24 : 96;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = false;

    std::printf("[eyes] rendering %dx%d at %d spp...\n", settings.width, settings.height,
                settings.samplesPerPixel);

    RenderStats stats;
    RenderTargets targets;
    renderPath(scene, settings, &stats, &targets);
    std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

    Image frame = denoise(targets, {});

    GradeSettings grade;
    grade.contrast = 1.05f;
    grade.saturation = 1.05f;
    applyGrade(frame, grade);
    applyVignette(frame, {0.14f, 0.75f, 0.45f});

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = draft ? "out/eyes_draft.png" : "out/eyes.png";
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[eyes] wrote %s\n", path);
    return 0;
}
