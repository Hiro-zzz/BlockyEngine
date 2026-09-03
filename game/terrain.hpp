#pragma once
// The world the game starts in.
//
// Kept apart from `main.cpp` because it is the piece most likely to be
// replaced: a heightmap is the simplest thing that produces somewhere worth
// standing, and the moment there are caves, biomes or structures it will want
// its own file anyway. The seam is one function -- everything else in the game
// asks the `World`, not the generator.
#include "engine/world/world.hpp"

namespace game {

struct TerrainSettings {
    // Half-width in blocks: the world runs from -extent to +extent.
    int extent = 96;

    uint32_t seed = 1337u;

    int seaLevel = 26;
    int baseHeight = 28;
    float amplitude = 14.0f;
    float frequency = 0.014f;
    int octaves = 5;

    // Roughly how far apart trees stand, in blocks.
    float treeSpacing = 9.0f;
};

// Fills `world` with ground, water, sand along the shore and trees on the
// grass. Returns the number of blocks placed.
uint64_t generateTerrain(blocky::World& world, const TerrainSettings& settings = {});

// A spot to start on: the highest ground near the middle that is not under
// water, as a feet position.
blocky::Vec3 findSpawn(const blocky::World& world, const TerrainSettings& settings);

} // namespace game
