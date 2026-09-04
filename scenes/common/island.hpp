#pragma once
// The island used by both the direct-lighting and the path-traced scenes.
// Kept here rather than in the engine because it is scene content, not
// engine functionality.
#include "engine/world/noise.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

namespace island {

using namespace blocky;

constexpr int      kRadius   = 46;  // island half-extent in blocks
constexpr int      kSeaLevel = 11;
constexpr uint32_t kSeed     = 20250825u;

// Terrain height at a column, in blocks.
inline int terrainHeight(int x, int z) {
    float fx = float(x), fz = float(z);

    float base = noise::fbm2(fx * 0.028f, fz * 0.028f, 5, kSeed);
    float ridge = noise::ridged2(fx * 0.045f, fz * 0.045f, 4, kSeed + 77u);

    // Radial falloff turns an infinite landscape into an island.
    float distance = std::sqrt(fx * fx + fz * fz) / float(kRadius);
    float falloff = saturate(1.0f - distance * distance);
    falloff = falloff * falloff;

    float height = (base * 0.5f + 0.5f) * 14.0f + ridge * 13.0f * falloff;
    height = height * falloff + 2.0f * falloff;

    return int(height) + 3;
}

inline void generateTerrain(World& world) {
    for (int z = -kRadius; z <= kRadius; ++z) {
        for (int x = -kRadius; x <= kRadius; ++x) {
            int height = terrainHeight(x, z);
            if (height < 1) continue;

            for (int y = 0; y <= height; ++y) {
                BlockId id;
                if (y > height - 1) {
                    id = (height <= kSeaLevel + 1) ? palette::Sand : palette::GrassBlock;
                } else if (y > height - 4) {
                    id = (height <= kSeaLevel + 1) ? palette::Sand : palette::Dirt;
                } else {
                    id = palette::Stone;
                }
                world.set({x, y, z}, id);
            }

            for (int y = height + 1; y <= kSeaLevel; ++y) {
                world.set({x, y, z}, palette::Water);
            }
        }
    }
}

inline void plantTree(World& world, int x, int z, int groundY, uint32_t salt) {
    int trunk = 4 + int(noise::hashToFloat(x, groundY, z, kSeed + salt) * 3.0f);
    int topY = groundY + trunk;

    for (int y = groundY + 1; y <= topY; ++y) world.set({x, y, z}, palette::OakLog);

    for (int dy = -2; dy <= 2; ++dy) {
        int radius = (dy <= 0) ? 2 : (dy == 1 ? 2 : 1);
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dz == 0 && dy <= 0) continue;  // keep the trunk
                int distance = dx * dx + dz * dz;
                if (distance > radius * radius) continue;
                if (distance == radius * radius &&
                    noise::hashToFloat(x + dx, topY + dy, z + dz, kSeed + 5u) < 0.45f) {
                    continue;
                }
                world.set({x + dx, topY + dy, z + dz}, palette::OakLeaves);
            }
        }
    }
}

inline void scatterTrees(World& world) {
    for (int z = -kRadius; z <= kRadius; z += 3) {
        for (int x = -kRadius; x <= kRadius; x += 3) {
            int height = terrainHeight(x, z);
            if (height <= kSeaLevel + 2 || height > 22) continue;
            if (world.get({x, height, z}) != palette::GrassBlock) continue;
            if (noise::hashToFloat(x, 0, z, kSeed + 991u) > 0.16f) continue;
            plantTree(world, x, z, height, 13u);
        }
    }
}

// A small lookout on the summit: something built by hand next to the
// generated terrain, and something emissive to light the scene from within.
inline void buildLookout(World& world) {
    int bestX = 0, bestZ = 0, bestHeight = -1;
    for (int z = -20; z <= 20; ++z) {
        for (int x = -20; x <= 20; ++x) {
            int height = terrainHeight(x, z);
            if (height > bestHeight) { bestHeight = height; bestX = x; bestZ = z; }
        }
    }

    int y = bestHeight + 1;
    IVec3 origin{bestX - 3, y, bestZ - 3};

    world.fillBox(origin, origin + IVec3{6, 0, 6}, palette::OakPlanks);
    world.fillHollowBox(origin + IVec3{0, 1, 0}, origin + IVec3{6, 4, 6}, palette::Cobblestone);
    world.fillBox(origin + IVec3{1, 1, 1}, origin + IVec3{5, 4, 5}, palette::Air);
    world.fillBox(origin + IVec3{0, 5, 0}, origin + IVec3{6, 5, 6}, palette::OakPlanks);

    // Doorway and two windows.
    world.fillBox(origin + IVec3{3, 1, 0}, origin + IVec3{3, 2, 0}, palette::Air);
    world.set(origin + IVec3{0, 3, 3}, palette::Glass);
    world.set(origin + IVec3{6, 3, 3}, palette::Glass);

    // A lantern inside, so light spills out of the door and windows, plus a
    // brazier of lava at the threshold.
    world.set(origin + IVec3{3, 3, 3}, palette::Glowstone);
    world.set(origin + IVec3{3, 6, 3}, palette::Glowstone);
    world.fillBox(origin + IVec3{2, 0, -2}, origin + IVec3{4, 0, -2}, palette::Cobblestone);
    world.set(origin + IVec3{3, 0, -2}, palette::Lava);
}

inline void build(World& world) {
    generateTerrain(world);
    scatterTrees(world);
    buildLookout(world);
}

} // namespace island
