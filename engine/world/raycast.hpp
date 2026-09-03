#pragma once
// Ray traversal of the voxel world.
//
// Two levels of Amanatides-Woo DDA: the outer one walks 16-block chunk cells
// and costs one hash lookup per cell, the inner one walks blocks and only
// runs inside chunks that actually exist. Empty space is therefore skipped
// 16 blocks at a time, which is what makes large scenes tractable on the CPU.
#include "engine/core/math.hpp"
#include "engine/world/world.hpp"

namespace blocky {

struct Ray {
    Vec3 origin;
    Vec3 direction;  // expected to be normalised
};

struct RayHit {
    float   t = 0.0f;          // distance along the ray
    Vec3    position{};        // exact point on the block face
    IVec3   block{};           // integer coordinate of the block hit
    IVec3   normal{};          // face normal, one component +/-1
    BlockId id = 0;
    int     axis = 0;          // 0 = X face, 1 = Y face, 2 = Z face
    Vec2    uv{};              // position across the face, both in [0, 1)
};

// What the traversal treats as empty.
struct RayFilter {
    // Blocks with this id are passed through. Air by default, which makes a
    // camera ray stop at the first non-air block.
    //
    // Set it to Water while a ray is *inside* water and the traversal instead
    // reports where the water ends -- and there, air counts as a hit, since
    // air is the far side of the interface being looked for.
    BlockId passThrough = block::Air;

    // Ignore anything that does not block light. For shadow rays.
    bool opaqueOnly = false;
};

// First block along the ray that the filter does not pass through.
//
// In medium mode (passThrough set to something other than air) the traversal
// always reports a hit while the ray is inside the world: leaving a populated
// chunk, or leaving the populated region altogether, both end the medium.
bool raycast(const World& world, const Ray& ray, float maxDistance, RayHit& hit,
             RayFilter filter = {});

// Cheaper query for shadow rays: stops at the first opaque block and never
// fills in surface data.
bool raycastOccluded(const World& world, const Ray& ray, float maxDistance);

// Fraction of light surviving the ray, per colour channel.
//
// Zero as soon as an opaque block is in the way; otherwise the Beer-Lambert
// attenuation of every transmissive block the ray passes through, weighted by
// how far it travels inside each one. This is what a shadow ray cast from the
// seabed needs: sunlight reaching it has already crossed metres of water.
//
// Refraction is deliberately ignored here -- a bent shadow ray no longer
// points at the light, and every renderer makes the same simplification.
Vec3 rayTransmittance(const World& world, const Ray& ray, float maxDistance);

} // namespace blocky
