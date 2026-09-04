#include "game/terrain.hpp"
#include "game/blocks.hpp"

#include "engine/world/noise.hpp"
#include "engine/world/vegetation.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

namespace {

int heightAt(const TerrainSettings& settings, int x, int z) {
    float n = noise::fbm2(float(x) * settings.frequency, float(z) * settings.frequency,
                          settings.octaves, settings.seed);

    // Ridged noise on top, at a higher frequency and a low weight: pure fbm
    // gives rolling hills and nothing to climb, and the ridges are what make
    // somewhere worth walking to.
    float ridge = noise::ridged2(float(x) * settings.frequency * 2.3f,
                                 float(z) * settings.frequency * 2.3f, 3, settings.seed + 91u);

    float h = float(settings.baseHeight) + n * settings.amplitude + ridge * settings.amplitude * 0.35f;
    return int(h);
}

}  // namespace

uint64_t generateTerrain(World& world, const TerrainSettings& settings) {
    const int extent = settings.extent;

    for (int z = -extent; z <= extent; ++z) {
        for (int x = -extent; x <= extent; ++x) {
            int h = std::max(1, heightAt(settings, x, z));

            // Stone under, a few blocks of dirt, and a surface that depends on
            // where the water is. Sand only just above the waterline, so a
            // beach is a beach and not a desert climbing a hill.
            for (int y = 0; y < h - 4; ++y) world.set({x, y, z}, block::Stone);
            for (int y = std::max(0, h - 4); y < h; ++y) world.set({x, y, z}, block::Dirt);

            BlockId surface = block::GrassBlock;
            if (h <= settings.seaLevel + 1) surface = block::Sand;
            if (h <= settings.seaLevel - 3) surface = block::Gravel;
            world.set({x, h, z}, surface);

            for (int y = h + 1; y <= settings.seaLevel; ++y) world.set({x, y, z}, block::Water);
        }
    }

    // Trees, spaced by a Poisson disk so they neither line up nor clump.
    std::vector<Vec2> spots = poissonDisk({float(-extent) + 2.0f, float(-extent) + 2.0f},
                                          {float(extent) - 2.0f, float(extent) - 2.0f},
                                          settings.treeSpacing, settings.seed + 7u);

    uint32_t which = 0;
    for (Vec2 spot : spots) {
        int x = int(spot.x);
        int z = int(spot.y);
        int h = std::max(1, heightAt(settings, x, z));

        // Only on grass, and not on a slope so steep the trunk would hang.
        if (world.get({x, h, z}) != block::GrassBlock) continue;
        if (std::abs(heightAt(settings, x + 2, z) - h) > 2) continue;
        if (std::abs(heightAt(settings, x, z + 2) - h) > 2) continue;

        TreeParams params = (which % 3 == 0)   ? tree::birch(block::BirchLog, block::BirchLeaves)
                            : (which % 3 == 1) ? tree::oak(block::OakLog, block::OakLeaves)
                                               : tree::spruce(block::SpruceLog, block::SpruceLeaves);
        growTree(world, {x, h + 1, z}, params, settings.seed + which * 13u);
        ++which;
    }

    return world.blockCount();
}

Vec3 findSpawn(const World& world, const TerrainSettings& settings) {
    // Spiral out from the middle until there is dry ground. A fixed spawn
    // point lands in the sea about a third of the time, and a player who
    // starts underwater assumes the game is broken rather than that the
    // terrain seed was unkind.
    for (int radius = 0; radius < settings.extent; ++radius) {
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;

                for (int y = settings.baseHeight + 40; y > settings.seaLevel; --y) {
                    BlockId here = world.get({dx, y, dz});
                    if (here == block::Air) continue;

                    // The first solid thing from above is not necessarily the
                    // ground: land on a canopy and the first thing the player
                    // sees is the inside of a tree. Only ground counts, and
                    // the two blocks above it have to be clear.
                    if (here != block::GrassBlock && here != block::Sand &&
                        here != block::Gravel && here != block::Dirt)
                        break;
                    if (world.get({dx, y + 1, dz}) != block::Air) break;
                    if (world.get({dx, y + 2, dz}) != block::Air) break;

                    return Vec3{float(dx) + 0.5f, float(y + 1), float(dz) + 0.5f};
                }
            }
        }
    }
    return Vec3{0.5f, float(settings.baseHeight + 40), 0.5f};
}

} // namespace game
