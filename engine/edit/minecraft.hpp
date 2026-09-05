#pragma once
// Getting a model out of this engine and into Minecraft.
//
// The C++ emitter next door writes what this project keeps. This writes what
// somebody else's game can read, and the two are not competing: a mod or a
// resource pack has never heard of `voxelize::fromLayers`, and this engine has
// no use for a JSON model. Each format belongs to whoever is going to open it.
//
// -------------------------------------------------------------- what it emits
//
// A vanilla block/item model: a `textures` map, an `elements` array of
// axis-aligned cuboids, and a `display` block. That is the same file format a
// mod ships and a resource pack overrides, so one exporter serves both.
//
// The cuboids come from `mergeVoxelBoxes(model, ByMaterial)` -- the same exact
// greedy merge the physics collider uses, with the equivalence that keeps
// colour. A sixteen-voxel item usually falls to a few dozen elements.
//
// ------------------------------------------------------- colour lives in a texture
//
// An element in Minecraft has no colour of its own; every face reads a
// texture. So the palette is baked into one, and each face's UV points at the
// texel holding its material.
//
// The texture is 64x64 holding a 16x16 grid of 4x4 blocks rather than a plain
// 16x16 of single texels, and the four is not decoration: identical texels
// either side of a boundary keep the first two mip levels pure, so a model
// seen across a room does not bleed its neighbour's colour into its faces.
// Vanilla UVs are in 0..16 whatever the texture's resolution, so the numbers
// in the file are the same either way -- the padding is free.
//
// ------------------------------------------------------------------ the limits
//
// Model coordinates in Minecraft must lie within -16..32, and this places the
// model from the origin, so a grid above 32 a side is refused rather than
// silently scaled: a model that came back at half size would look like a bug
// in the game, not in the export.
#include "engine/core/image.hpp"
#include "engine/prop/voxel_model.hpp"

#include <string>

namespace blocky {
namespace edit {

struct McExportOptions {
    // Resource namespace and file name. Both must be Minecraft resource
    // names: lowercase letters, digits, underscore, dash and dot. Use
    // `resourceName` to make one out of a title.
    std::string space = "forge";
    std::string name = "model";

    // `item` or `block`. Decides both directories and nothing else -- the
    // elements are identical, which is why one exporter covers both.
    std::string folder = "item";

    // The vanilla `block/block` display transforms, which is what a
    // block-shaped item wants in a hand and in the inventory. Off for a block
    // model, which takes its display from the blockstate instead.
    bool display = true;

    // Goes into pack.mcmeta. It is a number per game version and there is no
    // way to guess which one you are targeting, so it is a knob with a recent
    // default rather than a promise.
    int packFormat = 15;

    std::string description = "Made with BlockyEngine forge";
};

struct McStats {
    int elements = 0;
    int faces = 0;      // after the hidden ones are dropped
    int materials = 0;
};

// "Iron Mug" becomes "iron_mug". Anything that is not a legal resource
// character becomes an underscore, and runs of them collapse.
std::string resourceName(const std::string& title);

// The model file's text.
bool buildMinecraftModel(const VoxelModel& model, std::string& json,
                         const McExportOptions& options = {}, McStats* stats = nullptr,
                         std::string* error = nullptr);

// The palette texture the model's UVs point into.
ImageU8 buildMinecraftPalette(const VoxelModel& model);

// Both, plus pack.mcmeta, written as a resource pack under `root`:
//
//   <root>/pack.mcmeta
//   <root>/assets/<space>/models/<folder>/<name>.json
//   <root>/assets/<space>/textures/<folder>/<name>.png
bool writeMinecraftPack(const VoxelModel& model, const std::string& root,
                        const McExportOptions& options = {}, McStats* stats = nullptr,
                        std::string* error = nullptr);

} // namespace edit
} // namespace blocky
