#pragma once
// The palette the scenes are built from.
//
// The engine has none. A `BlockRegistry` arrives holding air and nothing
// else, because "there is such a thing as stone, and it is grey" is a fact
// about a world somebody is making rather than about how worlds are made --
// and `blocky` is the second thing. Air stays down there because id zero is
// the storage layer's own sentinel for an empty cell, the way `nullptr`
// belongs to the language rather than to a program.
//
// So this is the scenes' answer, shared across `scenes/` the way
// `common/island.hpp` and `common/props.hpp` are shared: one header, compiled
// into each scene executable, holding what a diorama is made of. Konstruct
// keeps its own in `game/blocks.hpp` and the two are free to drift, which is
// the point -- a game tuning how its stone looks should not be editing the
// thing `scenes/cornell.cpp` renders.
//
// Use it as:
//
//     Scene scene(palette::registry());
//     scene.world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);
//
// Ids are the order `registry()` registers in, because an id *is* an index.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/world/block.hpp"

#include <vector>

namespace palette {

using namespace blocky;

// Air is the engine's; named here so that one namespace answers for every
// block a scene can refer to.
inline constexpr BlockId Air          = block::Air;

inline constexpr BlockId Stone        = 1;
inline constexpr BlockId Cobblestone  = 2;
inline constexpr BlockId Dirt         = 3;
inline constexpr BlockId GrassBlock   = 4;
inline constexpr BlockId Sand         = 5;
inline constexpr BlockId Gravel       = 6;
inline constexpr BlockId OakLog       = 7;
inline constexpr BlockId OakPlanks    = 8;
inline constexpr BlockId OakLeaves    = 9;
inline constexpr BlockId Water        = 10;
inline constexpr BlockId Glass        = 11;
inline constexpr BlockId Glowstone    = 12;
inline constexpr BlockId Lava         = 13;
inline constexpr BlockId Snow         = 14;
inline constexpr BlockId Obsidian     = 15;
inline constexpr BlockId WhiteWool    = 16;
inline constexpr BlockId RedWool      = 17;
inline constexpr BlockId BlackWool    = 18;
inline constexpr BlockId Bricks       = 19;
inline constexpr BlockId IronBlock    = 20;
inline constexpr BlockId GoldBlock    = 21;
inline constexpr BlockId Netherrack   = 22;
inline constexpr BlockId BirchLog     = 23;
inline constexpr BlockId BirchLeaves  = 24;
inline constexpr BlockId SpruceLog    = 25;
inline constexpr BlockId SpruceLeaves = 26;
inline constexpr BlockId Count        = 27;

namespace detail {


// Palette entries are authored the way a texture is: as sRGB bytes sampled
// from the real block. srgb() converts them into the linear space the
// renderer works in.
inline Vec3 srgb(int r, int g, int b) {
    return srgbToLinear(Vec3{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f});
}

inline BlockDef solid(const char* name, Vec3 albedo, float roughness = 1.0f) {
    BlockDef d;
    d.name = name;
    d.albedo = albedo;
    d.roughness = roughness;
    return d;
}
} // namespace detail

// The registry every scene's world is built from. Filled once, on first use,
// in exactly the order of the constants above.
inline BlockRegistry& registry() {
    static BlockRegistry built = [] {
        using detail::srgb;
        using detail::solid;
        BlockRegistry reg;

        // Air is already in: a registry arrives holding it at id 0, because
        // the whole storage layer treats zero as "nothing here".
        reg.add(solid("stone",       srgb(125, 125, 125)));
        reg.add(solid("cobblestone", srgb(122, 122, 122)));
        reg.add(solid("dirt",        srgb(134,  96,  67)));

        BlockDef grass = solid("grass_block", srgb(134, 96, 67));
        grass.tintTop = true;
        grass.topTint = srgb(91, 153, 52);
        reg.add(grass);

        reg.add(solid("sand",         srgb(219, 207, 163)));
        reg.add(solid("gravel",       srgb(131, 127, 126)));
        reg.add(solid("oak_log",      srgb(102,  81,  50)));
        reg.add(solid("oak_planks",   srgb(162, 131,  79)));
        reg.add(solid("oak_leaves",   srgb( 60, 143,  40)));

        // Water is a refracting medium, not a blue surface. The albedo is almost
        // unused: the colour comes from absorption over distance, which is why
        // shallow water reads clear and deep water reads blue.
        BlockDef water = solid("water", srgb(230, 240, 255), 0.0f);
        water.opaque = false;
        water.transmission = 1.0f;
        water.ior = 1.333f;
        water.absorption = Vec3{0.42f, 0.09f, 0.035f};  // per block travelled
        water.solid = false;
        water.fluid = true;
        reg.add(water);

        BlockDef glass = solid("glass", srgb(255, 255, 255), 0.0f);
        glass.opaque = false;
        glass.transmission = 1.0f;
        glass.ior = 1.52f;
        glass.absorption = Vec3{0.02f, 0.015f, 0.02f};
        reg.add(glass);

        BlockDef glowstone = solid("glowstone", srgb(249, 222, 150));
        glowstone.emission = srgb(255, 224, 160) * 9.0f;
        reg.add(glowstone);

        BlockDef lava = solid("lava", srgb(207, 92, 22));
        lava.emission = srgb(255, 130, 40) * 14.0f;
        lava.solid = false;
        lava.fluid = true;
        reg.add(lava);

        reg.add(solid("snow",     srgb(249, 254, 254)));
        reg.add(solid("obsidian", srgb( 21,  18,  30), 0.35f));

        reg.add(solid("white_wool", srgb(233, 236, 236)));
        reg.add(solid("red_wool",   srgb(160,  39,  34)));
        reg.add(solid("black_wool", srgb( 20,  21,  25)));
        reg.add(solid("bricks",     srgb(150,  97,  83)));

        BlockDef iron = solid("iron_block", srgb(220, 220, 220), 0.28f);
        iron.metallic = 1.0f;
        reg.add(iron);

        BlockDef gold = solid("gold_block", srgb(249, 236, 79), 0.18f);
        gold.metallic = 1.0f;
        reg.add(gold);

        reg.add(solid("netherrack", srgb(97, 38, 38)));

        // A second and third tree species, so procedural forests are not all oak.
        reg.add(solid("birch_log",     srgb(216, 214, 207)));
        reg.add(solid("birch_leaves",  srgb(128, 167,  85)));
        reg.add(solid("spruce_log",    srgb( 58,  39,  20)));
        reg.add(solid("spruce_leaves", srgb( 97, 153,  97)));
        return reg;
    }();
    return built;
}

// Biome tints. Minecraft ships grass and foliage as greyscale art and
// multiplies a per-biome colour onto it at runtime; without a biome system we
// pick the plains values, which is what most builds are photographed in.
inline const Vec3 kGrassTint   = srgbToLinear(Vec3{145.0f / 255.0f, 189.0f / 255.0f, 89.0f / 255.0f});
inline const Vec3 kFoliageTint = srgbToLinear(Vec3{119.0f / 255.0f, 171.0f / 255.0f, 47.0f / 255.0f});
inline const Vec3 kNoTint{1.0f, 1.0f, 1.0f};

inline BlockTextureRule uniform(BlockId id, const char* name, Vec3 tint = kNoTint) {
    return {id, name, name, name, nullptr, kNoTint, kNoTint, tint};
}

inline BlockTextureRule pillar(BlockId id, const char* topName, const char* sideName) {
    return {id, topName, sideName, topName, nullptr, kNoTint, kNoTint, kNoTint};
}

// How this palette maps onto vanilla texture names, for the scenes that read
// a real jar or resource pack.
//
// Real Minecraft derives this from the blockstate and model JSON tree, which
// is a substantial subsystem of its own. A flat table covers a hand-built
// palette exactly, and it is a table *about this palette* -- which is why it
// lives beside it rather than inside `BlockTextureLibrary`, whose job is
// loading and compositing whatever it is handed.
inline const std::vector<BlockTextureRule>& minecraftRules() {
    static const std::vector<BlockTextureRule> rules = {
        uniform(Stone,       "stone"),
        uniform(Cobblestone, "cobblestone"),
        uniform(Dirt,        "dirt"),

        // Vanilla grass: greyscale top times the biome tint, and a tinted
        // overlay strip composited onto an untinted dirt side.
        {GrassBlock, "grass_block_top", "grass_block_side", "dirt",
         "grass_block_side_overlay", kGrassTint, kGrassTint, kNoTint},

        uniform(Sand,       "sand"),
        uniform(Gravel,     "gravel"),
        pillar(OakLog,      "oak_log_top", "oak_log"),
        uniform(OakPlanks,  "oak_planks"),
        uniform(OakLeaves,  "oak_leaves", kFoliageTint),
        uniform(Glowstone,  "glowstone"),
        uniform(Lava,       "lava_still"),
        uniform(Snow,       "snow"),
        uniform(Obsidian,   "obsidian"),
        uniform(WhiteWool,  "white_wool"),
        uniform(RedWool,    "red_wool"),
        uniform(BlackWool,  "black_wool"),
        uniform(Bricks,     "bricks"),
        uniform(IronBlock,  "iron_block"),
        uniform(GoldBlock,  "gold_block"),
        uniform(Netherrack, "netherrack"),

        // Vanilla tints birch and spruce leaves with fixed colours rather
        // than the biome foliage value, so they get their own here.
        pillar(BirchLog,  "birch_log_top",  "birch_log"),
        uniform(BirchLeaves,  "birch_leaves",
                srgbToLinear(Vec3{128.0f / 255.0f, 167.0f / 255.0f, 85.0f / 255.0f})),
        pillar(SpruceLog, "spruce_log_top", "spruce_log"),
        uniform(SpruceLeaves, "spruce_leaves",
                srgbToLinear(Vec3{97.0f / 255.0f, 153.0f / 255.0f, 97.0f / 255.0f})),
        // Water and glass are handled as refracting media, not as surfaces,
        // so a texture would have nothing to be applied to.
    };
    return rules;
}

} // namespace palette
