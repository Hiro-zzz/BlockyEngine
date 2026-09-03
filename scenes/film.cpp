// ПОСЛЕДНИЙ СВЕТ -- a five-minute silent film in four sets.
//
// Four characters, none of whom speaks: this rig has no jaw in it, so the
// picture has to carry everything, and the cards carry the rest. That is not
// a workaround. Silent film solved this problem a century ago and its answer
// -- stage the beat, then title it -- suits a blocky character better than
// lip sync ever would.
//
// The shot list is at the bottom of this file. Everything above it is the
// vocabulary the shots are written in: the sets, the cast, the props and the
// two or three lighting states each place has.
//
//   scene_film draft         640x360, 24 spp -- the whole film in ~20 minutes
//   scene_film               1280x720, 96 spp -- overnight
//   scene_film probe         one still per shot, for checking staging
//   scene_film only=eyes     re-render one shot (repeatable)
//   scene_film assemble      build the mp4 from frames already on disk
//   scene_film plain         no cel shading
#include "engine/anim/ease.hpp"
#include "engine/anim/track.hpp"
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/platform/window.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/render/gpu/denoise_gpu.hpp"
#include "engine/render/gpu/wavefront_gpu.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/sprite/scatter.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/sprite/text.hpp"
#include "scenes/common/palette.hpp"

#include "common/film/cast.hpp"
#include "common/film/reel.hpp"
#include "common/film/sets.hpp"
#include "common/film/type.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;
using namespace film;

namespace {

// ---------------------------------------------------------------- the cast
//
// Held as named members rather than in a vector: an Entity keeps bare
// pointers into these, so they must not move once a shot has taken one.
struct Cast {
    Figure crew;    // the one on the night shift
    Figure clerk;   // the one who comes in last
    Figure stripe;  // the one already sitting there
    Figure white;   // the one at the end of the road

    bool load() {
        const std::string dir = "C:/Users/yueiw/Downloads/";
        bool ok = true;
        ok &= crew.load(dir + "da14781977559b10.png", "crew");
        ok &= clerk.load(dir + "59e4706ab7ec90e7.png", "clerk");
        ok &= stripe.load(dir + "7983af27cf866d82.png", "stripe");
        ok &= white.load(dir + "dcbbff5cb0c44e79.png", "white");
        return ok;
    }
};

// --------------------------------------------------------------- the props
//
// Both are drawn here rather than pulled from the game, because both have to
// exist whether or not Minecraft is installed -- one of them is the object
// the whole second half turns on.

// A lidded cup. Sixteen voxels to a block, so nine tall is a little over half
// a block: the right size to sit on a counter without becoming furniture.
VoxelModel buildCup() {
    VoxelMaterial body;
    body.albedo = srgbToLinear(Vec3{0.86f, 0.20f, 0.18f});
    body.roughness = 0.55f;

    VoxelMaterial lid;
    lid.albedo = srgbToLinear(Vec3{0.93f, 0.92f, 0.90f});
    lid.roughness = 0.45f;

    VoxelMaterial straw;
    straw.albedo = srgbToLinear(Vec3{0.95f, 0.86f, 0.25f});
    straw.roughness = 0.40f;

    const std::vector<std::string> round = {".ccc.", "ccccc", "ccccc", "ccccc", ".ccc."};
    const std::vector<std::string> cap   = {"lllll", "lllll", "lllll", "lllll", "lllll"};
    const std::vector<std::string> stem  = {".....", ".....", "..s..", ".....", "....."};

    std::vector<std::vector<std::string>> layers;
    for (int i = 0; i < 7; ++i) layers.push_back(round);
    layers.push_back(cap);
    for (int i = 0; i < 4; ++i) layers.push_back(stem);

    return voxelize::fromLayers(layers, {{'c', body}, {'l', lid}, {'s', straw}});
}

// The thing left behind on the table. Emissive, and the only magenta in the
// diner -- which is the whole job of it.
VoxelModel buildShard() {
    VoxelMaterial core;
    core.albedo = srgbToLinear(Vec3{0.92f, 0.62f, 0.90f});
    core.emission = srgbToLinear(Vec3{1.00f, 0.36f, 0.88f}) * 5.0f;
    core.roughness = 0.25f;

    const std::vector<std::string> waist = {".#.", "###", ".#."};
    const std::vector<std::string> full  = {"###", "###", "###"};
    const std::vector<std::string> tip   = {"...", ".#.", "..."};

    return voxelize::fromLayers({waist, full, full, waist, tip, tip}, {{'#', core}});
}

// ------------------------------------------------------------- the buffers
//
// One set of each, cleared and refilled by every shot. They live out here
// because the scene only ever holds pointers at them.
struct Stage {
    EntitySet entities;
    PropSet   props;
    SpriteSet sprites;

    void begin() {
        entities.clear();
        props.clear();
        sprites.clear();
    }

