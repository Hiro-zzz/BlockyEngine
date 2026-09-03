#pragma once
// What Konstruct is built out of: its blocks, and what they look like.
//
// This file exists because the engine stopped having a palette. `blocky` knows
// nothing about stone or oak or water -- a `BlockRegistry` arrives empty
// except for air, and whoever runs a world says what goes in it. That is the
// line between the two names: `blocky` is a renderer, a solver and a codec
// with no opinions, and Konstruct is one thing built with them, with all the
// opinions.
//
// Air is the exception and not an inconsistency. Id zero is the storage
// layer's own sentinel -- an empty cell, a chunk that does not exist, the
// value `World::get` returns past the edge of everything -- so it belongs to
// the engine the way `nullptr` belongs to the language rather than to any
// program written in it.
//
// ------------------------------------------------------------------- ids
//
// The constants below are the order `palette()` registers in, and they have
// to stay in step with it: an id is an index. They are `game::block::`, and
// inside `namespace game` that is what a bare `block::Stone` finds, because
// the nearer namespace wins over the `using namespace blocky` above it.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/assets/blocks/texture_gen.hpp"
#include "engine/world/block.hpp"

namespace game {

namespace block {

// Air is the engine's, and named here only so that one namespace answers for
// every block Konstruct can refer to.
inline constexpr blocky::BlockId Air          = blocky::block::Air;

inline constexpr blocky::BlockId Stone        = 1;
inline constexpr blocky::BlockId Cobblestone  = 2;
inline constexpr blocky::BlockId Dirt         = 3;
inline constexpr blocky::BlockId GrassBlock   = 4;
inline constexpr blocky::BlockId Sand         = 5;
inline constexpr blocky::BlockId Gravel       = 6;
inline constexpr blocky::BlockId OakLog       = 7;
inline constexpr blocky::BlockId OakPlanks    = 8;
inline constexpr blocky::BlockId OakLeaves    = 9;
inline constexpr blocky::BlockId Water        = 10;
inline constexpr blocky::BlockId Glass        = 11;
inline constexpr blocky::BlockId Glowstone    = 12;
inline constexpr blocky::BlockId Lava         = 13;
inline constexpr blocky::BlockId Snow         = 14;
inline constexpr blocky::BlockId Obsidian     = 15;
inline constexpr blocky::BlockId WhiteWool    = 16;
inline constexpr blocky::BlockId RedWool      = 17;
inline constexpr blocky::BlockId BlackWool    = 18;
inline constexpr blocky::BlockId Bricks       = 19;
inline constexpr blocky::BlockId IronBlock    = 20;
inline constexpr blocky::BlockId GoldBlock    = 21;
inline constexpr blocky::BlockId Netherrack   = 22;
inline constexpr blocky::BlockId BirchLog     = 23;
inline constexpr blocky::BlockId BirchLeaves  = 24;
inline constexpr blocky::BlockId SpruceLog    = 25;
inline constexpr blocky::BlockId SpruceLeaves = 26;
inline constexpr blocky::BlockId Count        = 27;

} // namespace block

// The palette every Konstruct world is built from. Filled once, on the first
// call, in exactly the order of the constants above.
//
// A world is handed this: `World world(game::palette())`. Nothing reaches for
// it behind the world's back, so a second palette -- a test that wants two
// blocks and no more -- is a local `BlockRegistry` and needs no permission.
blocky::BlockRegistry& palette();

// Konstruct's own textures, painted in code rather than loaded.
//
// The verbs are the engine's (`texgen::Tile`); which verbs make grass is
// this game's business, and it is the reason no Minecraft install is involved
// at any point. Returns how many blocks came out textured.
//
// Water and lava are skipped on purpose: they are media, and the tracer takes
// their colour from absorption along the ray rather than from a surface.
int generateTextures(blocky::BlockTextureLibrary& library,
                     const blocky::BlockRegistry& registry,
                     const blocky::texgen::Settings& settings = {});

} // namespace game
