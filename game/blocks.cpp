#include "game/blocks.hpp"

// Konstruct's palette and its textures. Both were in the engine until the
// split: the palette as `BlockRegistry`'s constructor, the recipes as
// `texgen::generatePalette`. Neither was ever engine work -- one is a list of
// twenty-six things this game happens to be made of, the other is what they
// look like -- and both had reached into `block::` for ids the engine had no
// way to know the meaning of.

namespace game {
namespace {

using namespace blocky;


// Palette entries are authored the way a texture is: as sRGB bytes sampled
// from the real block. srgb() converts them into the linear space the
// renderer works in.
Vec3 srgb(int r, int g, int b) {
    return srgbToLinear(Vec3{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f});
}

// The same conversion under the name the texture recipes were written with.
// Both halves arrived here from different files; renaming either would make a
// diff that reads like a change.
Vec3 rgb(int r, int g, int b) { return srgb(r, g, b); }

BlockDef solid(const char* name, Vec3 albedo, float roughness = 1.0f) {
    BlockDef d;
    d.name = name;
    d.albedo = albedo;
    d.roughness = roughness;
    return d;
}

BlockRegistry build() {
    BlockRegistry reg;

    // Air is already in: a registry arrives holding it at id 0, because the
    // whole storage layer treats zero as "nothing here". Everything after it
    // is this game's.
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
}

} // namespace

BlockRegistry& palette() {
    static BlockRegistry registry = build();
    return registry;
}

int generateTextures(BlockTextureLibrary& library, const BlockRegistry& registry,
                     const texgen::Settings& settings) {
    using namespace blocky::texgen;
    Rng rng(settings.seed);
    int textured = 0;

    auto make = [&](const char* name, auto&& paint) {
        Tile tile(settings.size, rng);
        paint(tile);
        tile.edgeShade(settings.edgeShade);
        return library.addTexture(name, tile.toTexture());
    };

    // An overlay keeps its own alpha and must not be rimmed: the rim would
    // draw a dark line down a fringe that is supposed to be ragged.
    auto makeOverlay = [&](const char* name, auto&& paint) {
        Tile tile(settings.size, rng);
        tile.clearAlpha(0.0f);
        paint(tile);
        return library.addTexture(name, tile.toTexture());
    };

    auto bind = [&](BlockId id, int top, int side, int bottom, int overlay = -1) {
        if (id >= BlockId(registry.size())) return;
        if (library.bindIndices(id, top, side, bottom, {1.0f, 1.0f, 1.0f}, overlay)) ++textured;
    };

    // ------------------------------------------------------------- ground
    int stone = make("stone", [](Tile& t) {
        t.grain(rgb(108, 108, 112), rgb(140, 140, 146), 0.16f, 4);
        t.speckle(rgb(88, 88, 94), 0.06f, 0.7f);
    });

    int cobble = make("cobblestone", [](Tile& t) {
        t.pebbles(rgb(96, 96, 100), rgb(152, 152, 158), rgb(64, 64, 68), 11, 7.0f);
        t.speckle(rgb(80, 80, 86), 0.05f, 0.6f);
    });

    int dirt = make("dirt", [](Tile& t) {
        t.grain(rgb(104, 76, 52), rgb(140, 104, 72), 0.22f, 4);
        t.speckle(rgb(74, 54, 38), 0.10f, 0.8f);
    });

    int grassTop = make("grass_top", [](Tile& t) {
        t.grain(rgb(76, 108, 54), rgb(104, 138, 72), 0.28f, 4);
        t.speckle(rgb(64, 94, 46), 0.12f, 0.7f);
    });

    // The fringe is where a grass top wraps over the edge of the block. Doing
    // it as an overlay rather than as a second dirt texture is what lets one
    // dirt tile serve both blocks.
    int grassSide = makeOverlay("grass_side_overlay", [&](Tile& t) {
        t.fringe(rgb(84, 118, 58), std::max(2, settings.size / 8), std::max(1, settings.size / 10));
    });

    int sand = make("sand", [](Tile& t) {
        t.grain(rgb(214, 200, 154), rgb(236, 226, 186), 0.30f, 3);
        t.speckle(rgb(198, 182, 136), 0.08f, 0.5f);
    });

    int gravel = make("gravel", [](Tile& t) {
        t.pebbles(rgb(104, 100, 98), rgb(158, 152, 148), rgb(78, 74, 72), 22, 4.0f);
        t.speckle(rgb(190, 186, 182), 0.04f, 0.5f);
    });

    int snow = make("snow", [](Tile& t) {
        t.grain(rgb(232, 238, 246), rgb(252, 254, 255), 0.20f, 3);
        t.speckle(rgb(214, 224, 238), 0.05f, 0.4f);
    });

    int netherrack = make("netherrack", [](Tile& t) {
        t.grain(rgb(104, 44, 44), rgb(146, 62, 60), 0.30f, 4);
        t.speckle(rgb(74, 30, 30), 0.14f, 0.8f);
    });

    int obsidian = make("obsidian", [](Tile& t) {
        t.grain(rgb(26, 20, 36), rgb(46, 36, 62), 0.22f, 4);
        t.speckle(rgb(120, 96, 168), 0.03f, 0.6f);
    });

    // --------------------------------------------------------------- wood
    int planks = make("oak_planks", [](Tile& t) {
        t.boards(rgb(168, 132, 82), rgb(120, 92, 56), 4, 0.10f);
    });

    int oakBark = make("oak_log_side", [](Tile& t) {
        t.streaks(rgb(90, 68, 42), rgb(126, 98, 62), 0.55f);
    });
    int oakEnd = make("oak_log_top", [](Tile& t) {
        t.rings(rgb(176, 142, 92), rgb(132, 102, 62), 1.2f);
    });

    int birchBark = make("birch_log_side", [](Tile& t) {
        t.fill(rgb(216, 212, 198));
        t.streaks(rgb(198, 196, 184), rgb(232, 230, 220), 0.5f);
        t.speckle(rgb(58, 56, 52), 0.05f, 0.9f);
    });
    int birchEnd = make("birch_log_top", [](Tile& t) {
        t.rings(rgb(206, 186, 140), rgb(168, 148, 106), 1.3f);
    });

    int spruceBark = make("spruce_log_side", [](Tile& t) {
        t.streaks(rgb(58, 42, 26), rgb(88, 66, 42), 0.6f);
    });
    int spruceEnd = make("spruce_log_top", [](Tile& t) {
        t.rings(rgb(132, 102, 66), rgb(94, 72, 44), 1.25f);
    });

    auto leaves = [&](const char* name, Vec3 low, Vec3 high) {
        Tile tile(settings.size, rng);
        tile.grain(low, high, 0.42f, 3);
        tile.speckle(low * 0.7f, 0.18f, 0.8f);
        tile.edgeShade(settings.edgeShade);
        // Cut *after* the rim: a hole has no shading to receive.
        tile.holes(0.30f);
        return library.addTexture(name, tile.toTexture());
    };

    int oakLeaves = leaves("oak_leaves", rgb(52, 84, 40), rgb(84, 118, 56));
    int birchLeaves = leaves("birch_leaves", rgb(84, 110, 56), rgb(118, 142, 78));
    int spruceLeaves = leaves("spruce_leaves", rgb(36, 62, 44), rgb(58, 88, 60));

    // ------------------------------------------------------- manufactured
    int bricks = make("bricks", [&](Tile& t) {
        t.grain(rgb(148, 82, 66), rgb(178, 104, 84), 0.30f, 3);
        t.courses(rgb(196, 190, 182), std::max(2, settings.size / 8),
                  std::max(2, settings.size / 16), true);
    });

    int iron = make("iron_block", [&](Tile& t) {
        t.panel(rgb(214, 214, 216), rgb(178, 178, 182), std::max(1, settings.size / 16));
        t.speckle(rgb(232, 232, 236), 0.04f, 0.4f);
    });

    int gold = make("gold_block", [&](Tile& t) {
        t.panel(rgb(242, 206, 82), rgb(206, 166, 52), std::max(1, settings.size / 16));
        t.speckle(rgb(255, 236, 150), 0.05f, 0.5f);
    });

    int glass = make("glass", [&](Tile& t) {
        t.fill(rgb(226, 240, 246));
        t.panel(rgb(226, 240, 246), rgb(196, 216, 226), std::max(1, settings.size / 16));
    });

    int glowstone = make("glowstone", [](Tile& t) {
        t.grain(rgb(184, 148, 88), rgb(226, 196, 128), 0.28f, 3);
        t.speckle(rgb(255, 244, 196), 0.16f, 1.0f);
    });

    auto wool = [&](const char* name, Vec3 base) {
        Tile tile(settings.size, rng);
        tile.grain(base * 0.88f, base * 1.08f, 0.5f, 2);
        tile.speckle(base * 1.15f, 0.10f, 0.4f);
        tile.edgeShade(settings.edgeShade);
        return library.addTexture(name, tile.toTexture());
    };

    int whiteWool = wool("white_wool", rgb(226, 228, 230));
    int redWool = wool("red_wool", rgb(158, 52, 46));
    int blackWool = wool("black_wool", rgb(58, 56, 62));

    // -------------------------------------------------------------- bind
    bind(block::Stone, stone, stone, stone);
    bind(block::Cobblestone, cobble, cobble, cobble);
    bind(block::Dirt, dirt, dirt, dirt);
    bind(block::GrassBlock, grassTop, dirt, dirt, grassSide);
    bind(block::Sand, sand, sand, sand);
    bind(block::Gravel, gravel, gravel, gravel);
    bind(block::Snow, snow, snow, snow);
    bind(block::Netherrack, netherrack, netherrack, netherrack);
    bind(block::Obsidian, obsidian, obsidian, obsidian);

    bind(block::OakPlanks, planks, planks, planks);
    bind(block::OakLog, oakEnd, oakBark, oakEnd);
    bind(block::BirchLog, birchEnd, birchBark, birchEnd);
    bind(block::SpruceLog, spruceEnd, spruceBark, spruceEnd);
    bind(block::OakLeaves, oakLeaves, oakLeaves, oakLeaves);
    bind(block::BirchLeaves, birchLeaves, birchLeaves, birchLeaves);
    bind(block::SpruceLeaves, spruceLeaves, spruceLeaves, spruceLeaves);

    bind(block::Bricks, bricks, bricks, bricks);
    bind(block::IronBlock, iron, iron, iron);
    bind(block::GoldBlock, gold, gold, gold);
    bind(block::Glass, glass, glass, glass);
    bind(block::Glowstone, glowstone, glowstone, glowstone);
    bind(block::WhiteWool, whiteWool, whiteWool, whiteWool);
    bind(block::RedWool, redWool, redWool, redWool);
    bind(block::BlackWool, blackWool, blackWool, blackWool);

    // Water and lava are left alone. They are media -- the tracer gets their
    // colour from absorption along the path the ray takes inside them, not
    // from a surface, and giving them one would be painting over the physics.
    return textured;
}

} // namespace game