    void finish(Scene& scene) {
        props.build();
        sprites.build();
        scene.entities = entities.empty() ? nullptr : &entities;
        scene.props = &props;
        scene.sprites = &sprites;
    }
};

// ------------------------------------------------------------- lighting
//
// Each set has two or three states and the shots name one. Written as
// functions rather than stored on the Scene because a shot must set every
// one of these every frame -- the shot before it left the room lit for
// something else.

// Night outside: no sun to speak of, a cold sky, and everything that reads
// comes from the windows and the sign.
void lightNightExterior(Scene& scene) {
    // A moon, not a sun: high, cold and weak. It is not there to light the
    // scene -- the windows and the sign do that -- but without it the ground
    // outside the pool of lamplight is a flat black with no horizon in it,
    // and the walkers stop having anything to be silhouetted against.
    scene.sun.direction = normalize(Vec3{0.30f, 0.86f, 0.40f});
    scene.sun.color = Vec3{0.55f, 0.66f, 1.00f};
    scene.sun.intensity = 0.85f;
    scene.sun.angularRadiusDegrees = 2.2f;

    scene.sky.zenith  = srgbToLinear(Vec3{0.045f, 0.058f, 0.115f});
    scene.sky.horizon = srgbToLinear(Vec3{0.105f, 0.118f, 0.180f});
    scene.sky.ground  = srgbToLinear(Vec3{0.018f, 0.018f, 0.026f});
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 1.0f;

    scene.overrideBackground = false;
}

// Night inside: the ceiling lamps do the work, so the sky barely matters.
void lightNightInterior(Scene& scene) {
    lightNightExterior(scene);
    scene.sun.intensity = 0.30f;
    scene.ambientStrength = 0.45f;
}

// The shrine, before and after. `dawn` in [0,1] runs the sun up over the
// eastern ridge, which is at +X.
void lightShrine(Scene& scene, float dawn) {
    const float d = saturate(dawn);
    const float e = ease::smooth(d);

    scene.sun.direction = normalize(lerp(Vec3{0.30f, 0.90f, 0.30f}, Vec3{0.94f, 0.20f, 0.10f}, e));
    scene.sun.color = lerp(Vec3{0.55f, 0.66f, 1.00f}, Vec3{1.00f, 0.72f, 0.42f}, e);
    scene.sun.intensity = lerp(0.28f, 9.5f, e * e);
    scene.sun.angularRadiusDegrees = lerp(1.6f, 0.9f, e);

    scene.sky.zenith  = lerp(srgbToLinear(Vec3{0.020f, 0.028f, 0.070f}),
                             srgbToLinear(Vec3{0.16f, 0.30f, 0.60f}), e);
    scene.sky.horizon = lerp(srgbToLinear(Vec3{0.060f, 0.055f, 0.100f}),
                             srgbToLinear(Vec3{0.95f, 0.62f, 0.38f}), e);
    scene.sky.ground  = lerp(srgbToLinear(Vec3{0.010f, 0.010f, 0.014f}),
                             srgbToLinear(Vec3{0.20f, 0.16f, 0.12f}), e);
    scene.sky.intensity = lerp(1.0f, 1.35f, e);
    scene.ambientStrength = lerp(0.85f, 1.0f, e);
    scene.overrideBackground = false;
}

// ------------------------------------------------------------------- cards
//
// An intertitle is a label in an empty world. There is nothing to light it,
// so it is emissive -- which also means fading it is a single multiplier and
// needs no compositing step anywhere.
struct CardLook {
    float height = 0.58f;
    Vec3  colour{1.0f, 0.96f, 0.90f};
    float distance = 6.5f;
};

float cardAlpha(float local, float seconds, float in = 1.1f, float out = 0.9f) {
    const float up = ease::smooth(saturate(local / in));
    const float down = 1.0f - ease::smooth(saturate((local - (seconds - out)) / out));
    return up * down;
}

void playCard(Scene& scene, Stage& stage, const Font& font, const std::string& utf8,
              const Beat& beat, float seconds, const CardLook& look = {}) {
    stage.begin();

    scene.sun.intensity = 0.0f;
    scene.sky.intensity = 0.0f;
    scene.ambientStrength = 0.0f;
    scene.overrideBackground = true;
    scene.background = Vec3{0.0f};
    scene.materialStyle = MaterialStyle{};

    TextStyle style;
    style.height = look.height;
    style.color = Vec3{0.0f};                       // unlit: the glow is all of it
    style.emission = srgbToLinear(look.colour) * 2.6f * cardAlpha(beat.local, seconds);
    style.align = TextStyle::Align::Center;
    style.lineSpacing = 1.45f;

    stage.sprites.add(text::build(font, type::cp1251(utf8), Vec3{0.0f, 0.0f, 0.0f}, style));

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(40.0f);
    scene.camera.aperture = 0.0f;
    scene.camera.lookAt({0.0f, 0.0f, look.distance}, {0.0f, 0.0f, 0.0f});

    stage.finish(scene);
}

// --------------------------------------------------------------- utilities

// A camera that eases from one setup to another across the shot. Most of the
// moves in this film are this and nothing more: a blocky picture holds up
// better under a slow single move than under anything busier.
void glide(Scene& scene, const Beat& beat, Vec3 eyeFrom, Vec3 atFrom, Vec3 eyeTo, Vec3 atTo,
           float fovDegrees, float aperture = 0.0f, const ease::Curve& curve = &ease::smooth) {
    const float u = curve ? curve(beat.t01) : beat.t01;
    const Vec3 eye = lerp(eyeFrom, eyeTo, u);
    const Vec3 at = lerp(atFrom, atTo, u);

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(fovDegrees);
    scene.camera.aperture = aperture;
    scene.camera.lookAt(eye, at);
    if (aperture > 0.0f) scene.camera.focusOn(at);
}

void hold(Scene& scene, Vec3 eye, Vec3 at, float fovDegrees, float aperture = 0.0f) {
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(fovDegrees);
    scene.camera.aperture = aperture;
    scene.camera.lookAt(eye, at);
    if (aperture > 0.0f) scene.camera.focusOn(at);
}

} // namespace

