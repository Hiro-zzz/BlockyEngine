// Rigging: what a skeleton buys that six loose rotations could not.
//
// Three figures, left to right:
//
//   1. At rest. The baseline.
//   2. Bent at the waist. One rotation, on the torso joint -- and the head
//      and both arms come with it, because they hang off it. With the old
//      flat pose this was impossible: every part turned about its own pivot
//      alone, so a folded torso left its arms standing in the air beside it.
//   3. Rigged for detail. Elbows and knees cut in, a hinged jaw, a hat on its
//      own joint, and a brow attached to the head. Every one of those is a
//      joint that did not exist until the scene asked for it.
//
// The lamp beside them is the same skeleton driving voxel props instead of
// skin boxes -- post, arm and lantern on a three-joint chain.
//
// The skin is drawn here rather than loaded, because a rig demo needs a skin
// that shows the rig: bands exactly where the elbows and knees are cut, and a
// mouth inside the slice the jaw takes.
//
//   scene_rigging          full quality
//   scene_rigging draft    small and fast
//   scene_rigging bare     ignore game assets (only the walls lose texture)
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/prop/prop_rig.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/sprite/font.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/sprite/text.hpp"
#include "engine/world/shapes.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

using RGBA = ImageU8::RGBA;

// ------------------------------------------------------------- the skin
// Laid out by the standard 64x64 unwrap. The interesting parts are the two
// bands and the mouth: the bands sit exactly where addHinge cuts a limb, so a
// bent elbow shows the seam landing where it should, and the mouth sits in
// the bottom three pixels of the face, which is the slice addJaw takes.
Skin drawnSkin() {
    ImageU8 image(64, 64);

    auto fill = [&](int x, int y, int w, int h, RGBA colour) {
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) image.set(x + i, y + j, colour);
        }
    };

    const RGBA skinTone{224, 172, 133, 255};
    const RGBA shirt{62, 96, 150, 255};
    const RGBA sleeve{78, 118, 178, 255};
    const RGBA trousers{58, 58, 74, 255};
    const RGBA band{232, 196, 90, 255};   // the seam marker
    const RGBA cap{176, 62, 58, 255};
    const RGBA dark{46, 34, 30, 255};
    const RGBA white{245, 245, 240, 255};

    // Base layers: one flat block per part.
    fill(0, 0, 32, 16, skinTone);    // head
    fill(16, 16, 24, 16, shirt);     // body
    fill(40, 16, 16, 16, sleeve);    // right arm
    fill(32, 48, 16, 16, sleeve);    // left arm
    fill(0, 16, 16, 16, trousers);   // right leg
    fill(16, 48, 16, 16, trousers);  // left leg

    // Bands across the four limbs, on the row an elbow or a knee cuts.
    // A limb face is twelve rows tall and runs top-down, so the halfway cut
    // lands six rows in.
    fill(40, 25, 16, 2, band);  // right arm
    fill(32, 57, 16, 2, band);  // left arm
    fill(0, 25, 16, 2, band);   // right leg
    fill(16, 57, 16, 2, band);  // left leg

    // The face, on the head's front rect at (8,8). Row 8 is the top of the
    // head, so the bottom three rows -- 13, 14, 15 -- are the jaw.
    fill(9, 10, 6, 1, dark);    // brow line
    fill(9, 11, 2, 2, white);   // right eye as the figure sees it
    fill(13, 11, 2, 2, white);
    fill(10, 11, 1, 2, dark);   // pupils
    fill(13, 11, 1, 2, dark);
    fill(10, 13, 4, 2, dark);   // mouth, inside the jaw slice

    // The hat layer: a cap over the crown and an upper band, leaving the face
    // clear. Everything else on this layer stays transparent and is cut away.
    // Two rows only. Three reached down to model y = 29, and with the outer
    // layer's half-pixel inflate on top of that the brim sat straight across
    // the eyes.
    fill(40, 0, 8, 8, cap);     // top
    for (int x : {32, 40, 48, 56}) fill(x, 8, 8, 2, cap);

    std::vector<uint8_t> png = pngEncode(image);
    Skin skin;
    skin.loadFromPng(png.data(), png.size(), nullptr);
    return skin;
}

