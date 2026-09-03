#pragma once
// What a block is, and the registry that holds them. Not *which* blocks --
// the engine ships none, and a `BlockRegistry` arrives holding air and
// nothing else.
//
// A block is a name plus flat material constants, plus what physics needs to
// know about it. Texture references live beside the palette that names them,
// in `BlockTextureLibrary`, so that the albedo here stays the fallback an
// untextured render draws with.
#include "engine/core/math.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace blocky {

using BlockId = uint16_t;

struct BlockDef {
    std::string name;

    // Linear-light material constants. Never store sRGB values here.
    Vec3  albedo{0.8f, 0.8f, 0.8f};
    Vec3  emission{0.0f};   // radiance, may exceed 1
    float roughness = 1.0f; // 1 = fully diffuse, 0 = mirror
    float metallic  = 0.0f;

    // Opaque blocks stop both camera rays and shadow rays. Air is the only
    // block that is neither opaque nor drawn.
    bool opaque = true;

    // Whether a body stops here, and whether this block is a fluid.
    //
    // Asked of the palette rather than of the id, and that is the whole point
    // of them existing: `physics/` has no way to know that block 10 is water,
    // and no business knowing it. Before these flags the two questions were
    // answered by comparing ids against a built-in palette, which was honest
    // only for that palette -- anything a caller registered itself came out
    // solid whether it meant to or not.
    //
    // A block that says nothing is solid and dry. That is the common case and
    // the one a caller filling a registry means, so the defaults cost nothing.
    //
    // Fluid is not the opposite of solid. Air is passable too, and exactly one
    // of the two holds a swimmer up -- which is why these are two flags and
    // not one enum with three values pretending the third never happens.
    bool solid = true;
    bool fluid = false;

    // Dielectrics. `transmission` above zero turns the block into a refracting
    // medium: rays bend by `ior` at the surface and are attenuated inside it
    // by Beer-Lambert using `absorption`, given per block of travel. Water
    // absorbs red far faster than blue, which is the whole reason deep water
    // looks blue rather than merely tinted.
    float transmission = 0.0f;
    float ior = 1.5f;
    Vec3  absorption{0.0f};

    bool emissive() const { return emission.x > 0.0f || emission.y > 0.0f || emission.z > 0.0f; }
    bool transmissive() const { return transmission > 0.0f; }

    // Grass and leaves are tinted by biome in Minecraft; a per-face top tint
    // is enough to make them read correctly without a full biome system.
    bool  tintTop = false;
    Vec3  topTint{1.0f, 1.0f, 1.0f};

};

// The only block the engine knows by name.
//
// Id zero is the storage layer's own sentinel: an empty cell, a chunk that
// does not exist, what `World::get` answers past the edge of everything. That
// makes it structure rather than content, and it is why it survived the split
// that took stone, oak and water out of here.
//
// There used to be twenty-six more, and they were the reason `physics/` had
// to know that block ten was water and `assets/` had to carry a table of
// Minecraft texture names. Whoever runs a world now says what is in it:
// `palette::registry()` for the scenes, `game::palette()` for Konstruct, a local
// `BlockRegistry` for a test that wants two blocks and no more.
namespace block {
inline constexpr BlockId Air = 0;
} // namespace block

class BlockRegistry {
public:
    BlockRegistry();

    BlockId add(BlockDef def);

    const BlockDef& operator[](BlockId id) const {
        return id < defs_.size() ? defs_[id] : defs_[0];
    }
    BlockDef& mutableDef(BlockId id) { return defs_[id]; }

    // Returns block::Air when the name is unknown.
    BlockId find(const std::string& name) const;

    // The two questions physics asks, as one lookup each. They live here
    // rather than beside the solver so that no caller anywhere has to compare
    // an id against a name it should not know.
    bool isSolid(BlockId id) const { return (*this)[id].solid; }
    bool isFluid(BlockId id) const { return (*this)[id].fluid; }

    size_t size() const { return defs_.size(); }

private:
    std::vector<BlockDef> defs_;
    std::unordered_map<std::string, BlockId> byName_;
};

} // namespace blocky