// =========================================================================
int main(int argc, char** argv) {
    bool draft = false;
    bool probe = false;
    bool plain = false;
    bool assembleOnly = false;
    std::vector<std::string> only;
    std::string name = "film";
    std::string timeShot;
    bool bare = false;
    bool onGpu = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "draft") draft = true;
        else if (arg == "probe") probe = true;
        else if (arg == "plain") plain = true;
        else if (arg == "assemble") assembleOnly = true;
        else if (arg.rfind("only=", 0) == 0) only.push_back(arg.substr(5));
        else if (arg.rfind("time=", 0) == 0) timeShot = arg.substr(5);
        else if (arg == "bare") bare = true;
        else if (arg == "gpu") onGpu = true;
        else if (arg.rfind("out=", 0) == 0) name = arg.substr(4);
        else {
            std::printf("[film] unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }

    // ---------------------------------------------------------------- setup
    const Palette pal = registerBlocks();

    Cast cast;
    if (!cast.load()) return 1;

    Font font = type::buildFont();
    std::printf("[film] cards set in a drawn Cyrillic face\n");

    AssetSource source;
    const std::string jar = AssetSource::findClientJar();
    const bool haveAssets = !jar.empty() && source.open(jar, nullptr);

    // All four sets share one palette -- `registerBlocks` above has already
    // added the film's own blocks to it, and a shot cuts between sets.
    BlockRegistry& blocks = palette::registry();
    Scene voidSet(blocks), dinerSet(blocks), roadSet(blocks), shrineSet(blocks);
    diner::build(dinerSet.world, pal);
    road::build(roadSet.world, pal);
    shrine::build(shrineSet.world, pal);
    std::printf("[film] sets built: diner %llu blocks, road %llu, shrine %llu\n",
                (unsigned long long)dinerSet.world.blockCount(),
                (unsigned long long)roadSet.world.blockCount(),
                (unsigned long long)shrineSet.world.blockCount());

    BlockTextureLibrary textures;
    if (haveAssets && textures.load(source, dinerSet.world.registry(), palette::minecraftRules(), nullptr)) {
        dinerSet.blockTextures = &textures;
        roadSet.blockTextures = &textures;
        shrineSet.blockTextures = &textures;
        std::printf("[film] textures: %zu images\n", textures.textureCount());
    } else {
        std::printf("[film] no game assets -- flat palette\n");
    }

    const VoxelModel cup = buildCup();
    const VoxelModel shard = buildShard();

    Stage stage;

    // ------------------------------------------------------------ the reel
    Reel reel;
    reel.name = draft ? name + "_draft" : name;
    reel.fps = 24;
    reel.stepEvery = 2;
    // 64 spp and the denoiser, not 96 and hope. Measured on this film: the
    // draft runs about 4.5 s a frame, and almost none of that is resolution
    // -- it is that every set here has emissive geometry in it and the tracer
    // samples every emissive face explicitly on every bounce. Twenty-four
    // ceiling lamps cost more than the pixels do, so the settings that
    // actually move the clock are spp and bounces.
    reel.settings.width  = draft ? 640 : 1280;
    reel.settings.height = draft ? 360 : 720;
    reel.settings.samplesPerPixel = draft ? 24 : 64;
    reel.settings.maxBounces = draft ? 4 : 6;
    reel.tone.curve = Tonemap::ACES;
    reel.tone.exposure = 1.0f;
    reel.only = only;
    reel.sets = {&voidSet, &dinerSet, &roadSet, &shrineSet};

    enum Set { VOID = 0, DINER = 1, ROAD = 2, SHRINE = 3 };

    auto shot = [&](const char* shotName, int set, float seconds,
                    std::function<void(Scene&, const Beat&)> play) {
        Shot s;
        s.name = shotName;
        s.set = set;
        s.seconds = seconds;
        s.play = std::move(play);
        reel.shots.push_back(std::move(s));
    };
    auto card = [&](const char* shotName, float seconds, const char* utf8) {
        Shot s;
        s.name = shotName;
        s.set = VOID;
        s.seconds = seconds;
        s.card = true;
        const std::string content = utf8;
        s.play = [&, content, seconds](Scene& scene, const Beat& beat) {
            playCard(scene, stage, font, content, beat, seconds);
        };
        reel.shots.push_back(std::move(s));
    };

    // =====================================================================
    // ЧАСТЬ ПЕРВАЯ -- ЗАКРЫТИЕ
    // =====================================================================

    card("title", 9.0f, "ПОСЛЕДНИЙ\nСВЕТ");

    // A lit box a long way off, and nothing else. The push-in is slow enough
    // that it reads as the camera noticing rather than as the camera moving.
    shot("plain", DINER, 12.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);
        glide(scene, b, {-2.0f, 9.0f, -46.0f}, {1.0f, 2.4f, -4.0f},
              {0.0f, 5.2f, -31.0f}, {1.0f, 1.8f, -5.0f}, 36.0f);
        stage.finish(scene);
    });

    shot("front", DINER, 7.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);
        glide(scene, b, {-2.0f, 3.4f, -21.0f}, {2.0f, 1.6f, -6.0f},
              {0.0f, 2.6f, -17.0f}, {2.5f, 1.4f, -6.0f}, 38.0f, 0.02f);
        stage.finish(scene);
    });

    // Inside. The crew is the only thing moving in a room built for a crowd.
    shot("inside_wide", DINER, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 4.0f);
        crew.pose = poseAdd(pose::wipe(cast.crew.rig, b.global * 0.42f),
                            pose::breathe(b.global, 5.0f, 1.0f, 0.6f));
        cast.crew.look(crew.pose, 0.0f, -0.45f);
        stage.entities.add(crew);

        glide(scene, b, {0.6f, 2.35f, -4.6f}, {-1.6f, 1.25f, 3.4f},
              {-0.2f, 2.05f, -2.2f}, {-1.9f, 1.30f, 3.8f}, 44.0f);
        stage.finish(scene);
    });

    shot("wiping", DINER, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        // The wipe slows to a stop over the shot: the phase is integrated
        // from a rate that decays, not read off the clock, so it never jumps.
        const float rate = 0.42f * (1.0f - ease::smooth(saturate((b.local - 4.0f) / 4.5f)));
        const float phase = b.global * 0.42f - 0.5f * 0.42f *
                            std::max(0.0f, b.local - 4.0f) *
                            ease::smooth(saturate((b.local - 4.0f) / 4.5f));
        (void)rate;

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 10.0f);
        Pose p = poseAdd(pose::wipe(cast.crew.rig, phase),
                         pose::breathe(b.global, 5.0f, 1.0f, 0.7f));
        cast.crew.look(p, 0.15f, -0.5f);
        crew.pose = p;
        stage.entities.add(crew);

        glide(scene, b, {-4.6f, 1.85f, 0.4f}, {-2.0f, 1.25f, 4.2f},
              {-4.1f, 1.80f, 1.0f}, {-2.0f, 1.28f, 4.2f}, 40.0f, 0.03f);
        stage.finish(scene);
    });

    card("clock", 5.0f, "23:47");

    // He straightens up and looks at the window. Nothing is out there yet.
    shot("lookup", DINER, 8.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Track<Pose> t;
        t.key(0.0f, poseAdd(pose::wipe(cast.crew.rig, 0.0f), Pose{}), ease::hold(0.35f));
        t.key(2.6f, pose::rest(cast.crew.rig), ease::hold(0.30f));
        t.key(4.4f, pose::turnToLook(cast.crew.rig, -22.0f));
        t.key(8.0f, pose::turnToLook(cast.crew.rig, -22.0f));

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 8.0f);
        Pose p = poseAdd(t.at(b.local), pose::breathe(b.global, 5.0f, 1.0f, 0.9f));
        cast.crew.look(p, -0.5f * saturate((b.local - 3.6f) / 1.2f), 0.1f);
        crew.pose = p;
        stage.entities.add(crew);

        glide(scene, b, {-5.6f, 1.95f, 0.2f}, {-2.2f, 1.35f, 4.2f},
              {-5.2f, 1.90f, 0.0f}, {-2.2f, 1.38f, 4.2f}, 36.0f, 0.035f);
        stage.finish(scene);
    });

    // =====================================================================
    // ЧАСТЬ ВТОРАЯ -- ПОСЛЕДНИЙ КЛИЕНТ
    // =====================================================================

    // The door. He is a silhouette in it before he is a person.
    shot("door", DINER, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        // Walks in from outside, stops two blocks past the door.
        const float u = ease::smooth(saturate((b.local - 1.4f) / 3.4f));
        const float z = lerp(-8.4f, -3.6f, u);
        const bool moving = u > 0.02f && u < 0.98f;

        Entity clerk = cast.clerk.at({3.6f, 0.0f, z}, 178.0f);
        Pose p = moving ? pose::walk(cast.clerk.rig, b.global * 0.85f, 21.0f)
                        : pose::weary(cast.clerk.rig);
        if (!moving && u > 0.5f) p = poseAdd(p, pose::breathe(b.global, 4.4f, -1.0f, 1.0f));
        cast.clerk.look(p, 0.0f, -0.2f);
        clerk.pose = p;
        stage.entities.add(clerk);

        hold(scene, {-0.6f, 1.80f, 1.6f}, {3.4f, 1.15f, -5.2f}, 42.0f, 0.03f);
        stage.finish(scene);
    });

    shot("turn", DINER, 6.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Track<Pose> t;
        t.key(0.0f, pose::turnToLook(cast.crew.rig, -22.0f), ease::hold(0.25f));
        t.key(1.9f, pose::turnToLook(cast.crew.rig, 34.0f), ease::hold(0.4f));
        t.key(3.4f, pose::rest(cast.crew.rig));
        t.key(6.0f, pose::rest(cast.crew.rig));

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 6.0f);
        Pose p = poseAdd(t.at(b.local), pose::breathe(b.global, 5.0f, 1.0f, 0.9f));
        cast.crew.look(p, 0.62f * saturate((b.local - 1.5f) / 1.0f), 0.0f);
        crew.pose = p;
        stage.entities.add(crew);

        glide(scene, b, {-4.4f, 1.70f, 1.4f}, {-2.1f, 1.42f, 4.3f},
              {-4.0f, 1.70f, 1.8f}, {-2.1f, 1.42f, 4.3f}, 33.0f, 0.04f);
        stage.finish(scene);
    });

    // The two-shot. Profile, across the counter, so the gap between them is
    // the subject.
    shot("counter", DINER, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 2.0f);
        crew.pose = poseAdd(pose::rest(cast.crew.rig), pose::breathe(b.global, 5.0f, 1.0f));
        cast.crew.look(crew.pose, 0.1f, -0.1f);
        stage.entities.add(crew);

        // He comes the last two blocks to the counter and stops.
        const float u = ease::smooth(saturate(b.local / 3.0f));
        Entity clerk = cast.clerk.at({-2.2f, 0.0f, lerp(-1.2f, 0.9f, u)}, 181.0f);
        Pose cp = u < 0.97f ? pose::walk(cast.clerk.rig, b.global * 0.8f, 17.0f)
                            : poseAdd(pose::weary(cast.clerk.rig),
                                      pose::breathe(b.global, 4.4f, -1.0f));
        cast.clerk.look(cp, 0.0f, 0.15f);
        clerk.pose = cp;
        stage.entities.add(clerk);

        // Three quarters across the counter, not along it: from the end of
        // the counter the two of them line up one behind the other and the
        // gap between them -- which is the subject -- disappears.
        glide(scene, b, {-6.6f, 1.78f, -2.6f}, {-2.0f, 1.18f, 2.2f},
              {-6.0f, 1.74f, -1.9f}, {-2.1f, 1.20f, 2.4f}, 40.0f, 0.05f);
        stage.finish(scene);
    });

    card("ask", 7.0f, "— СКОЛЬКО С МЕНЯ?");

    // The cup is put down and pushed across. The hand does the talking.
    shot("give", DINER, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        const float reach = ease::smooth(saturate((b.local - 1.6f) / 2.6f)) *
                            (1.0f - ease::smooth(saturate((b.local - 6.4f) / 2.2f)));

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 2.0f);
        Pose p = pose::offer(cast.crew.rig, 1.0f, reach);
        p = poseAdd(p, pose::breathe(b.global, 5.0f, 1.0f, 0.5f));
        cast.crew.look(p, 0.2f, -0.35f);
        crew.pose = p;
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({-2.2f, 0.0f, 0.9f}, 181.0f);
        Pose cp = poseAdd(pose::weary(cast.clerk.rig), pose::breathe(b.global, 4.4f, -1.0f));
        cast.clerk.look(cp, 0.0f, -0.4f);
        clerk.pose = cp;
        stage.entities.add(clerk);

        // The cup travels across the counter top, which is at y = 1.
        Prop cupProp;
        cupProp.model = &cup;
        cupProp.voxelSize = 1.0f / 16.0f;
        cupProp.position = {lerp(-2.6f, -2.2f, reach), 1.0f, lerp(2.9f, 2.1f, reach)};
        cupProp.yawDegrees = 18.0f;
        stage.props.add(cupProp);

        glide(scene, b, {-5.6f, 1.62f, 0.6f}, {-2.3f, 1.15f, 2.6f},
              {-5.0f, 1.55f, 0.9f}, {-2.3f, 1.10f, 2.5f}, 32.0f, 0.05f);
        stage.finish(scene);
    });

    card("free", 5.0f, "— НИСКОЛЬКО.\nМЫ УЖЕ ЗАКРЫТЫ.");

    // =====================================================================
    // ЧАСТЬ ТРЕТЬЯ -- ТРЕТИЙ
    // =====================================================================

    shot("eat", DINER, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        // Sitting at the middle table. The stool top is at y = 1.
        Entity clerk = cast.clerk.at({-3.0f, 1.0f, -3.0f}, 270.0f);
        Pose p = poseAdd(pose::sit(cast.clerk.rig, -6.0f),
                         pose::breathe(b.global, 4.6f, -1.0f, 0.8f));
        // A slow, small lift of the near arm: eating, without a jaw to help.
        const float bite = 0.5f + 0.5f * std::sin(b.local * 0.9f);
        p[cast.clerk.rig.rightElbow].rotationDegrees.x += 26.0f * bite;
        p[joint::RightArm].rotationDegrees.x += 10.0f * bite;
        p[joint::Head].rotationDegrees.x -= 5.0f * bite;
        cast.clerk.look(p, 0.0f, -0.55f);
        clerk.pose = p;
        stage.entities.add(clerk);

        Prop cupProp;
        cupProp.model = &cup;
        cupProp.voxelSize = 1.0f / 16.0f;
        cupProp.position = {-2.0f, 1.0f, -3.0f};
        cupProp.yawDegrees = -24.0f;
        stage.props.add(cupProp);

        // A seated figure's head is at about 2.3, not at 1.3: the root drops
        // by six model pixels and the rest of him is still two blocks tall.
        // Every camera in the diner that aims at someone sitting aims there.
        glide(scene, b, {2.6f, 2.45f, -0.4f}, {-2.9f, 2.15f, -3.0f},
              {2.0f, 2.30f, -0.9f}, {-2.9f, 2.12f, -3.0f}, 40.0f, 0.05f);
        stage.finish(scene);
    });

    // The pan. It starts on him and ends somewhere he is not looking.
    shot("pan", DINER, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Entity clerk = cast.clerk.at({-3.0f, 1.0f, -3.0f}, 270.0f);
        clerk.pose = poseAdd(pose::sit(cast.clerk.rig, -6.0f),
                             pose::breathe(b.global, 4.6f, -1.0f, 0.8f));
        cast.clerk.look(clerk.pose, 0.0f, -0.5f);
        stage.entities.add(clerk);

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 2.0f);
        crew.pose = poseAdd(pose::wipe(cast.crew.rig, b.global * 0.3f),
                            pose::breathe(b.global, 5.0f, 1.0f, 0.5f));
        stage.entities.add(crew);

        // Someone is in the corner of this pan the whole way through, and the
        // framing is what decides when that becomes visible.
        Entity stripe = cast.stripe.at({-7.0f, 1.0f, -5.0f}, 180.0f);
        Pose sp = pose::sit(cast.stripe.rig, 2.0f);
        sp[joint::Head].rotationDegrees.y = -34.0f;
        cast.stripe.look(sp, -0.75f, 0.05f);
        stripe.pose = sp;
        stage.entities.add(stripe);

        glide(scene, b, {2.4f, 2.30f, -1.2f}, {-2.9f, 2.15f, -3.0f},
              {2.4f, 2.30f, -1.2f}, {-6.95f, 2.28f, -5.0f}, 40.0f, 0.04f,
              ease::hold(0.30f));
        stage.finish(scene);
    });

    shot("reveal", DINER, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Entity stripe = cast.stripe.at({-7.0f, 1.0f, -5.0f}, 180.0f);
        Pose sp = poseAdd(pose::sit(cast.stripe.rig, 2.0f),
                          pose::breathe(b.global, 8.0f, 1.0f, 0.35f));
        sp[joint::Head].rotationDegrees.y = -34.0f + 34.0f * ease::smooth(saturate((b.local - 3.0f) / 2.4f));
        cast.stripe.look(sp, lerp(-0.75f, 0.0f, ease::smooth(saturate((b.local - 3.2f) / 2.2f))), 0.0f);
        stripe.pose = sp;
        stage.entities.add(stripe);

        glide(scene, b, {-3.4f, 2.32f, -1.6f}, {-7.0f, 2.28f, -4.95f},
              {-4.6f, 2.30f, -2.4f}, {-7.0f, 2.28f, -4.95f}, 36.0f, 0.06f);
        stage.finish(scene);
    });

    // The eyes. Close enough that the pupils are the only thing that moves.
    shot("eyes", DINER, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Entity stripe = cast.stripe.at({-7.0f, 1.0f, -5.0f}, 180.0f);
        Pose sp = poseAdd(pose::sit(cast.stripe.rig, 2.0f),
                          pose::breathe(b.global, 8.0f, 1.0f, 0.3f));
        // Straight at the lens, then away to the window, and it is the look
        // away that carries the shot.
        const float away = ease::smooth(saturate((b.local - 4.6f) / 1.8f));
        sp[joint::Head].rotationDegrees.y = -6.0f * away;
        cast.stripe.look(sp, lerp(0.0f, -0.85f, away), lerp(0.0f, 0.15f, away));
        stripe.pose = sp;
        stage.entities.add(stripe);

        glide(scene, b, {-6.30f, 2.34f, -2.5f}, {-7.0f, 2.33f, -4.90f},
              {-6.45f, 2.33f, -2.8f}, {-7.0f, 2.33f, -4.90f}, 21.0f, 0.09f);
        stage.finish(scene);
    });

    shot("notice", DINER, 7.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        const float turn = ease::smooth(saturate((b.local - 1.2f) / 1.6f));

        Entity clerk = cast.clerk.at({-3.0f, 1.0f, -3.0f}, 270.0f);
        Pose cp = poseAdd(pose::sit(cast.clerk.rig, -4.0f),
                          pose::breathe(b.global, 4.6f, -1.0f, 0.6f));
        cp[joint::Body].rotationDegrees.y = -38.0f * turn;
        cp[joint::Head].rotationDegrees.y = -30.0f * turn;
        cast.clerk.look(cp, -0.7f * turn, 0.0f);
        clerk.pose = cp;
        stage.entities.add(clerk);

        Entity crew = cast.crew.at({-2.0f, 0.0f, 4.4f}, 2.0f);
        Pose kp = poseAdd(pose::turnToLook(cast.crew.rig, -48.0f * turn),
                          pose::breathe(b.global, 5.0f, 1.0f, 0.6f));
        cast.crew.look(kp, -0.8f * turn, 0.0f);
        crew.pose = kp;
        stage.entities.add(crew);

        hold(scene, {1.4f, 2.55f, 0.2f}, {-3.8f, 2.05f, -3.2f}, 46.0f, 0.04f);
        stage.finish(scene);
    });

    // The reverse. Nobody there, and one small magenta thing on the table.
    shot("empty", DINER, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        Prop shardProp;
        shardProp.model = &shard;
        shardProp.voxelSize = 1.0f / 16.0f;
        shardProp.position = {-7.5f, 1.0f, -4.0f};
        shardProp.yawDegrees = 24.0f;
        stage.props.add(shardProp);

        glide(scene, b, {-4.6f, 2.30f, -2.4f}, {-7.0f, 2.26f, -4.95f},
              {-5.3f, 1.86f, -2.9f}, {-7.45f, 1.20f, -4.05f}, 36.0f, 0.06f);
        stage.finish(scene);
    });

    card("allnight", 7.0f, "ОН СИДЕЛ ТАМ\nВСЮ НОЧЬ.");

    shot("pickup", DINER, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightInterior(scene);

        const float reach = ease::smooth(saturate((b.local - 1.8f) / 2.2f));
        const float lift = ease::smooth(saturate((b.local - 4.6f) / 2.4f));

        Entity crew = cast.crew.at({-5.4f, 0.0f, -4.6f}, 90.0f);
        Pose p = pose::offer(cast.crew.rig, 1.0f, reach * (1.0f - 0.45f * lift));
        p[joint::Body].rotationDegrees.x = -14.0f * reach + 6.0f * lift;
        p[joint::Head].rotationDegrees.x = -16.0f * reach + 10.0f * lift;
        p = poseAdd(p, pose::breathe(b.global, 5.0f, 1.0f, 0.5f));
        cast.crew.look(p, 0.35f, -0.6f + 0.5f * lift);
        crew.pose = p;
        stage.entities.add(crew);

        Prop shardProp;
        shardProp.model = &shard;
        shardProp.voxelSize = 1.0f / 16.0f;
        shardProp.position = {lerp(-7.5f, -6.4f, lift), lerp(1.0f, 1.44f, lift), -4.1f};
        shardProp.yawDegrees = 24.0f + 40.0f * lift;
        shardProp.rollDegrees = 18.0f * lift;
        stage.props.add(shardProp);

        glide(scene, b, {-3.8f, 1.95f, -1.9f}, {-6.7f, 1.25f, -4.1f},
              {-3.4f, 1.86f, -2.3f}, {-6.8f, 1.40f, -4.1f}, 34.0f, 0.06f);
        stage.finish(scene);
    });

    // Out of the door, into the dark. Shot from outside so the lit room is
    // behind them and they leave it.
    shot("outside", DINER, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);

        const float u = ease::smooth(saturate((b.local - 1.0f) / 5.5f));
        const float z = lerp(-5.0f, -12.5f, u);
        const bool moving = u > 0.02f && u < 0.985f;

        Entity crew = cast.crew.at({3.2f, 0.0f, z}, 0.0f);
        crew.pose = moving ? pose::walk(cast.crew.rig, b.global * 0.8f, 24.0f)
                           : pose::rest(cast.crew.rig);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({4.6f, 0.0f, z + 1.1f}, 0.0f);
        clerk.pose = moving ? pose::walk(cast.clerk.rig, b.global * 0.8f + 0.45f, 21.0f)
                            : pose::weary(cast.clerk.rig);
        stage.entities.add(clerk);

        Prop shardProp;
        shardProp.model = &shard;
        shardProp.voxelSize = 1.0f / 16.0f;
        shardProp.position = {2.5f, 1.35f, z + 0.2f};
        shardProp.anchor = Prop::Anchor::Centre;
        shardProp.yawDegrees = 60.0f;
        stage.props.add(shardProp);

        glide(scene, b, {6.5f, 2.6f, -19.0f}, {3.8f, 1.4f, -7.0f},
              {6.0f, 2.3f, -20.5f}, {3.8f, 1.2f, -12.0f}, 40.0f, 0.03f);
        stage.finish(scene);
    });

    // =====================================================================
    // ЧАСТЬ ЧЕТВЁРТАЯ -- ДОРОГА
    // =====================================================================

    // Everything on this set stands on ground that is not flat, so the feet
    // come off the terrain function rather than off a guess.
    auto onRoad = [](float x, float z) {
        return Vec3{x, float(road::groundBlockY(x, z)) + 1.0f, z};
    };

    shot("horizon", ROAD, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);

        const float x = lerp(-34.0f, -28.0f, b.t01);
        Entity crew = cast.crew.at(onRoad(x, -0.8f), 270.0f);
        crew.pose = pose::walk(cast.crew.rig, b.global * 0.62f, 24.0f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at(onRoad(x - 1.4f, 1.0f), 270.0f);
        clerk.pose = pose::walk(cast.clerk.rig, b.global * 0.62f + 0.4f, 20.0f);
        stage.entities.add(clerk);

        glide(scene, b, {-46.0f, 15.0f, 16.0f}, {30.0f, 3.0f, -2.0f},
              {-42.0f, 12.5f, 13.0f}, {40.0f, 3.0f, -2.0f}, 44.0f);
        stage.finish(scene);
    });

    // Tracking beside them. The camera holds them still in frame and lets the
    // trees do the moving, which is the cheapest way to sell distance.
    shot("walking", ROAD, 12.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);

        const float x = lerp(-6.0f, 16.0f, b.t01);
        Entity crew = cast.crew.at(onRoad(x, -0.9f), 270.0f);
        crew.pose = pose::walk(cast.crew.rig, b.global * 0.72f, 25.0f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at(onRoad(x - 1.6f, 0.9f), 270.0f);
        clerk.pose = pose::walk(cast.clerk.rig, b.global * 0.72f + 0.5f, 21.0f);
        stage.entities.add(clerk);

        Prop shardProp;
        shardProp.model = &shard;
        shardProp.voxelSize = 1.0f / 16.0f;
        shardProp.position = onRoad(x, -0.9f) + Vec3{0.35f, 1.35f, -0.45f};
        shardProp.anchor = Prop::Anchor::Centre;
        shardProp.yawDegrees = 60.0f + b.global * 20.0f;
        stage.props.add(shardProp);

        const Vec3 mid = onRoad(x - 0.8f, 0.0f);
        hold(scene, mid + Vec3{-2.0f, 1.7f, -9.5f}, mid + Vec3{0.0f, 1.1f, 0.0f}, 40.0f, 0.06f);
        stage.finish(scene);
    });

    shot("ford", ROAD, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightNightExterior(scene);

        const float x = lerp(52.0f, 68.0f, ease::smooth(b.t01));
        Entity crew = cast.crew.at(onRoad(x, -0.6f), 270.0f);
        crew.pose = pose::walk(cast.crew.rig, b.global * 0.58f, 27.0f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at(onRoad(x - 2.0f, 1.2f), 270.0f);
        clerk.pose = pose::walk(cast.clerk.rig, b.global * 0.58f + 0.5f, 23.0f);
        stage.entities.add(clerk);

        Prop shardProp;
        shardProp.model = &shard;
        shardProp.voxelSize = 1.0f / 16.0f;
        shardProp.position = onRoad(x, -0.6f) + Vec3{0.35f, 1.35f, -0.45f};
        shardProp.anchor = Prop::Anchor::Centre;
        shardProp.yawDegrees = 60.0f + b.global * 20.0f;
        stage.props.add(shardProp);

        const Vec3 mid = onRoad(x - 1.0f, 0.0f);
        glide(scene, b, mid + Vec3{4.0f, 2.4f, -10.0f}, mid + Vec3{0.0f, 1.0f, 0.0f},
              mid + Vec3{-3.0f, 1.9f, -9.0f}, mid + Vec3{0.0f, 1.0f, 0.0f}, 42.0f, 0.05f);
        stage.finish(scene);
    });

    card("further", 5.0f, "СВЕТ БЫЛ ДАЛЬШЕ,\nЧЕМ КАЗАЛСЯ.");

    // =====================================================================
    // ЧАСТЬ ПЯТАЯ -- СВЕТ
    // =====================================================================

    auto onShrine = [](float x, float z) {
        return Vec3{x, float(shrine::groundBlockY(x, z)) + 1.0f, z};
    };

    shot("arrive", SHRINE, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, 0.0f);

        // The causeway is flat cobble at y = 0, so its top is y = 1.
        const float x = lerp(-22.0f, -9.0f, ease::smooth(b.t01));
        Entity crew = cast.crew.at({x, 1.0f, -0.7f}, 270.0f);
        crew.pose = pose::walk(cast.crew.rig, b.global * 0.55f, 22.0f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({x - 1.7f, 1.0f, 0.9f}, 270.0f);
        clerk.pose = pose::walk(cast.clerk.rig, b.global * 0.55f + 0.5f, 19.0f);
        stage.entities.add(clerk);

        glide(scene, b, {-30.0f, 6.5f, -13.0f}, {-6.0f, 2.0f, 0.0f},
              {-21.0f, 4.2f, -10.0f}, {0.0f, 2.5f, 0.0f}, 44.0f);
        stage.finish(scene);
    });

    // She has been standing on the plinth the whole time. The reveal is the
    // camera arriving, not her appearing.
    shot("white", SHRINE, 11.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, 0.0f);

        Entity white = cast.white.at({0.0f, 2.0f, 0.0f}, 90.0f);
        white.pose = poseAdd(pose::rest(cast.white.rig), pose::float_(b.global, 7.0f));
        cast.white.look(white.pose, -0.25f, 0.0f);
        stage.entities.add(white);

        Entity crew = cast.crew.at({-5.4f, 1.0f, -1.6f}, 262.0f);
        crew.pose = poseAdd(pose::rest(cast.crew.rig), pose::breathe(b.global, 4.2f, 1.0f));
        cast.crew.look(crew.pose, -0.1f, 0.4f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({-5.0f, 1.0f, 1.4f}, 278.0f);
        clerk.pose = poseAdd(pose::weary(cast.clerk.rig), pose::breathe(b.global, 4.6f, -1.0f));
        cast.clerk.look(clerk.pose, -0.1f, 0.45f);
        stage.entities.add(clerk);

        // She is two blocks of character on a five-block plinth inside a
        // thirty-block ring: from anywhere far enough back to see the ring
        // she is a smudge. The move ends close, with the two of them held in
        // the near corner of frame for scale.
        glide(scene, b, {-14.0f, 3.6f, -9.0f}, {-3.0f, 2.4f, 0.0f},
              {-5.6f, 3.05f, -3.6f}, {0.0f, 3.05f, 0.0f}, 32.0f, 0.07f);
        stage.finish(scene);
    });

    // The gesture, and the dust it lights. Motes are frozen quads, so they
    // must be aimed at the eye or most of them present an edge.
    shot("raise", SHRINE, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, 0.0f);

        const float up = ease::smooth(saturate((b.local - 2.0f) / 3.0f));

        Entity white = cast.white.at({0.0f, 2.0f, 0.0f}, 90.0f);
        Pose wp = pose::raise(cast.white.rig, 1.0f, up);
        wp = poseAdd(wp, pose::float_(b.global, 7.0f));
        cast.white.look(wp, -0.2f, 0.3f * up);
        white.pose = wp;
        stage.entities.add(white);

        const Vec3 eye{-4.2f, 3.05f, -3.2f};

        scatter::ParticleStyle mote;
        mote.size = {0.030f, 0.030f};
        mote.sizeJitter = 0.6f;
        mote.tint = srgbToLinear(Vec3{1.0f, 0.72f, 0.95f});
        mote.emission = srgbToLinear(Vec3{1.0f, 0.42f, 0.90f}) * (1.4f * up);
        mote.randomYaw = false;

        std::vector<Sprite> motes =
            scatter::inSphere({0.0f, 3.1f, 0.0f}, 3.4f, 200, 20250831u, mote);
        aimAt(motes, eye);
        stage.sprites.add(motes);

        glide(scene, b, eye, {0.0f, 2.9f, 0.0f}, eye + Vec3{0.7f, 0.35f, 0.6f},
              {0.0f, 3.15f, 0.0f}, 34.0f, 0.07f);
        stage.finish(scene);
    });

    shot("three", SHRINE, 9.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, 0.12f * b.t01);

        Entity white = cast.white.at({0.0f, 2.0f, 0.0f}, 90.0f);
        white.pose = poseAdd(pose::raise(cast.white.rig, 1.0f, 1.0f - 0.35f * b.t01),
                             pose::float_(b.global, 7.0f));
        cast.white.look(white.pose, -0.3f, 0.1f);
        stage.entities.add(white);

        Entity crew = cast.crew.at({-4.4f, 1.0f, -1.4f}, 262.0f);
        crew.pose = poseAdd(pose::rest(cast.crew.rig), pose::breathe(b.global, 4.2f, 1.0f));
        cast.crew.look(crew.pose, -0.15f, 0.5f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({-4.0f, 1.0f, 1.8f}, 278.0f);
        clerk.pose = poseAdd(pose::rest(cast.clerk.rig), pose::breathe(b.global, 4.6f, -1.0f));
        cast.clerk.look(clerk.pose, -0.15f, 0.5f);
        stage.entities.add(clerk);

        glide(scene, b, {-7.6f, 2.5f, -5.4f}, {-1.8f, 2.4f, 0.0f},
              {-7.1f, 2.4f, -4.9f}, {-1.8f, 2.35f, 0.0f}, 38.0f, 0.06f);
        stage.finish(scene);
    });

    // Dawn. The sun comes up over the eastern ridge and the magenta stops
    // being the only colour in the world.
    shot("dawn", SHRINE, 12.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, ease::smooth(saturate((b.local - 1.0f) / 9.0f)));

        Entity white = cast.white.at({0.0f, 2.0f, 0.0f}, 90.0f);
        white.pose = poseAdd(pose::rest(cast.white.rig), pose::float_(b.global, 7.0f));
        stage.entities.add(white);

        Entity crew = cast.crew.at({-4.4f, 1.0f, -1.4f}, 262.0f);
        crew.pose = poseAdd(pose::rest(cast.crew.rig), pose::breathe(b.global, 4.2f, 1.0f));
        cast.crew.look(crew.pose, 0.5f, 0.35f);
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({-4.0f, 1.0f, 1.8f}, 278.0f);
        clerk.pose = poseAdd(pose::rest(cast.clerk.rig), pose::breathe(b.global, 4.6f, -1.0f));
        cast.clerk.look(clerk.pose, 0.5f, 0.35f);
        stage.entities.add(clerk);

        glide(scene, b, {-11.0f, 3.0f, -6.0f}, {2.0f, 2.6f, 0.0f},
              {-13.0f, 5.5f, -3.0f}, {8.0f, 3.4f, 0.0f}, 46.0f);
        stage.finish(scene);
    });

    shot("last", SHRINE, 10.0f, [&](Scene& scene, const Beat& b) {
        stage.begin();
        lightShrine(scene, 1.0f);

        Entity white = cast.white.at({0.0f, 2.0f, 0.0f}, 90.0f);
        white.pose = poseAdd(pose::rest(cast.white.rig), pose::float_(b.global, 7.0f));
        stage.entities.add(white);

        Entity crew = cast.crew.at({-4.4f, 1.0f, -1.4f}, 262.0f);
        crew.pose = poseAdd(pose::rest(cast.crew.rig), pose::breathe(b.global, 4.2f, 1.0f));
        stage.entities.add(crew);

        Entity clerk = cast.clerk.at({-4.0f, 1.0f, 1.8f}, 278.0f);
        clerk.pose = poseAdd(pose::rest(cast.clerk.rig), pose::breathe(b.global, 4.6f, -1.0f));
        stage.entities.add(clerk);

        // And the fourth, standing on the ridge where he has been all along.
        Entity stripe = cast.stripe.at(onShrine(26.0f, -6.0f), 96.0f);
        stripe.pose = poseAdd(pose::rest(cast.stripe.rig), pose::breathe(b.global, 9.0f, 1.0f, 0.4f));
        stage.entities.add(stripe);

        glide(scene, b, {-16.0f, 7.0f, -14.0f}, {2.0f, 2.0f, 0.0f},
              {-22.0f, 10.0f, -19.0f}, {4.0f, 2.0f, -1.0f}, 46.0f);
        stage.finish(scene);
    });

    card("end", 8.0f, "КОНЕЦ");

    // ------------------------------------------------------------ the look
    if (!plain) {
        reel.finish = [&](RenderTargets& targets, const Frame& f, const Shot& s) -> Image {
            if (s.card) {
                // A card is text on black. Cel banding has nothing to work on
                // and the outline pass would draw a box around every glyph.
                Image frame = targets.color;
                applyBloom(frame, {0.8f, 0.5f, 0.10f, 6});
                applyVignette(frame, {0.30f, 0.72f, 0.55f});
                return frame;
            }

            CelSettings cel;
            cel.bands = 5;
            cel.range = 1.5f;
            cel.bandGamma = 1.0f;
            cel.highlightCeiling = 4.0f;
            cel.shadowFloor = 0.10f;
            cel.depthThreshold = 0.36f;
            cel.normalThreshold = 0.32f;
            cel.outlineWidth = draft ? 1 : 2;
            cel.outlineColor = srgbToLinear(Vec3{0.05f, 0.04f, 0.06f});
            cel.outlineOpacity = 0.82f;
            cel.saturation = 1.06f;

            Image frame = celShade(targets, cel);

            applyBloom(frame, {1.1f, 0.6f, 0.07f, 6});

            GradeSettings grade;
            grade.contrast = 1.06f;
            grade.saturation = 1.05f;
            grade.temperature = -0.04f;
            applyGrade(frame, grade);

            applyVignette(frame, {0.26f, 0.72f, 0.52f});
            // Seeded off the quantised time, so a held pair grains alike --
            // an unseeded call here would break the hold silently.
            applyGrain(frame, 0.012f, uint64_t(f.time * 24.0f) + 1u);
            return frame;
        };
    }

    reel.layout();

    // ----------------------------------------------------------- run it
    if (assembleOnly) {
        std::string error;
        const std::string dir = "out/" + reel.name;
        if (!reel.assemble(dir, &error)) {
            std::printf("[film] %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    // One frame of one shot, with the tracer's own reporting on. What a
    // five-minute render costs is decided here and nowhere else, and the
    // number to have before starting one is seconds per traced frame on the
    // most expensive set -- not an average over the cards.
    if (!timeShot.empty()) {
        for (const Shot& s : reel.shots) {
            if (s.name != timeShot) continue;
            Scene& scene = *reel.sets[size_t(s.set)];
            Beat beat;
            beat.local = s.seconds * 0.5f;
            beat.t01 = 0.5f;
            beat.global = s.startTime + beat.local;
            beat.dt = 2.0f / float(reel.fps);
            s.play(scene, beat);

            PathSettings ps = reel.settings;
            ps.progress = false;

            // `bare` drops the sprites and props. The per-ray cost varies six
            // times across these sets and the dust in `raise` is the first
            // suspect, so the measurement has to be able to take it away.
            if (bare) {
                scene.sprites = nullptr;
                scene.props = nullptr;
            }

            std::printf("[film] %s%s: %dx%d, %d spp, %d bounces\n", s.name.c_str(),
                        bare ? " (no sprites or props)" : "", ps.width, ps.height,
                        ps.samplesPerPixel, ps.maxBounces);

            using Clock = std::chrono::steady_clock;
            RenderStats stats;
            RenderTargets targets;
            renderPath(scene, ps, &stats, &targets);

            // Keep the raw radiance: the denoiser is about to overwrite it,
            // and comparing a denoised frame against a raw one measures the
            // denoiser rather than the renderer.
            const Image rawCpu = targets.color;

            const auto denoiseStart = Clock::now();
            targets.color = denoise(targets, reel.denoiseSettings);
            const double denoiseSeconds =
                std::chrono::duration<double>(Clock::now() - denoiseStart).count();

            Frame f;
            f.index = s.firstFrame;
            f.time = beat.global;

            const auto finishStart = Clock::now();
            Image frame = reel.finish ? reel.finish(targets, f, s) : targets.color;
            const double finishSeconds =
                std::chrono::duration<double>(Clock::now() - finishStart).count();

            const auto saveStart = Clock::now();
            std::string err;
            pngSave("out/film_time.png", frame, reel.tone, &err);
            const double saveSeconds =
                std::chrono::duration<double>(Clock::now() - saveStart).count();

            // ---- the same frame on the GPU
            //
            // The whole port exists to be measured here rather than on a
            // synthetic scene: these shots are what the film is made of, and
            // they are the ones carrying the sprites and props that turned
            // out to cost the most.
            if (onGpu) {
                Window window;
                std::string werr;
                if (!window.create("film gpu", 320, 200, &werr)) {
                    std::printf("  window: %s\n", werr.c_str());
                    return 1;
                }
                gl::ContextSettings ctx;
                ctx.samples = 0;
                void* context = nullptr;
                if (!gl::createContext(window.deviceContext(), ctx, &context, &werr)) {
                    std::printf("  context: %s\n", werr.c_str());
                    return 1;
                }

                gpu::WavefrontTracer wave;
                if (!wave.build(&werr) || !wave.upload(scene, &werr)) {
                    std::printf("  gpu: %s\n", werr.c_str());
                    return 1;
                }

                Image gpuImage;
                RenderTargets gpuAovs;
                gpu::WavefrontStats ws;
                if (!wave.render(scene, ps, gpuImage, &gpuAovs, &ws, &werr)) {
                    std::printf("  gpu: %s\n", werr.c_str());
                    return 1;
                }

                // Per-pixel L1 is the wrong instrument here and it took a
                // wrong answer to notice. Two independent renders of a dark
                // interior at 64 samples disagree by half a pixel value out
                // of pure variance -- the means can be identical and the L1
                // still reads 0.6. So both frames are averaged into coarse
                // blocks first: the noise averages away and what is left is
                // bias, which is the only thing a comparison can be about.
                constexpr int kBlocks = 24;
                std::vector<Vec3> cpuBlocks(kBlocks * kBlocks, Vec3{0.0f});
                std::vector<Vec3> gpuBlocks(kBlocks * kBlocks, Vec3{0.0f});
                std::vector<float> counts(size_t(kBlocks) * kBlocks, 0.0f);

                double pixelAbs = 0.0, pixelRef = 0.0;
                for (int y = 0; y < ps.height; ++y) {
                    for (int x = 0; x < ps.width; ++x) {
                        const Vec3 a = rawCpu.at(x, y);
                        const Vec3 b = gpuImage.at(x, y);
                        pixelAbs += std::fabs(a.x - b.x) + std::fabs(a.y - b.y) +
                                    std::fabs(a.z - b.z);
                        pixelRef += std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);

                        const int bx = std::min(kBlocks - 1, x * kBlocks / ps.width);
                        const int by = std::min(kBlocks - 1, y * kBlocks / ps.height);
                        const size_t bi = size_t(by) * kBlocks + size_t(bx);
                        cpuBlocks[bi] += a;
                        gpuBlocks[bi] += b;
                        counts[bi] += 1.0f;
                    }
                }

                double blockAbs = 0.0, blockRef = 0.0, worstBlock = 0.0;
                for (size_t i = 0; i < cpuBlocks.size(); ++i) {
                    if (counts[i] <= 0.0f) continue;
                    const Vec3 a = cpuBlocks[i] / counts[i];
                    const Vec3 b = gpuBlocks[i] / counts[i];
                    const double d = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) +
                                     std::fabs(a.z - b.z);
                    const double r = std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);
                    blockAbs += d;
                    blockRef += r;
                    worstBlock = std::max(worstBlock, d / std::max(r, 1e-3));
                }

                std::printf("  gpu      %7.3f s   trace only, %d dispatches, %.0f MiB\n",
                            ws.traceSeconds, ws.dispatches, double(ws.bytesOnGpu) / 1048576.0);
                std::printf("  vs cpu   %7.2fx on the trace\n",
                            stats.seconds / std::max(ws.traceSeconds, 1e-9));
                std::printf("           block mean %.4f (worst block %.3f), per pixel %.4f\n",
                            blockAbs / std::max(blockRef, 1e-9), worstBlock,
                            pixelAbs / std::max(pixelRef, 1e-9));

                // The denoiser, on the same buffers the CPU one just used.
                // Now that the trace is seconds instead of tens of seconds,
                // this is the other half of the frame.
                gpu::Denoiser denoiser;
                if (denoiser.build(&werr)) {
                    // targets.color already holds the CPU-denoised frame by now,
                    // so the raw radiance goes back in first -- otherwise this
                    // would be filtering something already filtered.
                    RenderTargets raw = targets;
                    raw.color = rawCpu;
                    Image gpuDenoised;
                    if (denoiser.run(raw, reel.denoiseSettings, gpuDenoised, &werr)) {
                        std::printf("  denoise  %7.3f s   on the gpu (was %.3f on the cpu)\n",
                                    denoiser.lastSeconds(), denoiseSeconds);
                        std::printf("           upload %.3f, filter %.3f, readback %.3f\n",
                                    denoiser.lastUploadSeconds(), denoiser.lastFilterSeconds(),
                                    denoiser.lastReadbackSeconds());
                    }
                }

                std::string perr;
                pngSave("out/film_gpu.png", gpuImage, reel.tone, &perr);
                std::printf("  wrote out/film_gpu.png\n");
            }

            const double post = denoiseSeconds + finishSeconds + saveSeconds;
            const double total = stats.setupSeconds + stats.seconds + post;

            std::printf("  setup    %7.3f s   (scene copy + %zu emissive faces)\n",
                        stats.setupSeconds, stats.emissiveFaces);
            std::printf("  trace    %7.3f s   %d threads, %.1f M rays, %.2f Mrays/s\n",
                        stats.seconds, stats.threadsUsed, double(stats.totalRays) / 1e6,
                        double(stats.totalRays) / 1e6 / std::max(stats.seconds, 1e-9));
            std::printf("  denoise  %7.3f s\n", denoiseSeconds);
            std::printf("  finish   %7.3f s   (cel, bloom, grade, vignette, grain)\n",
                        finishSeconds);
            std::printf("  save     %7.3f s\n", saveSeconds);
            std::printf("  ------------------\n");
            std::printf("  frame    %7.3f s   trace %.0f%%, post %.0f%%\n", total,
                        100.0 * stats.seconds / total, 100.0 * post / total);
            return 0;
        }
        std::printf("[film] no shot called %s\n", timeShot.c_str());
        return 1;
    }

    if (probe) {
        // One still from the middle of every shot. Twenty-five stills at
        // small size cost a couple of minutes and answer every staging
        // question that a five-hour render would answer too late.
        PathSettings ps;
        ps.width = 480;
        ps.height = 270;
        ps.samplesPerPixel = 20;
        ps.maxBounces = 4;
        ps.progress = false;

        int index = 0;
        for (const Shot& s : reel.shots) {
            Scene& scene = *reel.sets[size_t(s.set)];
            Beat beat;
            beat.local = s.seconds * 0.55f;
            beat.t01 = 0.55f;
            beat.global = s.startTime + beat.local;
            beat.dt = 2.0f / float(reel.fps);
            s.play(scene, beat);

            RenderTargets targets;
            renderPath(scene, ps, nullptr, &targets);
            targets.color = denoise(targets, {});

            Frame f;
            f.index = s.firstFrame;
            f.time = beat.global;
            Image frame = reel.finish ? reel.finish(targets, f, s) : targets.color;

            char path[160];
            std::snprintf(path, sizeof path, "out/film_probe/%02d_%s.png", index, s.name.c_str());
            std::string error;
            if (!pngSave(path, frame, reel.tone, &error)) {
                std::printf("[film] %s\n", error.c_str());
                return 1;
            }
            std::printf("  %s\n", path);
            ++index;
        }
        std::printf("[film] %d probes written\n", index);
        return 0;
    }

    // ------------------------------------------------------------ on the gpu
    //
    // These have to outlive the render, so they are declared here rather than
    // inside the branch: the reel holds the lambdas and the lambdas hold
    // references to the tracer.
    Window window;
    gpu::WavefrontTracer wave;
    gpu::Denoiser denoiser;
    const Scene* uploadedSet = nullptr;

    if (onGpu) {
        std::string werr;
        if (!window.create("film", 320, 200, &werr)) {
            std::printf("[film] window: %s\n", werr.c_str());
            return 1;
        }
        gl::ContextSettings ctx;
        ctx.samples = 0;
        void* context = nullptr;
        if (!gl::createContext(window.deviceContext(), ctx, &context, &werr)) {
            std::printf("[film] context: %s\n", werr.c_str());
            return 1;
        }
        std::printf("[film] gpu: %s\n", gl::rendererString().c_str());

        if (!wave.build(&werr) || !denoiser.build(&werr)) {
            std::printf("[film] gpu: %s\n", werr.c_str());
            return 1;
        }

        reel.renderer = [&](const Scene& s, const PathSettings& ps, RenderTargets& out) -> bool {
            // The world, the palette and the lights only change when the
            // film leaves the room, and the reel groups shots by set -- so
            // comparing the scene pointer is enough to know. Re-packing a
            // road set every frame would cost more than the trace.
            std::string err;
            if (uploadedSet != &s) {
                if (!wave.uploadStatic(s, &err)) {
                    std::printf("[film] gpu upload: %s\n", err.c_str());
                    return false;
                }
                uploadedSet = &s;
            }
            // The cast, the sprites and the props, which the shot rebuilt.
            if (!wave.uploadDynamic(s, &err)) {
                std::printf("[film] gpu upload: %s\n", err.c_str());
                return false;
            }

            Image image;
            if (!wave.render(s, ps, image, &out, nullptr, &err)) {
                std::printf("[film] gpu render: %s\n", err.c_str());
                return false;
            }
            return true;
        };

        reel.filter = [&](const RenderTargets& targets, const DenoiseSettings& ds) {
            Image filtered;
            std::string err;
            if (!denoiser.run(targets, ds, filtered, &err)) {
                std::printf("[film] gpu denoise: %s\n", err.c_str());
                return targets.color;
            }
            return filtered;
        };
    }

    std::string error;
    if (!reel.render(&error)) {
        std::printf("[film] %s\n", error.c_str());
        return 1;
    }
    return 0;
}
