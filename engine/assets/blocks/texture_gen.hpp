#pragma once
// Block textures built in code instead of read from a game.
//
// The engine could always render without Minecraft installed, but what it fell
// back to was one flat colour per block -- enough to read a shape, not enough
// to look at. This makes a full set of surfaces the engine owns outright:
// stone that is mottled, planks with a grain and a seam, cobble with distinct
// stones in it, leaves with holes.
//
// It goes in through `BlockTextureLibrary::addTexture` / `bindIndices`, the
// same seam a resource pack uses, so nothing above the library can tell where
// a texture came from. That was the point of that seam existing.
//
// ------------------------------------------------------------------- pixels
//
// The default is 32 a face rather than the game's 16. Sixteen is a look, and
// a deliberate one; but it is a look chosen for hand-drawn tiles, and noise
// at that size is mush. Thirty-two keeps the blockiness and gives the grain
// somewhere to live. The viewport's texture array asks the library how big
// its tiles are, so raising this costs nothing else.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/assets/texture.hpp"
#include "engine/core/image.hpp"
#include "engine/core/random.hpp"

#include <cstdint>

namespace blocky::texgen {

struct Settings {
    int size = 32;
    uint32_t seed = 0x9e3779b9u;

    // How much darker the outermost texels of a tile are.
    //
    // The first attempt was a diagonal bevel -- lighter towards the top-left,
    // darker towards the bottom-right -- on the theory that a flat-lit voxel
    // face needs internal shading to read as a surface. It does, but not that
    // one: every tile got the *same* gradient, so a wall of them was a
    // repeating diagonal pattern, which is worse than flat. It also asserts a
    // light direction the renderer has not agreed to.
    //
    // Darkening the rim instead tiles perfectly, because each block's edge
    // meets its neighbour's edge and the seam reads as the boundary it
    // actually is. Zero turns it off.
    float edgeShade = 0.14f;
};

// One tile under construction: linear RGB plus alpha, square, `size` a side.
//
// Deliberately a small pile of verbs rather than a general image library.
// Every one of them exists because some block needed it, and a block that
// needs a new one gets a new verb -- which keeps this readable as a list of
// what surfaces are made of.
class Tile {
public:
    Tile(int size, Rng& rng);

    void fill(Vec3 colour);

    // Fractal noise across the tile, lerping `low` to `high`. The workhorse:
    // stone, dirt, sand and snow are all this with different colours.
    void grain(Vec3 low, Vec3 high, float frequency, int octaves = 4);

    // Individual texels of another colour, scattered. Gravel's flecks, the
    // glitter in an ore, the dry bits in sand.
    void speckle(Vec3 colour, float chance, float strength = 1.0f);

    // Rounded blobs, for cobble's stones and gravel's pebbles.
    void pebbles(Vec3 low, Vec3 high, Vec3 gap, int count, float radius);

    // A staggered grid of mortar lines: bricks.
    void courses(Vec3 mortar, int rows, int columns, bool stagger);

    // Vertical boards with a darker seam between them, and grain along each.
    void boards(Vec3 wood, Vec3 seam, int count, float grainStrength);

    // Concentric rings about the centre: the cut end of a log.
    void rings(Vec3 light, Vec3 dark, float spacing);

    // Vertical streaks: bark, and the side of anything fibrous.
    void streaks(Vec3 low, Vec3 high, float density);

    // A band of `colour` along the top edge with a ragged lower boundary,
    // written into alpha as well -- this is the grass fringe that hangs over
    // a dirt side, and it is an *overlay*, so the rest of the tile is clear.
    void fringe(Vec3 colour, int depth, int ragged);

    // Punches holes in alpha, for leaves.
    void holes(float chance);

    // Flat panels with a rim: iron, gold, wool, the manufactured-looking
    // blocks that would look wrong with noise on them.
    void panel(Vec3 face, Vec3 rim, int inset);

    // Darkens the outermost ring of texels; see Settings::edgeShade.
    void edgeShade(float strength);
    void clearAlpha(float value);

    Texture toTexture() const;

private:
    Vec3& at(int x, int y) { return pixels_[size_t(y) * size_t(size_) + size_t(x)]; }
    float& alphaAt(int x, int y) { return alpha_[size_t(y) * size_t(size_) + size_t(x)]; }

    int size_;
    Rng& rng_;
    uint32_t noiseSeed_;
    std::vector<Vec3> pixels_;
    std::vector<float> alpha_;
};

// There is no `generatePalette` here any more, and the absence is the point:
// a set of recipes is a set of blocks somebody decided on, and binding them
// needs ids this module has no way to know. `Tile` is the vocabulary; the
// sentences are written where the palette is. See `game::generateTextures`.

} // namespace blocky::texgen
