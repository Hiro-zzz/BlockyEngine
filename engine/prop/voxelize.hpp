#pragma once
// Two ways to author a voxel model by hand.
//
// `fromWorld` is the important one. Everything in `shape::` already builds
// voxels -- ellipsoids, cylinders, lines, erosion, the clipboard -- but it
// builds them into a World, locked to the lattice. Capturing a region turns
// that work into a prop that can be rotated freely, scaled to any size, and
// stamped a hundred times. Model a barrel once with the tools that exist,
// then place it as furniture.
//
// `fromLayers` is for the small things a primitive cannot describe: a mug, a
// book, a rune. Drawn as text, like the built-in font, so a wrong voxel is a
// wrong character you can see in the source.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/world/world.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace voxelize {

struct CaptureOptions {
    // Blocks that let light through are skipped. Water and glass are media in
    // the world, tracked by the block the ray is inside; a prop has no such
    // notion and would render them as opaque paint.
    bool skipTransmissive = true;

    // With a library, a voxel takes the mean of the block's six face colours
    // instead of its flat palette albedo. One colour per voxel either way --
    // see the note below.
    const BlockTextureLibrary* textures = nullptr;

    bool trim = true;
};

// Capture an inclusive region of the world.
//
// > A voxel carries **one** material, not six. A captured grass block is a
// > single colour rather than green on top and brown at the sides. That is the
// > same trade MagicaVoxel makes and it is the right one for props: per-face
// > materials would sextuple the palette lookups to serve a case -- a captured
// > block that must keep its face variation -- that the world itself already
// > handles better.
VoxelModel fromWorld(const World& world, IVec3 min, IVec3 max,
                     const CaptureOptions& options = {});

struct VoxelKey {
    char code = '#';
    VoxelMaterial material;
};

// Hand-drawn. `layers[y][z][x]`: one slice per Y from the bottom up, each
// slice a list of rows running along +Z, each row a string of columns running
// along +X. '.' and ' ' are empty; every other character is looked up in the
// key and ignored if it is not there.
//
//   VoxelModel mug = voxelize::fromLayers({
//       {"###", "#.#", "###"},     // y = 0, the base
//       {"###", "#.#", "###"},     // y = 1
//   }, {{'#', clay}});
VoxelModel fromLayers(const std::vector<std::vector<std::string>>& layers,
                      const std::vector<VoxelKey>& key);

} // namespace voxelize
} // namespace blocky
