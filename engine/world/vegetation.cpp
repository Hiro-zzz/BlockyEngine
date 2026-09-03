#include "engine/world/vegetation.hpp"

#include "engine/core/random.hpp"
#include "engine/world/noise.hpp"
#include "engine/world/shapes.hpp"

#include <algorithm>

namespace blocky {
namespace {

// Leaves are written only where there is nothing already, so a crown never
// eats the trunk or a neighbouring tree's wood.
void placeLeaf(World& world, IVec3 p, BlockId leaves) {
    if (world.get(p) == block::Air) world.set(p, leaves);
}

// A crown blob: an ellipsoid whose surface is broken up by noise, so the
// silhouette reads as foliage rather than as a ball.
void leafBlob(World& world, Vec3 centre, Vec3 radii, BlockId leaves, float raggedness,
              uint32_t seed) {
    Vec3 safe = maxv(radii, Vec3{0.5f});
    IVec3 lo = floorToInt(centre - safe - Vec3{1.0f});
    IVec3 hi = floorToInt(centre + safe + Vec3{1.0f});

    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                Vec3 p{float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f};
                Vec3 d = (p - centre) / safe;
                float distance = length(d);
                if (distance > 1.0f) continue;

                // Thin the shell out towards the edge, and let noise decide
                // which of those outer blocks survive.
                if (distance > 1.0f - raggedness) {
                    float t = (distance - (1.0f - raggedness)) / std::max(raggedness, 1e-3f);
                    float keep = noise::hashToFloat(x, y, z, seed);
                    if (keep < t) continue;
                }
                placeLeaf(world, {x, y, z}, leaves);
            }
        }
    }
}

} // namespace

namespace tree {

TreeParams oak(BlockId log, BlockId leaves) {
    TreeParams p;
    p.log = log;
    p.leaves = leaves;
    p.minHeight = 5;
    p.maxHeight = 8;
    p.trunkRadius = 0.62f;
    p.branches = 3;
    p.branchLength = 3.0f;
    p.crownRadius = 2.9f;
    p.crownHeight = 3.2f;
    p.raggedness = 0.42f;
    return p;
}

TreeParams birch(BlockId log, BlockId leaves) {
    TreeParams p;
    p.log = log;
    p.leaves = leaves;
    p.minHeight = 7;
    p.maxHeight = 10;
    p.trunkRadius = 0.52f;
    p.lean = 0.6f;
    p.branches = 2;
    p.branchStart = 0.68f;
    p.branchLength = 2.2f;
    p.branchTiltDegrees = 30.0f;
    p.crownRadius = 2.3f;
    p.crownHeight = 3.6f;
    p.raggedness = 0.34f;
    return p;
}

TreeParams spruce(BlockId log, BlockId leaves) {
    TreeParams p;
    p.log = log;
    p.leaves = leaves;
    p.minHeight = 9;
    p.maxHeight = 14;
    p.trunkRadius = 0.58f;
    p.branches = 0;
    p.crownRadius = 3.2f;
    p.crownHeight = 8.0f;
    p.raggedness = 0.3f;
    p.conifer = true;
    p.coniferTiers = 5;
    return p;
}

TreeParams bush(BlockId log, BlockId leaves) {
    TreeParams p;
    p.log = log;
    p.leaves = leaves;
    p.minHeight = 1;
    p.maxHeight = 2;
    p.trunkRadius = 0.5f;
    p.branches = 0;
    p.crownRadius = 1.8f;
    p.crownHeight = 1.6f;
    p.raggedness = 0.5f;
    return p;
}

} // namespace tree

