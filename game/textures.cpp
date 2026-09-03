#include "game/textures.hpp"

#include "engine/core/png.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"

#include "game/blocks.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game {
namespace {

using namespace blocky;

// Each block gets a row: top, side, bottom, magnified so a single texel is
// visible. A texture is easier to judge next to its neighbours than alone.
ImageU8 contactSheet(const BlockTextureLibrary& library, const BlockRegistry& registry, int tile,
                     int zoom) {
    struct Row { BlockId id; };
    std::vector<Row> rows;
    for (BlockId id = 0; id < BlockId(registry.size()); ++id)
        if (library.hasTexture(id, FacePosY) || library.hasTexture(id, FaceNegZ)) rows.push_back({id});

    const int pad = 4;
    const int cell = tile * zoom;
    const int width = pad + 3 * (cell + pad);
    const int height = pad + int(rows.size()) * (cell + pad);

    ImageU8 sheet(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) sheet.set(x, y, {28, 30, 34, 255});

    const int faces[3] = {FacePosY, FaceNegZ, FaceNegY};

    for (size_t r = 0; r < rows.size(); ++r) {
        for (int column = 0; column < 3; ++column) {
            int originX = pad + column * (cell + pad);
            int originY = pad + int(r) * (cell + pad);

            for (int y = 0; y < cell; ++y) {
                for (int x = 0; x < cell; ++x) {
                    Vec2 uv{(float(x) + 0.5f) / float(cell), (float(y) + 0.5f) / float(cell)};
                    Vec3 linear = library.sampleAlbedo(rows[r].id, faces[column], uv,
                                                       registry[rows[r].id].albedo);
                    Vec3 encoded = linearToSrgb(minv(maxv(linear, Vec3{0.0f}), Vec3{1.0f}));
                    sheet.set(originX + x, originY + y,
                              {uint8_t(encoded.x * 255.0f + 0.5f), uint8_t(encoded.y * 255.0f + 0.5f),
                               uint8_t(encoded.z * 255.0f + 0.5f), 255});
                }
            }
        }
    }
    return sheet;
}

// A small piece of ground with one of everything standing on it, so the
// textures are judged the way they will be seen: lit, at an angle, next to
// each other.
void buildShowcase(World& world) {
    world.fillBox({-14, -3, -14}, {14, -1, 14}, block::Stone);
    world.fillBox({-14, 0, -14}, {14, 0, 14}, block::GrassBlock);

    // A ridge, so there are dirt sides and grass fringes to look at.
    for (int z = -14; z <= 14; ++z) {
        for (int x = -14; x <= 14; ++x) {
            // fbm2 runs about [-1, 1]; the ridge wants [0, 1].
            float n = noise::fbm2(float(x) * 0.11f, float(z) * 0.11f, 3, 7u) * 0.5f + 0.5f;
            int h = int(n * 4.0f);
            for (int y = 1; y <= h; ++y) world.set({x, y, z}, block::Dirt);
            if (h >= 1) world.set({x, h, z}, block::GrassBlock);
        }
    }

    const BlockId kRow[] = {block::Cobblestone, block::Bricks,   block::OakPlanks, block::Sand,
                            block::Gravel,      block::Snow,     block::IronBlock, block::GoldBlock,
                            block::RedWool,     block::WhiteWool, block::Obsidian, block::Netherrack};

    // A row of pillars: two blocks each, spaced so the sides are all visible.
    int x = -11;
    for (BlockId id : kRow) {
        world.fillBox({x, 4, -2}, {x + 1, 5, -1}, id);
        x += 2;
    }

    // Three trees, for bark and leaves.
    growTree(world, {-7, 1, 6}, tree::oak(block::OakLog, block::OakLeaves), 11u);
    growTree(world, {0, 1, 9}, tree::birch(block::BirchLog, block::BirchLeaves), 23u);
    growTree(world, {7, 1, 6}, tree::spruce(block::SpruceLog, block::SpruceLeaves), 37u);

    world.fillBox({-3, 1, -6}, {2, 3, -6}, block::Glass);
    world.set({-1, 1, -9}, block::Glowstone);
}

} // namespace

int runTextures(int argc, char** argv) {
    bool sheetOnly = false;
    texgen::Settings settings;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "sheet") == 0) sheetOnly = true;
        else if (std::strncmp(argv[i], "size=", 5) == 0) settings.size = std::atoi(argv[i] + 5);
    }

    Scene scene(palette());
    BlockTextureLibrary library;

    int textured = generateTextures(library, scene.world.registry(), settings);
    scene.blockTextures = &library;

    std::printf("[textures] generated %d blocks at %d x %d, tile size %d\n", textured, settings.size,
                settings.size, library.tileSize());

    int zoom = std::max(1, 96 / settings.size);
    ImageU8 sheet = contactSheet(library, scene.world.registry(), settings.size, zoom);
    pngSave("out/textures_sheet.png", sheet, nullptr);
    std::printf("[textures] wrote out/textures_sheet.png (%d x %d)\n", sheet.width(), sheet.height());
    if (sheetOnly) return 0;

    buildShowcase(scene.world);

    scene.camera.lookAt({16.0f, 11.0f, 19.0f}, {-1.0f, 2.5f, 0.0f});
    scene.camera.fovY = radians(46.0f);
    scene.camera.aspect = 1100.0f / 640.0f;

    scene.sun.direction = normalize(Vec3{-0.38f, 0.74f, 0.55f});
    scene.sun.color = {1.0f, 0.95f, 0.86f};
    // A full-frame ground plane faces the sun square on, so it takes the most
    // light anything in the scene can take. At 6 the grass clipped towards the
    // green primary and read as neon -- which looked like a texture fault and
    // was a lighting one.
    scene.sun.intensity = 3.6f;
    scene.sun.angularRadiusDegrees = 1.0f;
    scene.ambientStrength = 0.7f;

    PathSettings settingsPath;
    settingsPath.width = 1100;
    settingsPath.height = 640;
    settingsPath.samplesPerPixel = 64;
    settingsPath.maxBounces = 6;

    RenderStats stats;
    RenderTargets targets;
    Image frame = renderPath(scene, settingsPath, &stats, &targets);
    frame = denoise(targets, {});

    GradeSettings grade;
    grade.contrast = 1.04f;
    grade.saturation = 1.04f;
    applyGrade(frame, grade);
    applyVignette(frame, {});

    ToneParams tone;
    tone.curve = Tonemap::ACES;
    pngSave("out/textures.png", frame, tone, nullptr);
    std::printf("[textures] wrote out/textures.png\n");
    return 0;
}

} // namespace game
