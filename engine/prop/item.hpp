#pragma once
// Minecraft items, turned into voxel models.
//
// An item in the game is a flat 16x16 sprite. What you see in a hand or on the
// ground is that sprite *extruded*: every opaque texel becomes a little box a
// couple of texels deep, and the silhouette gets sides. For a voxel renderer
// that is not an approximation of the game's model -- it is literally the same
// construction, and the result is real geometry that shades and shadows like
// everything else.
//
// All 587 item textures in a 1.20 jar are 16x16 indexed PNGs, which our decoder
// already reads. Nothing here needs the game's model JSON.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/texture.hpp"
#include "engine/prop/voxel_model.hpp"

#include <string>

namespace blocky {
namespace item {

struct ItemOptions {
    // The game's generated item model is two texels thick, front face at
    // 7/16 and back at 9/16. Matching it keeps a sword looking like a sword
    // rather than a plank.
    int depthTexels = 2;

    // Texels below this are not part of the item.
    float alphaCutoff = 0.5f;

    // Shrink to the occupied box. An item texture is mostly empty, and
    // without this every prop's bounds would be the full 16-cube.
    bool trim = true;

    // Applied to every voxel. Metal items read better with a little of both,
    // but nothing in the texture says which items are metal, so it is left
    // to the caller rather than guessed from the file name.
    float roughness = 1.0f;
    float metallic = 0.0f;

    // Linear radiance added to every voxel, for the ones that should glow.
    Vec3 emission{0.0f, 0.0f, 0.0f};
};

// Where the game keeps them.
inline std::string texturePath(const std::string& name) {
    return "assets/minecraft/textures/item/" + name + ".png";
}

// From an already-decoded texture.
void buildModel(const Texture& texture, VoxelModel& out, const ItemOptions& options = {});

// From a path inside the pack.
bool loadModel(const AssetSource& source, const std::string& path, VoxelModel& out,
               const ItemOptions& options = {}, std::string* error = nullptr);

// From a bare item name: "diamond_sword", "apple", "golden_pickaxe".
bool loadByName(const AssetSource& source, const std::string& name, VoxelModel& out,
                const ItemOptions& options = {}, std::string* error = nullptr);

} // namespace item
} // namespace blocky
