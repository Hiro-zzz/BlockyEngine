// Particles and floating text, both of them sprites.
//
// The point of the scene is that none of it is composited on afterwards. The
// dust in the shafts is lit by those shafts -- it is bright where the sun
// reaches it and dark two blocks away, because it is being traced, not drawn.
// The embers over the lava glow and land in the water's reflection. The label
// is geometry: it faces the camera because it was turned to face it once,
// while the scene was being built, and from then on it behaves like any other
// surface.
//
// A roofed hall rather than an open field on purpose. A shaft of light is
// only a shaft against something darker, and out in the sun there is nothing
// darker to see it against.
//
//   scene_sprites          full quality
//   scene_sprites draft    small and fast, for iterating on the lighting
//   scene_sprites bare     ignore game assets, to check the fallbacks
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/sprite/font.hpp"
#include "engine/sprite/scatter.hpp"
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

constexpr uint32_t kSeed = 31337u;

// The hall. Open towards +Z, which is where the camera stands.
constexpr int kHalfX = 19;
constexpr int kBackZ = -16;
constexpr int kFrontZ = 14;
constexpr int kRoofY = 17;

// Steep enough that a shaft from the roof reaches the floor inside the room
// rather than running out through the open front.
const Vec3 kSun = normalize(Vec3{0.22f, 0.90f, 0.30f});

// Where the roof is opened up, and therefore where the shafts come down.
const Vec3 kHoles[] = {{-10.0f, float(kRoofY), 0.0f},
                       {4.0f, float(kRoofY), 6.0f},
                       {14.0f, float(kRoofY), -4.0f}};

void buildHall(World& world) {
    // Floor, with a little depth so the trenches have something to cut into.
    world.fillBox({-kHalfX - 2, -4, kBackZ - 2}, {kHalfX + 2, 0, kFrontZ + 2}, palette::Stone);
    shape::replace(world, {-kHalfX, 0, kBackZ}, {kHalfX, 0, kFrontZ}, palette::Stone,
                   palette::Cobblestone);

    // Three walls and a roof; the fourth side is left open.
    world.fillBox({-kHalfX - 2, 1, kBackZ - 2}, {kHalfX + 2, kRoofY, kBackZ}, palette::Cobblestone);
    world.fillBox({-kHalfX - 2, 1, kBackZ}, {-kHalfX, kRoofY, kFrontZ + 2}, palette::Cobblestone);
    world.fillBox({kHalfX, 1, kBackZ}, {kHalfX + 2, kRoofY, kFrontZ + 2}, palette::Cobblestone);
    world.fillBox({-kHalfX - 2, kRoofY, kBackZ - 2}, {kHalfX + 2, kRoofY + 1, kFrontZ + 2},
                  palette::Cobblestone);

    // A brick course along the wall tops, to break up the grey.
    world.fillBox({-kHalfX - 2, kRoofY - 1, kBackZ - 2}, {kHalfX + 2, kRoofY - 1, kBackZ},
                  palette::Bricks);

    // Open the roof where the light is meant to come through.
    for (Vec3 hole : kHoles) {
        shape::ellipsoid(world, {hole.x, float(kRoofY) + 0.5f, hole.z}, {3.0f, 2.5f, 3.0f},
                         palette::Air);
    }

    // Pillars, eroded so they read as a ruin.
    // Only down the sides: a centre row puts a pillar in front of every
    // thing worth looking at.
    const float pillarX[] = {-13.5f, 13.5f};
    const float pillarZ[] = {-10.0f, 1.0f, 10.0f};
    for (float x : pillarX) {
        for (float z : pillarZ) {
            shape::cylinder(world, {x, 1.0f, z}, {0, 1, 0}, 1.7f, float(kRoofY) - 1.0f,
                            palette::Cobblestone);
            shape::torus(world, {x, float(kRoofY) - 2.0f, z}, {0, 1, 0}, 2.0f, 0.7f,
                         palette::Bricks);
        }
    }
    shape::erode(world, {-16, 10, -12}, {16, kRoofY - 2, 9}, 3, 1);

    // A lava pool in the back left corner. It was a channel across the whole
    // back wall at first, which lit every mote in the room orange and left
    // the shafts nothing to stand out against.
    world.fillBox({-15, -1, -13}, {-4, 0, -9}, palette::Air);
    world.fillBox({-15, -1, -13}, {-4, 0, -9}, palette::Lava);

    // A still pool towards the front, to catch reflections.
    world.fillBox({-7, -2, 2}, {7, 0, 9}, palette::Air);
    world.fillBox({-7, -2, 2}, {7, 0, 9}, palette::Water);
    shape::replace(world, {-9, 0, 0}, {9, 0, 11}, palette::Cobblestone, palette::Sand);

    // A couple of lanterns so the corners are not pure black.
    world.set({-kHalfX + 1, 12, -14}, palette::Glowstone);
    world.set({kHalfX - 1, 12, -14}, palette::Glowstone);
}