// ------------------------------------------------------------- the world
void buildRoom(World& world) {
    world.fillBox({-16, -3, -10}, {16, 0, 10}, palette::Stone);
    shape::replace(world, {-16, 0, -10}, {16, 0, 10}, palette::Stone, palette::Cobblestone);

    // A low plinth for the figures to stand on, so they read against the wall.
    world.fillBox({-9, 1, -2}, {9, 1, 6}, palette::OakPlanks);

    // Back wall with a brick course.
    world.fillBox({-16, 1, 7}, {16, 14, 9}, palette::Cobblestone);
    world.fillBox({-16, 6, 7}, {16, 7, 9}, palette::Bricks);

    // Real light for the lamp: a prop glows but does not illuminate, so the
    // scene puts an actual emitter where the lantern hangs.
    world.set({-5, 7, 5}, palette::Glowstone);
}

// ------------------------------------------------- the rigged prop models
VoxelModel columnModel(int height, Vec3 albedo, float roughness, float metallic) {
    VoxelModel model;
    model.resize({2, height, 2});
    VoxelMaterial material;
    material.albedo = albedo;
    material.roughness = roughness;
    material.metallic = metallic;
    uint16_t slot = model.addMaterial(material);
    for (int y = 0; y < height; ++y) {
        for (int z = 0; z < 2; ++z) {
            for (int x = 0; x < 2; ++x) model.set({x, y, z}, slot);
        }
    }
    return model;
}