void growTree(World& world, IVec3 base, const TreeParams& params, uint32_t seed) {
    Rng rng(seed, uint64_t(base.x) * 73856093u ^ uint64_t(base.z) * 19349663u);

    int height = params.minHeight;
    if (params.maxHeight > params.minHeight) {
        height += int(rng.nextFloat() * float(params.maxHeight - params.minHeight + 1));
        height = std::min(height, params.maxHeight);
    }

    // The trunk starts on top of the block we were handed.
    Vec3 foot{float(base.x) + 0.5f, float(base.y) + 1.0f, float(base.z) + 0.5f};

    float leanAngle = rng.nextFloat() * kTwoPi;
    Vec3 leanOffset{std::cos(leanAngle) * params.lean, 0.0f, std::sin(leanAngle) * params.lean};
    Vec3 crownBase = foot + Vec3{0.0f, float(height), 0.0f} + leanOffset;

    shape::line(world, foot, crownBase, params.trunkRadius, params.log);

    if (params.conifer) {
        // Stacked cones, widest at the bottom, tapering to a spire.
        int tiers = std::max(1, params.coniferTiers);
        float tierHeight = params.crownHeight / float(tiers);
        float start = float(height) - params.crownHeight * 0.75f;

        for (int tier = 0; tier < tiers; ++tier) {
            float t = float(tier) / float(tiers);
            float radius = params.crownRadius * (1.0f - t) + 0.6f;
            Vec3 centre = foot + Vec3{0.0f, start + float(tier) * tierHeight, 0.0f} +
                          leanOffset * (start / std::max(1.0f, float(height)));

            leafBlob(world, centre, Vec3{radius, tierHeight * 0.9f, radius}, params.leaves,
                     params.raggedness, seed + uint32_t(tier) * 131u);
        }
        // A tip, so the spire does not end bluntly.
        leafBlob(world, crownBase + Vec3{0.0f, 0.6f, 0.0f}, Vec3{0.9f, 1.4f, 0.9f}, params.leaves,
                 params.raggedness * 0.5f, seed + 977u);
        return;
    }

    // ---- branches, and a crown blob at the tip of each
    for (int i = 0; i < params.branches; ++i) {
        float fraction = params.branchStart +
                         (1.0f - params.branchStart) * (float(i) + 0.5f) / float(params.branches);
        Vec3 from = lerp(foot, crownBase, fraction);

        // Spread the branches around the trunk, with a little jitter so they
        // do not sit at perfectly regular angles.
        float angle = (float(i) / float(params.branches)) * kTwoPi + rng.nextFloat() * 0.9f;
        float tilt = radians(params.branchTiltDegrees) * (0.75f + rng.nextFloat() * 0.5f);

        Vec3 direction{std::cos(angle) * std::sin(tilt), std::cos(tilt), std::sin(angle) * std::sin(tilt)};
        float reach = params.branchLength * (0.75f + rng.nextFloat() * 0.5f);
        Vec3 to = from + direction * reach;

        shape::line(world, from, to, params.branchRadius, params.log);

        leafBlob(world, to, Vec3{params.crownRadius * 0.62f, params.crownHeight * 0.45f,
                                 params.crownRadius * 0.62f},
                 params.leaves, params.raggedness, seed + uint32_t(i) * 7919u);
    }

    // ---- the main crown, sitting on the top of the trunk
    Vec3 crownCentre = crownBase + Vec3{0.0f, params.crownHeight * 0.25f, 0.0f};
    leafBlob(world, crownCentre,
             Vec3{params.crownRadius, params.crownHeight * 0.5f, params.crownRadius},
             params.leaves, params.raggedness, seed + 4801u);

    if (params.crownDroop > 0.0f) {
        leafBlob(world, crownCentre - Vec3{0.0f, params.crownHeight * 0.45f, 0.0f},
                 Vec3{params.crownRadius * (1.0f + params.crownDroop),
                      params.crownHeight * 0.28f,
                      params.crownRadius * (1.0f + params.crownDroop)},
                 params.leaves, params.raggedness, seed + 6151u);
    }
}

std::vector<Vec2> poissonDisk(Vec2 minCorner, Vec2 maxCorner, float minSpacing, uint32_t seed,
                              int attemptsPerPoint) {
    std::vector<Vec2> result;
    float width = maxCorner.x - minCorner.x;
    float depth = maxCorner.y - minCorner.y;
    if (width <= 0.0f || depth <= 0.0f || minSpacing <= 0.0f) return result;

    // One point per cell at most, which is what makes the neighbour search a
    // fixed-size window rather than a scan of everything placed so far.
    const float cellSize = minSpacing / 1.41421356f;
    const int cols = std::max(1, int(std::ceil(width / cellSize)));
    const int rows = std::max(1, int(std::ceil(depth / cellSize)));

    std::vector<int> grid(size_t(cols) * size_t(rows), -1);
    std::vector<Vec2> active;

    Rng rng(seed, 0x9E3779B9ull);

    auto cellOf = [&](Vec2 p) {
        int cx = std::min(cols - 1, std::max(0, int((p.x - minCorner.x) / cellSize)));
        int cy = std::min(rows - 1, std::max(0, int((p.y - minCorner.y) / cellSize)));
        return std::pair<int, int>{cx, cy};
    };

    auto farEnough = [&](Vec2 p) {
        auto [cx, cy] = cellOf(p);
        for (int y = std::max(0, cy - 2); y <= std::min(rows - 1, cy + 2); ++y) {
            for (int x = std::max(0, cx - 2); x <= std::min(cols - 1, cx + 2); ++x) {
                int index = grid[size_t(y) * size_t(cols) + size_t(x)];
                if (index < 0) continue;
                Vec2 other = result[size_t(index)];
                float dx = other.x - p.x, dy = other.y - p.y;
                if (dx * dx + dy * dy < minSpacing * minSpacing) return false;
            }
        }
        return true;
    };

    auto accept = [&](Vec2 p) {
        auto [cx, cy] = cellOf(p);
        grid[size_t(cy) * size_t(cols) + size_t(cx)] = int(result.size());
        result.push_back(p);
        active.push_back(p);
    };

    accept({minCorner.x + rng.nextFloat() * width, minCorner.y + rng.nextFloat() * depth});

    while (!active.empty()) {
        size_t pick = size_t(rng.nextUint() % uint32_t(active.size()));
        Vec2 origin = active[pick];
        bool placed = false;

        for (int attempt = 0; attempt < attemptsPerPoint; ++attempt) {
            // Uniform in the annulus between one and two spacings.
            float angle = rng.nextFloat() * kTwoPi;
            float radius = minSpacing * (1.0f + rng.nextFloat());
            Vec2 candidate{origin.x + std::cos(angle) * radius, origin.y + std::sin(angle) * radius};

            if (candidate.x < minCorner.x || candidate.x >= maxCorner.x) continue;
            if (candidate.y < minCorner.y || candidate.y >= maxCorner.y) continue;
            if (!farEnough(candidate)) continue;

            accept(candidate);
            placed = true;
            break;
        }

        if (!placed) {
            active[pick] = active.back();
            active.pop_back();
        }
    }
    return result;
}

} // namespace blocky