bool loadParticle(const AssetSource& source, const char* name, Texture& out) {
    std::string path = std::string("assets/minecraft/textures/particle/") + name;
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, nullptr)) return false;
    if (!out.loadFromPng(bytes.data(), bytes.size(), nullptr)) return false;
    out.cropToFirstSquareFrame();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false, bare = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        if (std::strcmp(argv[i], "bare") == 0) bare = true;
    }

    Scene scene(palette::registry());
    buildHall(scene.world);

    // ------------------------------------------------------------- assets
    AssetSource source;
    BlockTextureLibrary blockTextures;
    Texture dustTexture, emberTexture;
    Font font;

    std::string jar;
    bool haveAssets = false;
    if (!bare) {
        jar = AssetSource::findClientJar();
        haveAssets = !jar.empty() && source.open(jar, nullptr);
    }

    if (haveAssets) {
        if (blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
            scene.blockTextures = &blockTextures;
        }
        loadParticle(source, "generic_0.png", dustTexture);
        loadParticle(source, "flame.png", emberTexture);
        if (!font.loadFromSource(source, Font::kMinecraftAscii, nullptr)) font.useBuiltin();
        std::printf("[sprites] assets: %s\n", jar.c_str());
        std::printf("[sprites] font %s, dust %s, ember %s\n", font.isBuiltin() ? "built-in" : "game",
                    dustTexture.empty() ? "flat" : "game",
                    emberTexture.empty() ? "flat" : "game");
    } else {
        font.useBuiltin();
        std::printf("[sprites] no game assets: flat palette and the built-in font\n");
    }

    // -------------------------------------------------------------- camera
    const Vec3 eye{8.0f, 10.0f, 39.0f};
    const Vec3 focus{-3.0f, 6.5f, -6.0f};

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(44.0f);
    scene.camera.lookAt(eye, focus);
    scene.camera.aperture = draft ? 0.0f : 0.22f;
    scene.camera.focusOn({0.0f, 6.0f, -2.0f});

    // --------------------------------------------------------------- light
    // Low and warm outside, dim inside: a shaft only reads against dark.
    scene.sun.direction = kSun;
    scene.sun.color = {1.0f, 0.83f, 0.60f};
    scene.sun.intensity = 27.0f;
    scene.sun.angularRadiusDegrees = 0.9f;

    scene.sky.zenith = {0.07f, 0.13f, 0.30f};
    scene.sky.horizon = {0.36f, 0.38f, 0.52f};
    scene.sky.intensity = 0.7f;
    scene.ambientStrength = 0.45f;

    // ------------------------------------------------------------- sprites
    SpriteSet sprites;

    // Dust down each shaft. Placed along the sun direction from each hole, so
    // the cylinder of motes sits exactly where the light is -- and the ones
    // that stray under the roof get shadowed by it, for free.
    {
        scatter::ParticleStyle style;
        style.size = {0.075f, 0.075f};
        style.sizeJitter = 0.6f;
        // Flat, not textured. The game's dust sprite is mostly soft alpha, and
        // the cutout eats it down to a few texels -- which is right for a
        // glyph and wrong for a mote that is supposed to catch the light.
        style.texture = nullptr;
        style.tint = {1.0f, 0.95f, 0.84f};
        style.randomYaw = false;
        style.randomRoll = true;

        for (size_t i = 0; i < sizeof(kHoles) / sizeof(kHoles[0]); ++i) {
            std::vector<Sprite> motes =
                scatter::inBeam(kHoles[i] + Vec3{0.0f, 3.0f, 0.0f}, -kSun, 26.0f, 2.6f,
                                draft ? 900 : 2600, kSeed + uint32_t(i) * 17u, style);
            scatter::removeInsideSolid(motes, scene.world);

            // Turned to the camera and frozen, exactly as the label is. Left
            // at random angles most of them present an edge or a shadowed
            // back, and the shaft never gathers into a shaft.
            aimAt(motes, eye);
            sprites.add(motes);
        }
    }

    // A thin haze through the whole room, so the shafts have something to be
    // brighter than.
    {
        scatter::ParticleStyle style;
        style.size = {0.07f, 0.07f};
        style.sizeJitter = 0.6f;
        style.texture = nullptr;
        style.tint = {0.82f, 0.86f, 1.0f};
        style.randomYaw = false;
        style.randomRoll = true;

        std::vector<Sprite> haze =
            scatter::inBox({-17.0f, 1.0f, -15.0f}, {17.0f, float(kRoofY) - 1.0f, 13.0f},
                           draft ? 260 : 900, kSeed + 2u, style);
        scatter::removeInsideSolid(haze, scene.world);
        aimAt(haze, eye);
        sprites.add(haze);
    }

    // Embers off the lava. They carry emission, so they glow -- though they
    // light only themselves; the light list is built from the voxel world.
    {
        scatter::ParticleStyle style;
        style.size = {0.10f, 0.10f};
        style.sizeJitter = 0.5f;
        style.texture = emberTexture.empty() ? nullptr : &emberTexture;
        style.tint = {1.0f, 0.52f, 0.16f};
        style.emission = {11.0f, 3.8f, 0.8f};

        sprites.add(scatter::above(scene.world, {-15, 0, -13}, {-4, 0, -9}, palette::Lava, 7.0f,
                                   draft ? 2 : 5, kSeed + 3u, style));
    }

    // The label, turned to face the camera once and frozen there.
    {
        TextStyle style;
        style.height = 1.4f;
        style.color = srgbToLinear(Vec3{0.98f, 0.92f, 0.78f});
        style.emission = {1.1f, 0.92f, 0.62f};
        style.lineSpacing = 1.4f;

        sprites.add(text::facing(font, "EMBER HALL", {-3.0f, 12.5f, 0.0f}, eye, style));

        // Well in front of the haze box, which reaches z = 13. Standing inside
        // it the label is legible in isolation and unreadable in the render:
        // several hundred lit motes end up between it and the camera.
        TextStyle small = style;
        small.height = 0.62f;
        small.emission = {0.5f, 0.44f, 0.32f};
        sprites.add(text::facing(font, "every mote is traced\nnothing is composited",
                                 {0.0f, 3.4f, 13.5f}, eye, small));
    }

    sprites.build();
    scene.sprites = &sprites;

    std::printf("[sprites] %zu quads, bvh depth %d over %zu nodes\n", sprites.size(),
                sprites.bvh().maxDepth(), sprites.bvh().nodeCount());

    // -------------------------------------------------------------- render
    PathSettings settings;
    settings.width = draft ? 700 : 1500;
    settings.height = draft ? 440 : 940;
    settings.samplesPerPixel = draft ? 32 : 160;
    settings.maxBounces = draft ? 4 : 8;

    RenderStats stats;
    RenderTargets targets;
    Image frame = renderPath(scene, settings, &stats, &targets);
    frame = denoise(targets, {});

    BloomSettings bloom;
    bloom.threshold = 1.05f;
    bloom.intensity = 0.10f;
    applyBloom(frame, bloom);

    GradeSettings grade;
    grade.contrast = 1.06f;
    grade.saturation = 1.08f;
    grade.temperature = 0.10f;
    applyGrade(frame, grade);

    applyVignette(frame, {});
    applyGrain(frame, 0.010f);

    ToneParams tone;
    tone.curve = Tonemap::ACES;

    const char* path = draft ? "out/sprites_draft.png" : "out/sprites.png";
    pngSave(path, frame, tone, nullptr);
    std::printf("[sprites] wrote %s\n", path);
    return 0;
}
