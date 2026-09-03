#pragma once
// The three places this film happens in, plus the void the cards hang in.
//
// A set is a World and nothing else. Light, camera and cast belong to the
// shot, because a set is stood in more than once and never twice the same
// way -- the diner is lit one way while it is open and another once the
// lamps are the only thing left on.
//
// Sizes are in blocks, and a block is a metre. That rule keeps proving
// itself: the counter is one block tall because that is waist height on a
// two-block character, and anything that wants to be smaller than a block --
// a cup, a tray, a lantern -- is a prop, not a block.
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

#include <cmath>

namespace film {

using namespace blocky;

// ------------------------------------------------------------ extra blocks
//
// Added to the global palette, which is what every World in this film shares.
// None of them is in the built-in texture table, so without a bind() they
// render with their flat albedo -- which for the two lights is exactly what
// is wanted: a pure colour that owes nothing to a texture.
struct Palette {
    BlockId asphalt = 0;
    BlockId wall = 0;         // the diner's off-white panelling
    BlockId redPanel = 0;
    BlockId yellowPanel = 0;
    BlockId tileDark = 0;
    BlockId warmGlow = 0;     // the diner's lamps
    BlockId magentaGlow = 0;  // the other thing's light
    BlockId dimGlow = 0;      // a lamp seen from a long way off
};

inline Palette registerBlocks() {
    BlockRegistry& reg = palette::registry();
    Palette p;

    auto srgbOf = [](int r, int g, int b) {
        return srgbToLinear(Vec3{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f});
    };

    // Adding the same names twice would double the palette on a second call,
    // and every scene here calls this once -- but a find() first costs
    // nothing and makes the function safe to call again.
    auto add = [&](const char* name, BlockDef def) {
        const BlockId existing = reg.find(name);
        if (existing != palette::Air) return existing;
        def.name = name;
        return reg.add(def);
    };

    BlockDef asphalt;
    asphalt.albedo = srgbOf(46, 46, 50);
    asphalt.roughness = 0.92f;
    p.asphalt = add("film_asphalt", asphalt);

    BlockDef wall;
    wall.albedo = srgbOf(206, 202, 191);
    wall.roughness = 0.85f;
    p.wall = add("film_wall", wall);

    BlockDef red;
    red.albedo = srgbOf(178, 34, 34);
    red.roughness = 0.62f;
    p.redPanel = add("film_red", red);

    BlockDef yellow;
    yellow.albedo = srgbOf(240, 186, 30);
    yellow.roughness = 0.60f;
    p.yellowPanel = add("film_yellow", yellow);

    BlockDef tile;
    tile.albedo = srgbOf(62, 60, 64);
    tile.roughness = 0.45f;
    p.tileDark = add("film_tile", tile);

    // The lamps. Emission is radiance and may exceed one; glowstone sits at
    // nine, so these are read against that.
    BlockDef warm;
    warm.albedo = srgbOf(255, 240, 214);
    warm.emission = srgbOf(255, 226, 178) * 7.5f;
    p.warmGlow = add("film_warm", warm);

    BlockDef magenta;
    magenta.albedo = srgbOf(255, 170, 240);
    magenta.emission = srgbOf(255, 92, 226) * 6.5f;
    p.magentaGlow = add("film_magenta", magenta);

    BlockDef dim;
    dim.albedo = srgbOf(255, 236, 200);
    dim.emission = srgbOf(255, 220, 170) * 2.4f;
    p.dimGlow = add("film_dim", dim);

    return p;
}

// ================================================================ the diner
//
// One building, built whole: the shots stand inside it and outside it, and a
// door you can walk out through is cheaper than two sets that have to agree
// with each other about where the door was.
//
//   interior   x in [-8, 8], z in [-6, 6]
//   floor top  y = 0          walls y = 0..3        ceiling y = 4
//   windows    z = -6, y = 1..2
//   door       x in [3, 4], z = -6, y = 0..1
//   counter    z in [2, 3], y = 0 -- so its top surface is y = 1
//
// The crew stands behind the counter at z = 4 and faces -Z; customers are at
// z = 0..1.
namespace diner {

inline constexpr int kFloorY = -1;
inline constexpr int kCeilY = 4;
inline constexpr int kFrontZ = -6;
inline constexpr int kBackZ = 6;
inline constexpr int kLeftX = -9;
inline constexpr int kRightX = 9;

inline constexpr float kCounterTop = 1.0f;
inline constexpr int kDoorMinX = 3;
inline constexpr int kDoorMaxX = 4;

inline void build(World& world, const Palette& pal) {
    // ---- the plain it stands on. Dark, and big enough that a wide shot
    // never finds its edge.
    world.fillBox({-90, kFloorY - 1, -90}, {90, kFloorY - 1, 90}, palette::Stone);
    world.fillBox({-90, kFloorY, -90}, {90, kFloorY, 90}, palette::Gravel);

    // A road running east-west in front, with a kerb.
    world.fillBox({-90, kFloorY, -16}, {90, kFloorY, -11}, pal.asphalt);
    world.fillBox({-90, kFloorY, -10}, {90, kFloorY, -10}, palette::Stone);
    world.fillBox({-90, kFloorY, -17}, {90, kFloorY, -17}, palette::Stone);
    // Centre line, dashed.
    for (int x = -90; x <= 90; x += 6) {
        world.fillBox({x, kFloorY, -13}, {x + 2, kFloorY, -13}, pal.yellowPanel);
    }

    // Forecourt: paved between the road and the door.
    world.fillBox({-12, kFloorY, -10}, {12, kFloorY, kFrontZ}, palette::Stone);

    // ---- shell
    world.fillBox({kLeftX, kFloorY, kFrontZ}, {kRightX, kFloorY, kBackZ}, pal.tileDark);
    // A chequer, because a flat floor under a path tracer is a grey field and
    // the eye needs something to measure the room by.
    shape::fillWhereBlock(world, {kLeftX, kFloorY, kFrontZ}, {kRightX, kFloorY, kBackZ},
                          palette::WhiteWool,
                          [](IVec3 p) { return ((p.x >> 1) + (p.z >> 1)) % 2 == 0; });

    world.fillBox({kLeftX, 0, kFrontZ}, {kLeftX, 3, kBackZ}, pal.wall);
    world.fillBox({kRightX, 0, kFrontZ}, {kRightX, 3, kBackZ}, pal.wall);
    world.fillBox({kLeftX, 0, kBackZ}, {kRightX, 3, kBackZ}, pal.wall);
    world.fillBox({kLeftX, 0, kFrontZ}, {kRightX, 3, kFrontZ}, pal.wall);

    // The red band is the outside of the building and must not be the inside
    // of the room. A block carries one material on all six faces, so the two
    // cannot be the same block: the fascia is an overhanging ring one block
    // wider than the walls, and the ceiling is laid inside it afterwards.
    //
    // The first version painted the band onto the top course of the wall
    // itself, and every interior shot came back with a red ceiling and red
    // bounce over everything under it.
    world.fillBox({kLeftX - 1, kCeilY, kFrontZ - 1}, {kRightX + 1, kCeilY, kBackZ + 1},
                  pal.redPanel);
    world.fillBox({kLeftX, kCeilY, kFrontZ}, {kRightX, kCeilY, kBackZ}, pal.tileDark);

    // ---- glazing and door in the front wall
    world.fillBox({-8, 1, kFrontZ}, {1, 2, kFrontZ}, palette::Glass);
    world.fillBox({6, 1, kFrontZ}, {8, 2, kFrontZ}, palette::Glass);
    world.fillBox({kDoorMinX, 0, kFrontZ}, {kDoorMaxX, 1, kFrontZ}, palette::Air);
    // Door frame, so the gap reads as a door rather than as a hole.
    world.fillBox({kDoorMinX - 1, 0, kFrontZ}, {kDoorMinX - 1, 2, kFrontZ}, pal.redPanel);
    world.fillBox({kDoorMaxX + 1, 0, kFrontZ}, {kDoorMaxX + 1, 2, kFrontZ}, pal.redPanel);
    world.fillBox({kDoorMinX, 2, kFrontZ}, {kDoorMaxX, 2, kFrontZ}, pal.redPanel);

    // A window in the left wall too, so the corner booth is not in a box.
    world.fillBox({kLeftX, 1, -4}, {kLeftX, 2, -1}, palette::Glass);

    // ---- lamps, recessed in the ceiling
    const int lampX[] = {-7, -4, -1, 2, 5, 8};
    const int lampZ[] = {-5, -2, 1, 4};
    for (int x : lampX) {
        for (int z : lampZ) world.set({x, kCeilY, z}, pal.warmGlow);
    }

    // ---- counter. One block tall, because that is waist height on a
    // two-block character; two would be a wall with a person behind it.
    // Kept off-white: a red counter front runs the whole width of every
    // interior shot, and one wall-sized accent is enough for a room.
    world.fillBox({-7, 0, 2}, {2, 0, 2}, pal.wall);          // the customer's side
    world.fillBox({-7, 0, 3}, {2, 0, 3}, palette::IronBlock);  // the working top

    // Till, at the right-hand end of the counter.
    world.set({1, 1, 3}, pal.tileDark);
    world.set({2, 1, 3}, palette::IronBlock);

    // ---- back wall: menu boards, lit from the lamps rather than emissive,
    // so the room still goes dark when they are switched off. Dark faces with
    // small accents -- a wall of yellow directly under a lamp blows out and
    // becomes the brightest thing in every shot that looks this way.
    world.fillBox({-6, 2, kBackZ - 1}, {-1, 3, kBackZ - 1}, pal.tileDark);
    world.fillBox({1, 2, kBackZ - 1}, {5, 3, kBackZ - 1}, pal.tileDark);
    world.fillBox({-6, 3, kBackZ - 1}, {-1, 3, kBackZ - 1}, pal.redPanel);
    world.fillBox({1, 3, kBackZ - 1}, {5, 3, kBackZ - 1}, pal.redPanel);
    world.set({-3, 2, kBackZ - 1}, pal.yellowPanel);
    world.set({3, 2, kBackZ - 1}, pal.yellowPanel);

    // Kitchen line behind the counter.
    world.fillBox({-7, 0, 5}, {-3, 1, 5}, palette::IronBlock);
    world.fillBox({0, 0, 5}, {3, 0, 5}, palette::IronBlock);

    // ---- seating
    //
    // No stools at the counter. A stool is smaller than a block, so a block
    // standing in for one is a metre cube of black in the foreground of every
    // interior shot -- and a counter you queue at rather than sit at is what
    // this kind of place has anyway.

    // Two booths down the left wall, and the corner one at the far end.
    auto booth = [&](int z) {
        world.fillBox({-8, 0, z - 1}, {-6, 0, z - 1}, pal.redPanel);   // bench
        world.fillBox({-8, 0, z + 1}, {-6, 0, z + 1}, pal.redPanel);   // bench
        world.fillBox({-8, 0, z}, {-7, 0, z}, pal.wall);               // table
    };
    booth(-4);
    booth(-1);

    // A free-standing table in the middle of the room, with a seat either
    // side. Red, like the booths: a dark seat reads as a hole in the floor.
    world.set({-2, 0, -3}, pal.wall);
    world.set({-3, 0, -3}, pal.redPanel);
    world.set({-1, 0, -3}, pal.redPanel);

    // ---- outside: the sign, and a lamp over the forecourt
    world.fillBox({7, 0, -9}, {7, 5, -9}, palette::IronBlock);
    world.fillBox({6, 6, -9}, {8, 7, -9}, pal.redPanel);
    world.fillBox({7, 6, -9}, {7, 7, -9}, pal.yellowPanel);
    world.set({7, 5, -9}, pal.dimGlow);

    world.fillBox({-8, 0, -9}, {-8, 4, -9}, palette::IronBlock);
    world.set({-8, 5, -9}, pal.warmGlow);

    // A bin and a stack of crates, so the forecourt is not an empty slab.
    world.fillBox({-3, 0, -8}, {-3, 1, -8}, pal.tileDark);
    world.fillBox({10, 0, -8}, {11, 1, -8}, palette::OakPlanks);
    world.set({10, 2, -8}, palette::OakPlanks);
}

} // namespace diner

// ================================================================= the road
//
// Where they walk. Rolling ground under a dark sky, a gravel track running
// along +X, trees scattered off it, a shallow ford about two thirds of the
// way, and the magenta light standing on the horizon at the far end.
//
// The track runs along X rather than Z so a tracking camera can travel
// sideways with the walkers and keep the trees crossing frame.
namespace road {

inline constexpr int kMinX = -60;
inline constexpr int kMaxX = 120;
inline constexpr int kMinZ = -48;
inline constexpr int kMaxZ = 48;
inline constexpr int kFordMinX = 54;
inline constexpr int kFordMaxX = 66;

// The ground height at a point. Exposed because the shots need to put feet
// on it, and a walker floating two centimetres up is visible immediately.
inline float groundHeight(float x, float z) {
    const float base = noise::fbm2(x * 0.020f, z * 0.020f, 4, 1207) * 5.4f;
    const float fine = noise::fbm2(x * 0.075f, z * 0.075f, 3, 55) * 1.1f;

    // The track is cut flat: a band around z = 0 is pulled toward the height
    // of the track's centre line, so the walkers are not climbing a hillside.
    const float centre = noise::fbm2(x * 0.020f, 0.0f, 4, 1207) * 5.4f;
    const float onTrack = 1.0f - saturate((std::fabs(z) - 3.0f) / 7.0f);

    float h = lerp(base + fine, centre, onTrack * 0.94f);

    // The ford: the ground dips into a stream bed across the track.
    const float mid = 0.5f * float(kFordMinX + kFordMaxX);
    const float half = 0.5f * float(kFordMaxX - kFordMinX);
    const float inFord = 1.0f - saturate((std::fabs(x - mid) - half * 0.35f) / (half * 0.65f));
    h -= 2.3f * inFord;

    return h;
}

inline int groundBlockY(float x, float z) { return int(std::floor(groundHeight(x, z))); }

inline void build(World& world, const Palette& pal) {
    // ---- terrain
    for (int x = kMinX; x <= kMaxX; ++x) {
        for (int z = kMinZ; z <= kMaxZ; ++z) {
            const int top = groundBlockY(float(x) + 0.5f, float(z) + 0.5f);
            world.fillBox({x, top - 5, z}, {x, top - 1, z}, palette::Stone);

            const bool onTrack = std::fabs(float(z)) < 3.2f;
            const bool inFord = x >= kFordMinX && x <= kFordMaxX;
            BlockId surface = palette::GrassBlock;
            if (inFord) surface = palette::Gravel;
            else if (onTrack) surface = palette::Gravel;
            world.set({x, top, z}, surface);
        }
    }

    // ---- the stream, filled to a level so it reads as water rather than as
    // a wet trench. Water is a medium here: shallow reads clear, deep blue.
    const int level = groundBlockY(float(kFordMinX + kFordMaxX) * 0.5f, 0.0f) + 1;
    for (int x = kFordMinX - 3; x <= kFordMaxX + 3; ++x) {
        for (int z = kMinZ; z <= kMaxZ; ++z) {
            for (int y = level; y > level - 4; --y) {
                if (world.get({x, y, z}) == palette::Air) world.set({x, y, z}, palette::Water);
            }
        }
    }
    // Stepping stones, so crossing is a choice and not a wade.
    for (int z = -3; z <= 3; ++z) {
        if ((z & 1) != 0) continue;
        for (int x = kFordMinX + 1; x <= kFordMaxX - 1; x += 3) {
            world.set({x, level, z}, palette::Cobblestone);
        }
    }

    // ---- trees, kept off the track
    const std::vector<Vec2> spots =
        poissonDisk({float(kMinX), float(kMinZ)}, {float(kMaxX), float(kMaxZ)}, 7.5f, 4242);
    uint32_t seed = 91;
    for (const Vec2& spot : spots) {
        if (std::fabs(spot.y) < 6.0f) continue;                     // clear of the track
        if (spot.x > kFordMinX - 5 && spot.x < kFordMaxX + 5) continue;  // clear of the water

        const int x = int(spot.x);
        const int z = int(spot.y);
        const shape::SurfacePoint surf = shape::findSurface(world, x, z, 40, -30);
        if (!surf.found || surf.id != palette::GrassBlock) continue;
        if (shape::surfaceRoughness(world, x, z, 40, 1) > 2) continue;

        const float pick = noise::hashToFloat(x, 0, z, 7);
        TreeParams params = pick < 0.42f ? tree::spruce(palette::SpruceLog, palette::SpruceLeaves) : (pick < 0.78f ? tree::oak(palette::OakLog, palette::OakLeaves) : tree::birch(palette::BirchLog, palette::BirchLeaves));
        params.minHeight += 1;
        params.maxHeight += 2;
        growTree(world, surf.block, params, seed);
        seed = seed * 1664525u + 1013904223u;
    }

    // ---- the diner, small and lit, back at the far -X end. Not the same
    // world as diner::build -- this is the shape of it seen from a mile off.
    {
        const int bx = kMinX + 8;
        const int by = groundBlockY(float(bx), 0.0f);
        world.fillBox({bx - 5, by + 1, -12}, {bx + 5, by + 4, -6}, pal.wall);
        world.fillBox({bx - 5, by + 4, -12}, {bx + 5, by + 4, -6}, pal.redPanel);
        world.fillBox({bx - 4, by + 2, -12}, {bx + 4, by + 3, -12}, pal.dimGlow);
        world.fillBox({bx + 6, by + 1, -12}, {bx + 6, by + 7, -12}, palette::IronBlock);
        world.set({bx + 6, by + 8, -12}, pal.dimGlow);
    }

    // ---- the light on the horizon: what they are walking toward, seen from
    // behind a low ridge so it is a glow rather than a lamp.
    {
        const int lx = kMaxX - 6;
        const int ly = groundBlockY(float(lx), 0.0f);
        shape::ellipsoid(world, {float(lx), float(ly) + 2.0f, 0.0f}, {5.0f, 4.0f, 5.0f},
                         pal.magentaGlow);
        world.fillBox({lx - 9, ly + 1, -14}, {lx + 9, ly + 6, -12}, palette::Stone);
        world.fillBox({lx - 9, ly + 1, 12}, {lx + 9, ly + 6, 14}, palette::Stone);
    }
}

} // namespace road

// ============================================================== the shrine
//
// A shallow basin with a ring of standing stones, and the light in the
// middle of it. Dawn arrives here, which is why the ring stands on open
// ground with nothing tall to the east.
namespace shrine {

inline constexpr int kRadius = 34;

inline float groundHeight(float x, float z) {
    const float r = std::sqrt(x * x + z * z);
    const float bowl = -3.2f * saturate(1.0f - r / 22.0f);
    const float rough = noise::fbm2(x * 0.035f, z * 0.035f, 4, 8801) * 3.0f;
    return bowl + rough * saturate((r - 12.0f) / 16.0f);
}

inline int groundBlockY(float x, float z) { return int(std::floor(groundHeight(x, z))); }

inline void build(World& world, const Palette& pal) {
    for (int x = -kRadius - 20; x <= kRadius + 20; ++x) {
        for (int z = -kRadius - 20; z <= kRadius + 20; ++z) {
            const int top = groundBlockY(float(x) + 0.5f, float(z) + 0.5f);
            world.fillBox({x, top - 5, z}, {x, top - 1, z}, palette::Stone);
            const float r = std::sqrt(float(x * x + z * z));
            world.set({x, top, z}, r < 20.0f ? palette::Gravel : palette::GrassBlock);
        }
    }

    // Water pooled in the bottom of the bowl.
    for (int x = -22; x <= 22; ++x) {
        for (int z = -22; z <= 22; ++z) {
            for (int y = -1; y > -4; --y) {
                if (world.get({x, y, z}) == palette::Air) world.set({x, y, z}, palette::Water);
            }
        }
    }

    // A dry island in the centre for the cast to stand on.
    for (int x = -6; x <= 6; ++x) {
        for (int z = -6; z <= 6; ++z) {
            if (x * x + z * z > 40) continue;
            world.fillBox({x, -4, z}, {x, -1, z}, palette::Stone);
            world.set({x, 0, z}, palette::Cobblestone);
        }
    }
    // A causeway out to it, on the -X side, which is where they arrive from.
    for (int x = -24; x <= -5; ++x) {
        for (int z = -2; z <= 2; ++z) {
            world.fillBox({x, -4, z}, {x, -1, z}, palette::Stone);
            world.set({x, 0, z}, palette::Cobblestone);
        }
    }

    // The ring: twelve standing stones, every third one lit.
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) / 12.0f * kTwoPi;
        const int x = int(std::lround(std::cos(a) * 15.0f));
        const int z = int(std::lround(std::sin(a) * 15.0f));
        const int base = groundBlockY(float(x), float(z));
        const int height = 4 + (i % 3);
        world.fillBox({x, base, z}, {x, base + height, z}, palette::Obsidian);
        if (i % 3 == 0) world.set({x, base + height + 1, z}, pal.magentaGlow);
    }

    // The middle: a low plinth, unlit. What lights it arrives later, and a
    // plinth that glows before she does gives the shot away.
    //
    // One step high, and pale. The first version was three courses of
    // obsidian, which is taller than the character standing on it and darker
    // than everything around it -- she read as a smudge on top of a hole.
    world.fillBox({-3, 1, -3}, {3, 1, 3}, palette::Stone);

    // Ridge to the east, low enough to let a sunrise over it.
    for (int x = 30; x <= kRadius + 18; ++x) {
        for (int z = -kRadius - 18; z <= kRadius + 18; ++z) {
            const int top = groundBlockY(float(x), float(z));
            const float rise = float(x - 30) * 0.35f;
            world.fillBox({x, top + 1, z}, {x, top + int(rise), z}, palette::Stone);
        }
    }
}

} // namespace shrine
} // namespace film
