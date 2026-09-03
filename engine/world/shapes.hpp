#pragma once
// Modelling primitives.
//
// The one that matters is fillWhere: hand it a predicate over a point and it
// stamps every block the predicate accepts. Everything else in this header is
// that function with a shape already written for you, and anything not here
// you can write in three lines without extending the engine.
#include "engine/core/math.hpp"
#include "engine/world/world.hpp"

#include <vector>

namespace blocky {
namespace shape {

// ------------------------------------------------------------------- core
// Visits every block in the inclusive box and writes `id` wherever
// `accept(centre)` returns true. `centre` is the block's centre in world
// units, which is what shape maths wants -- a block at p spans [p, p+1).
template <class Fn>
void fillWhere(World& world, IVec3 minCorner, IVec3 maxCorner, BlockId id, Fn&& accept) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);

    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                Vec3 centre{float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f};
                if (accept(centre)) world.set({x, y, z}, id);
            }
        }
    }
}

// The same, but the predicate also sees the block coordinate -- for anything
// that depends on the lattice rather than on continuous position.
template <class Fn>
void fillWhereBlock(World& world, IVec3 minCorner, IVec3 maxCorner, BlockId id, Fn&& accept) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);

    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                IVec3 p{x, y, z};
                if (accept(p)) world.set(p, id);
            }
        }
    }
}

// ----------------------------------------------------------------- solids
// Axis-aligned ellipsoid. Equal radii give a sphere.
void ellipsoid(World& world, Vec3 centre, Vec3 radii, BlockId id, bool hollow = false);

// Cylinder along an arbitrary axis. `axis` need not be normalised; its length
// is ignored and `height` is used instead.
void cylinder(World& world, Vec3 base, Vec3 axis, float radius, float height, BlockId id);

// Cone, or a truncated cone when `topRadius` is above zero.
void cone(World& world, Vec3 base, Vec3 axis, float baseRadius, float topRadius, float height,
          BlockId id);

// Thick line between two points -- beams, branches, paths.
void line(World& world, Vec3 from, Vec3 to, float radius, BlockId id);

// Torus in the plane whose normal is `axis`.
void torus(World& world, Vec3 centre, Vec3 axis, float majorRadius, float minorRadius, BlockId id);

// Solid disc, one block thick, lying in the plane whose normal is `axis`.
void disc(World& world, Vec3 centre, Vec3 axis, float radius, BlockId id);

// ------------------------------------------------------------- edits
// Swap one block type for another inside a box. `from` may be Air.
void replace(World& world, IVec3 minCorner, IVec3 maxCorner, BlockId from, BlockId to);

// Erode: remove blocks with fewer than `minNeighbours` solid neighbours.
// Run a couple of passes to soften a hard-edged procedural shape.
void erode(World& world, IVec3 minCorner, IVec3 maxCorner, int minNeighbours, int passes = 1);

// ------------------------------------------------------------- clipboard
// A detached copy of a region, so it can be transformed and stamped back.
// This is what turns "place blocks" into modelling: build a thing once,
// mirror it, rotate it, repeat it.
struct Clipboard {
    IVec3 size{};
    std::vector<BlockId> blocks;

    BlockId at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= size.x || y >= size.y || z >= size.z) return block::Air;
        return blocks[size_t((y * size.z + z) * size.x + x)];
    }
    void set(int x, int y, int z, BlockId id) {
        blocks[size_t((y * size.z + z) * size.x + x)] = id;
    }
    bool empty() const { return blocks.empty(); }
};

Clipboard copy(const World& world, IVec3 minCorner, IVec3 maxCorner);

// `skipAir` leaves whatever is already there where the clipboard holds air,
// which is what you want when stamping a shape onto terrain.
void paste(World& world, const Clipboard& clip, IVec3 at, bool skipAir = true);

Clipboard mirrored(const Clipboard& clip, int axis);
Clipboard rotatedY(const Clipboard& clip, int quarterTurns);

// ------------------------------------------------------------- surface
// Where the ground is in a column, searching downwards from `topY`.
struct SurfacePoint {
    IVec3 block{};    // the topmost non-air block
    BlockId id = block::Air;
    bool found = false;
};
SurfacePoint findSurface(const World& world, int x, int z, int topY, int bottomY = 0);

// How uneven the ground is around a column, in blocks of height difference.
// Useful for refusing to plant a tree on a cliff edge.
int surfaceRoughness(const World& world, int x, int z, int topY, int radius = 1);

} // namespace shape
} // namespace blocky
