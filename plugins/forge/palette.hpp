#pragma once
// What forge is made of: sixteen colours and two blocks.
//
// Both belong here rather than in the engine, for the reason
// `docs/architecture.md` gives at length: `BlockRegistry` arrives holding air,
// and "there is such a thing as a studio floor, and it is grey" is a fact
// about this program. Konstruct keeps its own palette, the scenes keep
// theirs, and a third one costs nothing because none of the three is the
// engine's.
//
// The swatches are written in sRGB, the way a person picks a colour, and
// converted on the way in. Everything past that point is linear.
#include "engine/core/image.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/render/gl/overlay.hpp"
#include "engine/world/block.hpp"

#include <vector>

namespace forge {

using namespace blocky;

// The floor the model stands on, so there is somewhere for a shadow to land
// and something for the eye to judge scale against.
inline constexpr BlockId Air   = block::Air;
inline constexpr BlockId Slab  = 1;
inline constexpr BlockId Tile  = 2;

// A reference, and filled once on first use, because `Scene` holds the
// registry rather than copying it -- the same shape `palette::registry()` has
// in `scenes/`, and for the same reason: an id is an index into this, so
// there had better be one of it.
inline BlockRegistry& registry() {
    static BlockRegistry built = [] {
        BlockRegistry reg;

        // Air is already in at id 0: the storage layer's own sentinel.
        BlockDef slab;
        slab.name = "studio_slab";
        slab.albedo = srgbToLinear(Vec3{0.34f, 0.35f, 0.38f});
        reg.add(slab);

        BlockDef tile;
        tile.name = "studio_tile";
        tile.albedo = srgbToLinear(Vec3{0.27f, 0.28f, 0.31f});
        reg.add(tile);
        return reg;
    }();
    return built;
}

struct Swatch {
    const char* name;
    int r, g, b;
};

// Sixteen, because that is how many fit across the bottom of a window at a
// size the eye can hit with a mouse, and because a palette that scrolls is a
// palette nobody learns the position of.
inline const std::vector<Swatch>& swatches() {
    static const std::vector<Swatch> list = {
        {"white",  238, 240, 243}, {"silver", 178, 183, 190}, {"grey",   120, 126, 134},
        {"slate",   72,  78,  87}, {"ink",     26,  29,  34}, {"brown",  110,  74,  46},
        {"wood",   158, 118,  74}, {"sand",   214, 188, 140}, {"moss",    88, 124,  62},
        {"leaf",   126, 168,  72}, {"teal",    58, 134, 140}, {"sky",     92, 148, 206},
        {"blue",    54,  84, 168}, {"plum",   124,  70, 146}, {"blood",  160,  46,  46},
        {"ember",  222, 108,  38},
    };
    return list;
}

inline ImageU8::RGBA swatchPixel(int index) {
    const Swatch& s = swatches()[size_t(index) % swatches().size()];
    return {uint8_t(s.r), uint8_t(s.g), uint8_t(s.b), 255};
}

inline Rgba swatchOverlay(int index) {
    const Swatch& s = swatches()[size_t(index) % swatches().size()];
    return rgb8(s.r, s.g, s.b);
}

inline VoxelMaterial swatchMaterial(int index) {
    const Swatch& s = swatches()[size_t(index) % swatches().size()];

    VoxelMaterial material;
    material.albedo = srgbToLinear(
        Vec3{float(s.r) / 255.0f, float(s.g) / 255.0f, float(s.b) / 255.0f});
    return material;
}

inline const char* swatchName(int index) {
    return swatches()[size_t(index) % swatches().size()].name;
}

} // namespace forge
