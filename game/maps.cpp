#include "game/maps.hpp"
#include "game/blocks.hpp"

#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace game {

using namespace blocky;

namespace {

// Dry ground somewhere near a hint, spiralling outwards until it finds some.
//
// A named map knows roughly where it wants people to arrive and cannot know
// exactly: the ground under that point is whatever the noise did. Guessing a
// column and trusting it is how `be_valley` first put everyone on the riverbed
// with the water over their heads -- the search had one candidate, it failed,
// and the fallback was the origin, which is the bottom of the river.
//
// So the rule is the one `findSpawn` already follows for a generated world:
// the first solid thing from above is not necessarily the ground, and ground
// under water is not somewhere to stand.
bool dryGroundNear(const World& world, int x0, int z0, int waterLevel, int topY, int radius,
                   Vec3& out) {
    for (int r = 0; r <= radius; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;

                const int x = x0 + dx, z = z0 + dz;
                shape::SurfacePoint ground = shape::findSurface(world, x, z, topY);
                if (!ground.found) continue;
                if (ground.block.y <= waterLevel) continue;

                // Not a treetop, and with room to stand.
                if (ground.id != block::GrassBlock && ground.id != block::Sand &&
                    ground.id != block::Gravel && ground.id != block::Snow &&
                    ground.id != block::Stone)
                    continue;
                if (world.get({x, ground.block.y + 1, z}) != block::Air) continue;
                if (world.get({x, ground.block.y + 2, z}) != block::Air) continue;

                out = {float(x) + 0.5f, float(ground.block.y + 1), float(z) + 0.5f};
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- be_construct
//
// The floor is at this height rather than at zero so that a pit can be dug
// into it. A build yard with nothing below the ground is missing half of what
// people do with one.
constexpr int kYardFloor = 24;
constexpr int kYardHalf = 68;

// A flat slab with a grid ruled into it.
//
// The grid is not decoration. On an untextured plain there is no way to judge
// distance or the size of what you are building, and every sandbox map in the
// genre has some version of this for exactly that reason. Sixteen blocks
// apart, so a line is also a chunk boundary.
void buildYardFloor(World& world) {
    world.fillBox({-kYardHalf, kYardFloor - 4, -kYardHalf}, {kYardHalf, kYardFloor - 1, kYardHalf},
                  block::Stone);
    world.fillBox({-kYardHalf, kYardFloor, -kYardHalf}, {kYardHalf, kYardFloor, kYardHalf},
                  block::Stone);

    for (int i = -kYardHalf; i <= kYardHalf; ++i) {
        if (i % 16 != 0) continue;
        BlockId line = (i == 0) ? block::WhiteWool : block::Cobblestone;
        world.fillBox({i, kYardFloor, -kYardHalf}, {i, kYardFloor, kYardHalf}, line);
        world.fillBox({-kYardHalf, kYardFloor, i}, {kYardHalf, kYardFloor, i}, line);
    }
}

// A wall around the yard, so the edge of the world is somewhere rather than a
// cliff you walk off while looking at something else.
void buildYardWall(World& world) {
    const int h = kYardFloor + 7;
    world.fillBox({-kYardHalf, kYardFloor + 1, -kYardHalf}, {kYardHalf, h, -kYardHalf},
                  block::Bricks);
    world.fillBox({-kYardHalf, kYardFloor + 1, kYardHalf}, {kYardHalf, h, kYardHalf},
                  block::Bricks);
    world.fillBox({-kYardHalf, kYardFloor + 1, -kYardHalf}, {-kYardHalf, h, kYardHalf},
                  block::Bricks);
    world.fillBox({kYardHalf, kYardFloor + 1, -kYardHalf}, {kYardHalf, h, kYardHalf},
                  block::Bricks);

    // Lamps along the top, spaced so the yard is lit at night without the
    // wall becoming a strip light.
    for (int i = -kYardHalf + 8; i <= kYardHalf - 8; i += 16) {
        for (int side = -1; side <= 1; side += 2) {
            world.set({i, h, side * kYardHalf}, block::Glowstone);
            world.set({side * kYardHalf, h, i}, block::Glowstone);
        }
    }
}

// The staircase that answers "how high can I step".
//
// Steps of one, two and three blocks, side by side. The controller walks a
// block and refuses two, and this is where that is visible rather than
// written down: one column is walkable, the next needs a jump, the third
// cannot be climbed at all.
void buildStepTest(World& world, int x0, int z0) {
    const BlockId tread[3] = {block::OakPlanks, block::WhiteWool, block::RedWool};
    for (int lane = 0; lane < 3; ++lane) {
        const int rise = lane + 1;
        const int x = x0 + lane * 5;
        for (int step = 1; step <= 6; ++step) {
            const int top = kYardFloor + step * rise;
            world.fillBox({x, kYardFloor + 1, z0 + step * 2}, {x + 3, top, z0 + step * 2 + 1},
                          tread[lane]);
        }
    }
}

// A tower with a ledge every few blocks, for finding out what a fall costs.
void buildTower(World& world, int x0, int z0) {
    const int top = kYardFloor + 46;
    world.fillBox({x0 - 3, kYardFloor + 1, z0 - 3}, {x0 + 3, top, z0 + 3}, block::Cobblestone);

    // Hollowed, with a window band, so it reads as a building rather than a
    // pillar and so the inside is somewhere to stand.
    world.fillBox({x0 - 2, kYardFloor + 1, z0 - 2}, {x0 + 2, top - 1, z0 + 2}, block::Air);

    for (int y = kYardFloor + 8; y < top; y += 10) {
        // A platform sticking out on one side: the thing to jump off.
        world.fillBox({x0 + 4, y, z0 - 2}, {x0 + 9, y, z0 + 2}, block::OakPlanks);
        world.set({x0 + 9, y + 1, z0}, block::Glowstone);

        // Windows, one ring per landing.
        for (int side = -3; side <= 3; ++side) {
            world.set({x0 + side, y + 2, z0 - 3}, block::Glass);
            world.set({x0 + side, y + 2, z0 + 3}, block::Glass);
        }
    }
    world.fillBox({x0 - 3, top, z0 - 3}, {x0 + 3, top, z0 + 3}, block::IronBlock);
}

// A pool sunk into the floor, plus a deeper shaft beside it. Water is a medium
// in this renderer rather than a blue surface, and a pool with a shallow end
// and a deep end is where that becomes visible.
void buildPool(World& world, int x0, int z0) {
    world.fillBox({x0, kYardFloor - 3, z0}, {x0 + 22, kYardFloor, z0 + 22}, block::Air);
    world.fillBox({x0, kYardFloor - 4, z0}, {x0 + 22, kYardFloor - 4, z0 + 22}, block::Obsidian);

    // A shelf, so half of it is one block deep and half is four.
    world.fillBox({x0, kYardFloor - 3, z0}, {x0 + 10, kYardFloor - 1, z0 + 22}, block::Obsidian);
    world.fillBox({x0, kYardFloor - 3, z0}, {x0 + 22, kYardFloor, z0 + 22}, block::Water);

    // A rim, so nobody walks into it backwards.
    world.fillBox({x0 - 1, kYardFloor, z0 - 1}, {x0 + 23, kYardFloor, z0 - 1}, block::WhiteWool);
    world.fillBox({x0 - 1, kYardFloor, z0 + 23}, {x0 + 23, kYardFloor, z0 + 23}, block::WhiteWool);
    world.fillBox({x0 - 1, kYardFloor, z0 - 1}, {x0 - 1, kYardFloor, z0 + 23}, block::WhiteWool);
    world.fillBox({x0 + 23, kYardFloor, z0 - 1}, {x0 + 23, kYardFloor, z0 + 23}, block::WhiteWool);
}

// Material swatches: one column per block the player can place, in the order
// the hotbar holds them. Being able to see what a block looks like before
// building with it is the sort of thing a build yard is for.
void buildSwatches(World& world, int x0, int z0) {
    const BlockId palette[] = {block::Stone,     block::Cobblestone, block::OakPlanks,
                               block::Bricks,    block::Glass,       block::Glowstone,
                               block::RedWool,   block::WhiteWool,   block::IronBlock,
                               block::GoldBlock, block::Obsidian,    block::Sand};
    const int count = int(sizeof(palette) / sizeof(palette[0]));

    for (int i = 0; i < count; ++i) {
        world.fillBox({x0 + i * 3, kYardFloor + 1, z0}, {x0 + i * 3 + 1, kYardFloor + 4, z0 + 1},
                      palette[i]);
    }
}

// A ring of pillars around the spawn plaza, at heights that rise round the
// circle. Something to look at that is not a box, and something to test
// shadows against.
void buildRing(World& world, int x0, int z0) {
    for (int i = 0; i < 12; ++i) {
        const float angle = float(i) * (kTwoPi / 12.0f);
        const int x = x0 + int(std::round(std::cos(angle) * 20.0f));
        const int z = z0 + int(std::round(std::sin(angle) * 20.0f));
        const int height = 4 + i;
        shape::cylinder(world, {float(x), float(kYardFloor + 1), float(z)}, {0.0f, 1.0f, 0.0f},
                         1.6f, float(height), block::Stone);
        world.set({x, kYardFloor + 1 + height, z}, block::Glowstone);
    }
}

uint64_t buildConstruct(World& world, Vec3& spawn, float& yaw) {
    buildYardFloor(world);
    buildYardWall(world);

    // The plaza: a raised platform of planks where everyone arrives.
    world.fillBox({-6, kYardFloor + 1, -6}, {6, kYardFloor + 1, 6}, block::OakPlanks);
    world.fillBox({-6, kYardFloor + 2, -6}, {-6, kYardFloor + 2, 6}, block::Bricks);
    buildRing(world, 0, 0);

    buildStepTest(world, 26, -12);
    buildTower(world, -40, -36);
    buildPool(world, 24, 26);
    buildSwatches(world, -22, 30);

    // A pit, because a yard with no hole in it cannot show what falling into
    // one looks like. Lava at the bottom, lit and obvious.
    world.fillBox({-40, kYardFloor - 12, 20}, {-22, kYardFloor, 38}, block::Air);

    // A floor under the lava, and it is not decoration. A fluid is something
    // you fall *through*, so two blocks of lava over nothing was a hole out of
    // the bottom of the world -- which nobody noticed while there was nothing
    // to notice it with.
    world.fillBox({-40, kYardFloor - 13, 20}, {-22, kYardFloor - 13, 38}, block::Obsidian);
    world.fillBox({-40, kYardFloor - 12, 20}, {-22, kYardFloor - 11, 38}, block::Lava);

    spawn = {0.5f, float(kYardFloor + 2), 0.5f};
    yaw = 0.0f;   // looking towards -Z, down the middle of the yard
    return world.blockCount();
}

// -------------------------------------------------------------------- be_lake

uint64_t buildLake(World& world, uint32_t seed, Vec3& spawn, float& yaw) {
    const int extent = 90;
    const int water = 30;

    for (int z = -extent; z <= extent; ++z) {
        for (int x = -extent; x <= extent; ++x) {
            const float distance = std::sqrt(float(x * x + z * z));

            // A bowl: the ground drops towards the middle and climbs to a rim.
            // Written as a curve rather than as noise so the lake is always a
            // lake -- noise alone gives a puddle about half the time.
            const float t = std::min(1.0f, distance / float(extent));

            // Gentle enough to walk down. A steeper curve makes a more
            // dramatic bowl and a worse map: every slope becomes a staircase
            // of one-block terraces that the character can climb but nobody
            // wants to look at.
            const float bowl = -13.0f * (1.0f - t * t * 2.0f) + 15.0f * t * t * t;

            const float wobble = noise::fbm2(float(x) * 0.021f, float(z) * 0.021f, 4, seed) * 5.0f +
                                 noise::fbm2(float(x) * 0.06f, float(z) * 0.06f, 3, seed + 3u) * 2.0f;
            int h = int(float(water) + bowl + wobble);
            h = std::max(1, h);

            for (int y = 0; y < h - 3; ++y) world.set({x, y, z}, block::Stone);
            for (int y = std::max(0, h - 3); y < h; ++y) world.set({x, y, z}, block::Dirt);

            BlockId surface = block::GrassBlock;
            if (h <= water + 2) surface = block::Sand;
            if (h <= water - 4) surface = block::Gravel;
            world.set({x, h, z}, surface);

            for (int y = h + 1; y <= water; ++y) world.set({x, y, z}, block::Water);
        }
    }

    // An island, so the middle of the lake is somewhere to swim to.
    shape::ellipsoid(world, {0.0f, float(water) - 1.0f, 0.0f}, {9.0f, 6.0f, 9.0f}, block::Sand);
    world.fillBox({-3, water + 1, -3}, {3, water + 1, 3}, block::GrassBlock);
    growTree(world, {0, water + 2, 0}, tree::oak(block::OakLog, block::OakLeaves), seed + 3u);

    // Forest on the rim only: trees standing in the shallows look like a
    // mistake rather than like a shore.
    std::vector<Vec2> spots = poissonDisk({float(-extent) + 3.0f, float(-extent) + 3.0f},
                                          {float(extent) - 3.0f, float(extent) - 3.0f}, 8.0f,
                                          seed + 11u);
    uint32_t which = 0;
    for (Vec2 spot : spots) {
        const int x = int(spot.x), z = int(spot.y);
        shape::SurfacePoint ground = shape::findSurface(world, x, z, 120);
        if (!ground.found || ground.id != block::GrassBlock) continue;
        if (ground.block.y <= water + 3) continue;

        TreeParams params = (which % 3 == 0)   ? tree::spruce(block::SpruceLog, block::SpruceLeaves)
                            : (which % 3 == 1) ? tree::birch(block::BirchLog, block::BirchLeaves)
                                               : tree::oak(block::OakLog, block::OakLeaves);
        growTree(world, {x, ground.block.y + 1, z}, params, seed + which * 17u);
        ++which;
    }

    // Arrive on the shore looking across the water, which is the view the map
    // exists for.
    if (!dryGroundNear(world, 0, -(extent * 2) / 3, water, 130, 40, spawn))
        spawn = {0.5f, float(water + 12), float(-extent / 2) + 0.5f};
    yaw = 180.0f;   // -Z is away from the middle, so turn round to face it
    return world.blockCount();
}

// ------------------------------------------------------------------ be_valley

uint64_t buildValley(World& world, uint32_t seed, Vec3& spawn, float& yaw) {
    const int halfWidth = 76;
    const int halfLength = 100;
    const int riverLevel = 26;

    for (int z = -halfLength; z <= halfLength; ++z) {
        for (int x = -halfWidth; x <= halfWidth; ++x) {
            // The valley runs along Z, so the shape is a function of x alone
            // and the noise is what keeps it from being an extrusion.
            const float across = std::fabs(float(x)) / float(halfWidth);
            const float walls = 46.0f * across * across;

            // The river wanders. Without this the water is a canal, and a
            // canal reads as built rather than as found.
            const float bend = noise::fbm2(0.0f, float(z) * 0.012f, 3, seed + 5u) * 22.0f;
            const float toRiver = std::fabs(float(x) - bend) / float(halfWidth);
            const float channel = -9.0f * std::max(0.0f, 1.0f - toRiver * 7.0f);

            const float rough =
                noise::ridged2(float(x) * 0.018f, float(z) * 0.018f, 4, seed + 31u) * 12.0f * across;
            const float wobble = noise::fbm2(float(x) * 0.03f, float(z) * 0.03f, 4, seed) * 4.0f;

            int h = int(float(riverLevel) + 4.0f + walls + channel + rough + wobble);
            h = std::max(1, h);

            for (int y = 0; y < h - 3; ++y) world.set({x, y, z}, block::Stone);
            for (int y = std::max(0, h - 3); y < h; ++y) world.set({x, y, z}, block::Dirt);

            BlockId surface = block::GrassBlock;
            if (h <= riverLevel + 1) surface = block::Sand;
            // Bare rock high on the walls, and snow above that. A valley whose
            // ridges are grass to the top is a hill.
            if (h > riverLevel + 34) surface = block::Stone;
            if (h > riverLevel + 44) surface = block::Snow;
            world.set({x, h, z}, surface);

            for (int y = h + 1; y <= riverLevel; ++y) world.set({x, y, z}, block::Water);
        }
    }

    std::vector<Vec2> spots =
        poissonDisk({float(-halfWidth) + 3.0f, float(-halfLength) + 3.0f},
                    {float(halfWidth) - 3.0f, float(halfLength) - 3.0f}, 7.0f, seed + 23u);
    uint32_t which = 0;
    for (Vec2 spot : spots) {
        const int x = int(spot.x), z = int(spot.y);
        shape::SurfacePoint ground = shape::findSurface(world, x, z, 160);
        if (!ground.found || ground.id != block::GrassBlock) continue;

        // Not on a slope steep enough to leave the trunk hanging.
        if (shape::surfaceRoughness(world, x, z, 160, 2) > 4) continue;

        // Spruce up high, oak and birch down by the water. Free, and it makes
        // the height of the valley readable from the trees alone.
        TreeParams params = ground.block.y > riverLevel + 20 ? tree::spruce(block::SpruceLog, block::SpruceLeaves)
                            : (which % 2 == 0)         ? tree::oak(block::OakLog, block::OakLeaves)
                                                       : tree::birch(block::BirchLog, block::BirchLeaves);
        growTree(world, {x, ground.block.y + 1, z}, params, seed + which * 19u);
        ++which;
    }

    // On the bank, part way up the eastern slope, looking across at the water.
    if (!dryGroundNear(world, 34, 0, riverLevel + 1, 170, 40, spawn))
        spawn = {34.5f, float(riverLevel + 20), 0.5f};
    yaw = 90.0f;   // across the valley, towards the river
    return world.blockCount();
}

const MapInfo kInfo[] = {
    {"random", "NEW WORLD", "A FRESH LANDSCAPE FROM THE SEED"},
    {"be_construct", "BE_CONSTRUCT", "FLAT BUILD YARD, TOWER, POOL, STEPS"},
    {"be_lake", "BE_LAKE", "A BOWL OF WATER RINGED WITH FOREST"},
    {"be_valley", "BE_VALLEY", "A RIVER BETWEEN TWO RIDGES"},
};

} // namespace

const MapInfo& mapInfo(MapKind kind) { return kInfo[int(kind)]; }

MapKind mapFromName(const std::string& name) {
    for (int i = 0; i < kMapCount; ++i) {
        const MapInfo& info = kInfo[i];
        if (name == info.id) return MapKind(i);

        // "lake" as well as "be_lake": the prefix is there to look like the
        // genre, not to be typed.
        const char* bare = std::strncmp(info.id, "be_", 3) == 0 ? info.id + 3 : info.id;
        if (name == bare) return MapKind(i);
    }
    return MapKind::Random;
}

uint64_t buildMap(World& world, MapKind kind, const TerrainSettings& settings, Vec3& spawn,
                  float& spawnYawDegrees) {
    spawnYawDegrees = 0.0f;

    switch (kind) {
        case MapKind::Construct: return buildConstruct(world, spawn, spawnYawDegrees);
        case MapKind::Lake:      return buildLake(world, settings.seed, spawn, spawnYawDegrees);
        case MapKind::Valley:    return buildValley(world, settings.seed, spawn, spawnYawDegrees);

        case MapKind::Random:
        default: {
            const uint64_t blocks = generateTerrain(world, settings);
            spawn = findSpawn(world, settings);
            return blocks;
        }
    }
}

} // namespace game