VoxelModel lanternModel() {
    VoxelMaterial iron;
    iron.albedo = srgbToLinear(Vec3{0.34f, 0.36f, 0.40f});
    iron.roughness = 0.32f;
    iron.metallic = 1.0f;

    VoxelMaterial glass;
    glass.albedo = srgbToLinear(Vec3{1.0f, 0.85f, 0.52f});
    glass.emission = {16.0f, 10.0f, 3.8f};

    return voxelize::fromLayers(
        {
            {".###.", "#####", "#####", "#####", ".###."},
            {".....", ".#~#.", ".~~~.", ".#~#.", "....."},
            {".....", ".#~#.", ".~~~.", ".#~#.", "....."},
            {".###.", "#####", "#####", "#####", ".###."},
        },
        {{'#', iron}, {'~', glass}});
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false, bare = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        if (std::strcmp(argv[i], "bare") == 0) bare = true;
    }

    Scene scene(palette::registry());
    buildRoom(scene.world);

    AssetSource source;
    BlockTextureLibrary blockTextures;
    Font font;

    bool haveAssets = false;
    if (!bare) {
        std::string jar = AssetSource::findClientJar();
        haveAssets = !jar.empty() && source.open(jar, nullptr);
    }
    if (haveAssets) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
            scene.blockTextures = &blockTextures;
        }
        if (!font.loadFromSource(source, Font::kMinecraftAscii, nullptr)) font.useBuiltin();
    } else {
        font.useBuiltin();
    }
    std::printf("[rigging] block textures %s, font %s\n", scene.blockTextures ? "game" : "flat",
                font.isBuiltin() ? "built-in" : "game");

    Skin skin = drawnSkin();

    // ------------------------------------------------------------ figure 1
    // Untouched, for comparison.
    EntityModel plainModel = buildPlayerModel(skin);

    // ------------------------------------------------------------ figure 2
    // Also untouched: the point is that one rotation on the torso joint moves
    // everything above the waist, which needs no rigging at all -- only the
    // parenting the skeleton already provides.
    EntityModel bentModel = buildPlayerModel(skin);

    // ------------------------------------------------------------ figure 3
    EntityModel riggedModel = buildPlayerModel(skin);

    const int rightElbow = rigging::addHinge(riggedModel, joint::RightArm, "rightElbow");
    const int leftElbow  = rigging::addHinge(riggedModel, joint::LeftArm, "leftElbow");
    const int rightKnee  = rigging::addHinge(riggedModel, joint::RightLeg, "rightKnee");
    const int leftKnee   = rigging::addHinge(riggedModel, joint::LeftLeg, "leftKnee");
    const int jaw        = rigging::addJaw(riggedModel, joint::Head, 3.0f, "jaw");
    const int hat        = rigging::detachLayer(riggedModel, PartHead, LayerOuter, "hat");

    // A brow ridge, cut from a few texels of the face it sits above.
    SkinRect headFront = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
    int browJoint = rigging::attach(
        riggedModel, rigging::attachmentFrom("brow", joint::Head, {-3.0f, 29.5f, -4.6f},
                                             {6.0f, 1.0f, 1.0f},
                                             rigging::subRect(headFront, 1, 2, 6, 1)));

    std::printf("[rigging] joints: plain %zu, rigged %zu (elbows %d/%d, knees %d/%d, jaw %d,"
                " hat %d, brow %d)\n",
                plainModel.skeleton.size(), riggedModel.skeleton.size(), rightElbow, leftElbow,
                rightKnee, leftKnee, jaw, hat, browJoint);

    // ------------------------------------------------------------- poses
    Pose restPose;

    // Signs: a part that rises above its pivot turns the opposite way to one
    // that hangs below it. The torso stands up from the waist, so folding it
    // forward is negative; an arm hangs from its shoulder, so swinging it
    // forward is positive.
    Pose bentPose;
    bentPose[joint::Body].rotationDegrees = {-38.0f, -12.0f, 0.0f};  // fold at the waist
    bentPose[joint::Head].rotationDegrees = {32.0f, 20.0f, 0.0f};    // look back up
    bentPose[joint::RightArm].rotationDegrees = {22.0f, 0.0f, 7.0f};
    bentPose[joint::LeftArm].rotationDegrees = {31.0f, 0.0f, -9.0f};

    Pose riggedPose;
    // Right arm: raised forward at the shoulder, folded up at the elbow.
    riggedPose[joint::RightArm].rotationDegrees = {64.0f, 0.0f, 26.0f};
    riggedPose[rightElbow].rotationDegrees = {76.0f, 0.0f, 0.0f};
    // Left arm: hanging, a little bent.
    riggedPose[joint::LeftArm].rotationDegrees = {-14.0f, 0.0f, -10.0f};
    riggedPose[leftElbow].rotationDegrees = {40.0f, 0.0f, 0.0f};
    // A shallow crouch: hips forward, knees folded back.
    riggedPose[joint::RightLeg].rotationDegrees = {26.0f, 0.0f, 0.0f};
    riggedPose[rightKnee].rotationDegrees = {-46.0f, 0.0f, 0.0f};
    riggedPose[joint::LeftLeg].rotationDegrees = {-14.0f, 0.0f, 0.0f};
    riggedPose[leftKnee].rotationDegrees = {-24.0f, 0.0f, 0.0f};
    // Head up and turned, mouth open, cap knocked askew.
    // The head rises above the neck, so tipping it back is positive; the jaw
    // hangs below its hinge, so dropping it open is negative.
    // The jaw hinges at the back of the skull, so a small angle already swings
    // the chin a long way. At -38 it read as a dropped-off box rather than an
    // open mouth.
    riggedPose[joint::Head].rotationDegrees = {2.0f, 13.0f, 0.0f};
    riggedPose[jaw].rotationDegrees = {-20.0f, 0.0f, 0.0f};
    // Turned about the vertical, not tipped. A tipped cap is the same box as
    // the head inflated half a pixel, so any real tilt swings its brim across
    // the eyes -- which is honest, and useless for showing a face.
    riggedPose[hat].rotationDegrees = {0.0f, 47.0f, 0.0f};

    // ------------------------------------------------------------ placing
    // The set holds pointers, so the models and the skin must outlive it and
    // the vectors must not reallocate.
    EntitySet entities;
    {
        Entity plain;
        plain.model = &plainModel;
        plain.skin = &skin;
        plain.position = {3.4f, 2.0f, 0.0f};
        plain.yawDegrees = -8.0f;
        plain.pose = restPose;
        entities.add(plain);

        Entity bent = plain;
        bent.model = &bentModel;
        bent.position = {0.0f, 2.0f, 0.3f};
        bent.yawDegrees = 6.0f;
        bent.pose = bentPose;
        entities.add(bent);

        Entity rigged = plain;
        rigged.model = &riggedModel;
        rigged.position = {-3.4f, 2.0f, 0.0f};
        rigged.yawDegrees = 16.0f;
        rigged.pose = riggedPose;
        entities.add(rigged);
    }
    scene.entities = &entities;

    // ------------------------------------------------------- the prop rig
    VoxelModel postModel = columnModel(20, srgbToLinear(Vec3{0.22f, 0.23f, 0.26f}), 0.4f, 1.0f);
    VoxelModel armModel = columnModel(9, srgbToLinear(Vec3{0.24f, 0.25f, 0.28f}), 0.4f, 1.0f);
    VoxelModel lantern = lanternModel();

    PropRig lampRig;
    const int lampBase = lampRig.addPart("post", -1, {1.0f, 0.0f, 1.0f}, &postModel);
    // The arm is a column stood on its side: pitched a quarter turn at its
    // own joint, so the rig -- not the model -- decides it points sideways.
    const int lampArm = lampRig.addPart("arm", lampBase, {1.0f, 20.0f, 1.0f}, &armModel,
                                        {0.0f, 0.0f, 0.0f});
    const int lampHead = lampRig.addPart("lantern", lampArm, {1.0f, 29.0f, 1.0f}, &lantern,
                                         {-1.5f, 25.0f, -1.5f});

    Pose lampPose;
    lampPose[lampArm].rotationDegrees = {0.0f, 0.0f, -62.0f};  // swing the arm out
    lampPose[lampHead].rotationDegrees = {0.0f, 0.0f, 62.0f};  // hang the lantern level

    PropSet props;
    PropPlacement lamp;
    lamp.position = {-5.2f, 2.0f, 5.0f};
    lamp.voxelSize = 0.155f;
    lamp.yawDegrees = 34.0f;
    addRigged(props, lampRig, lampPose, lamp);
    props.build();
    scene.props = &props;

    std::printf("[rigging] lamp: %zu parts placed, %llu voxels\n", props.size(),
                (unsigned long long)props.voxelCount());

    // ------------------------------------------------------------- labels
    SpriteSet sprites;
    const Vec3 eye{0.3f, 3.7f, -7.7f};
    {
        TextStyle style;
        style.height = 0.34f;
        style.color = srgbToLinear(Vec3{0.97f, 0.93f, 0.82f});
        style.emission = {0.34f, 0.30f, 0.22f};

        sprites.add(text::facing(font, "at rest", {3.1f, 2.16f, -1.3f}, eye, style));
        sprites.add(text::facing(font, "waist joint", {0.0f, 2.16f, -1.3f}, eye, style));
        sprites.add(text::facing(font, "full rig", {-3.1f, 2.16f, -1.3f}, eye, style));

        TextStyle title = style;
        title.height = 0.85f;
        title.emission = {0.7f, 0.62f, 0.44f};
        sprites.add(text::facing(font, "RIGGING", {0.0f, 7.4f, 6.2f}, eye, title));
    }
    sprites.build();
    scene.sprites = &sprites;

    // -------------------------------------------------------------- camera
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(44.0f);
    scene.camera.lookAt(eye, {0.0f, 3.05f, 0.7f});
    scene.camera.aperture = draft ? 0.0f : 0.07f;
    scene.camera.focusOn({0.0f, 2.9f, 0.1f});

    // --------------------------------------------------------------- light
    // From behind the camera and to the side, so the faces are lit and the
    // limbs still get a shadow side to read their shape by.
    scene.sun.direction = normalize(Vec3{-0.42f, 0.68f, -0.60f});
    scene.sun.color = {1.0f, 0.92f, 0.80f};
    scene.sun.intensity = 8.5f;
    scene.sun.angularRadiusDegrees = 1.6f;

    scene.sky.zenith = {0.16f, 0.26f, 0.54f};
    scene.sky.horizon = {0.62f, 0.66f, 0.80f};
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 0.85f;

    // -------------------------------------------------------------- render
    PathSettings settings;
    settings.width = draft ? 760 : 1500;
    settings.height = draft ? 460 : 910;
    settings.samplesPerPixel = draft ? 28 : 140;
    settings.maxBounces = draft ? 4 : 8;

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    auto render = [&](const char* path) {
        RenderStats stats;
        RenderTargets targets;
        Image frame = renderPath(scene, settings, &stats, &targets);
        frame = denoise(targets, {});

        BloomSettings bloom;
        bloom.threshold = 1.2f;
        bloom.intensity = 0.06f;
        applyBloom(frame, bloom);

        GradeSettings grade;
        grade.contrast = 1.05f;
        grade.saturation = 1.05f;
        grade.temperature = 0.05f;
        applyGrade(frame, grade);

        applyVignette(frame, {});
        applyGrain(frame, 0.008f);

        pngSave(path, frame, tone, nullptr);
        std::printf("[rigging] wrote %s\n", path);
    };

    render(draft ? "out/rigging_draft.png" : "out/rigging.png");

    // A second frame, close on the rigged figure's head. A jaw three pixels
    // deep and a brow one pixel tall are the whole point of the fine rigging,
    // and neither is visible from across the room.
    const Vec3 faceEye{-4.95f, 3.78f, -2.75f};
    const Vec3 faceTarget{-3.42f, 3.56f, 0.05f};

    scene.camera.fovY = radians(30.0f);
    scene.camera.lookAt(faceEye, faceTarget);
    scene.camera.aperture = draft ? 0.0f : 0.03f;
    scene.camera.focusOn(faceTarget);

    // The labels were turned to face the wide camera and are frozen there --
    // that being the whole point of a frozen billboard. Drop them rather than
    // show a row of edge-on slivers.
    scene.sprites = nullptr;

    settings.width = draft ? 520 : 900;
    settings.height = draft ? 520 : 900;
    render(draft ? "out/rigging_face_draft.png" : "out/rigging_face.png");

    return 0;
}
