#pragma once
// Trees, and the scattering that decides where they go.
//
// A tree here is a handful of parameters rather than a hard-coded blob: a
// trunk, some branches, and a crown built either as overlapping ellipsoids or
// as stacked cones. The presets are starting points, not the API -- change
// any field and you get a different species.
#include "engine/core/math.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <vector>

namespace blocky {

struct TreeParams {
    // Which blocks this species is made of. There is no sensible default: the
    // engine ships no palette, so "oak" is a shape here and a pair of ids the
    // caller owns. Left as air, a tree grows into nothing at all.
    BlockId log = block::Air;
    BlockId leaves = block::Air;

    // Trunk. Height is picked uniformly in the range, per tree.
    int   minHeight = 5;
    int   maxHeight = 8;
    float trunkRadius = 0.6f;
    float lean = 0.0f;          // sideways drift over the trunk's height

    // Branches, grown from the upper part of the trunk.
    int   branches = 3;
    float branchStart = 0.55f;  // fraction of trunk height where they begin
    float branchLength = 3.2f;
    float branchTiltDegrees = 42.0f;
    float branchRadius = 0.45f;

    // Crown.
    float crownRadius = 2.8f;
    float crownHeight = 3.4f;
    float crownDroop = 0.0f;    // pulls the lower crown outward and down
    float raggedness = 0.35f;   // 0 = smooth shell, 1 = very broken up

    // Conifers build a stack of cones instead of blobs.
    bool  conifer = false;
    int   coniferTiers = 5;
};

// Shape presets. Each is a set of proportions -- how tall, how many branches,
// how ragged the crown -- and takes the two blocks it should be built from,
// because those belong to whoever owns the palette. `tree::spruce` is a
// conifer whatever its wood is called.
namespace tree {
TreeParams oak(BlockId log, BlockId leaves);
TreeParams birch(BlockId log, BlockId leaves);
TreeParams spruce(BlockId log, BlockId leaves);
TreeParams bush(BlockId log, BlockId leaves);
} // namespace tree

// Grows a tree with its trunk starting on top of `base` -- pass the ground
// block, and the trunk begins one above it.
void growTree(World& world, IVec3 base, const TreeParams& params, uint32_t seed);

// ------------------------------------------------------------- scattering
// Bridson's Poisson-disk sampling: points at least `minSpacing` apart, with
// none of the clumping or grid alignment that random or jittered-grid
// placement produces. This is what makes a scattered forest look natural.
std::vector<Vec2> poissonDisk(Vec2 minCorner, Vec2 maxCorner, float minSpacing, uint32_t seed,
                              int attemptsPerPoint = 24);

} // namespace blocky
