#pragma once
// Ways of strewing sprites around, which is what "particles" means when the
// scene never moves.
//
// There is no emitter and no simulation to step. A still frame wants the
// *result* of one -- snow already in the air, embers already risen off the
// lava, dust already hanging in the shaft of light. So these functions place
// the frozen instant directly, and the interesting parameter is the shape of
// the volume rather than any velocity.
#include "engine/sprite/sprite.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <vector>

namespace blocky {
namespace scatter {

struct ParticleStyle {
    // Edge length in blocks. A Minecraft particle is roughly a tenth.
    Vec2 size{0.12f, 0.12f};

    // Fraction the size may wander either way, so a drift does not read as a
    // grid of identical stamps.
    float sizeJitter = 0.4f;

    // Null leaves the quad flat `tint`, which is all a dust mote needs.
    const Texture* texture = nullptr;

    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 emission{0.0f, 0.0f, 0.0f};

    // Turned at random about +Y and in their own plane. Particles are the one
    // case where per-sprite orientation is right: they have no shared reading
    // direction to preserve, unlike the glyphs of a label.
    bool randomYaw = true;
    bool randomRoll = true;

    float alphaCutoff = 0.5f;
    bool  doubleSided = true;
};

// Uniformly through a box.
std::vector<Sprite> inBox(Vec3 lo, Vec3 hi, int count, uint32_t seed,
                          const ParticleStyle& style = {});

// Uniformly through a ball, by volume rather than by radius -- sampling the
// radius directly would pile everything into the middle.
std::vector<Sprite> inSphere(Vec3 centre, float radius, int count, uint32_t seed,
                             const ParticleStyle& style = {});

// Dust caught in a shaft of light: a cylinder of `radius` running `length`
// along `direction`, which need not be normalised.
std::vector<Sprite> inBeam(Vec3 from, Vec3 direction, float length, float radius, int count,
                           uint32_t seed, const ParticleStyle& style = {});

// Settled just above whatever surface the world offers under each point.
// Columns with nothing in them are skipped, so the count is an upper bound.
std::vector<Sprite> onSurface(const World& world, IVec3 lo, IVec3 hi, int topY, int count,
                              uint32_t seed, float heightAbove = 0.15f,
                              const ParticleStyle& style = {});

// Rising off every exposed block of `id` in the region: embers over lava,
// bubbles over water. Density falls off with height, the way it would if
// something were actually emitting them.
std::vector<Sprite> above(const World& world, IVec3 lo, IVec3 hi, BlockId id, float height,
                          int perBlock, uint32_t seed, const ParticleStyle& style = {});

// Drops sprites whose centre sits inside a block that stops light. Volume
// scatters do not know about the world, so this is how motes stop appearing
// buried in the terrain they were strewn across.
void removeInsideSolid(std::vector<Sprite>& sprites, const World& world);

} // namespace scatter
} // namespace blocky
