#pragma once
// One query that both renderers use, so blocks and entities are lit,
// shadowed and bounced off by exactly the same code.
//
// The voxel world, the entity boxes, the sprite quads and the prop grids are
// four completely different structures with four completely different
// traversals -- a two-level DDA, a flat list of oriented boxes, a BVH over
// flat quads, and a BVH over transformed grids each walked by its own DDA.
// Rather than teach the integrators about all four, they are resolved here
// into a single hit that already carries its material.
//
// This is the only place in the renderer that knows there is more than one
// kind of geometry. Everything above it shades a SceneHit and never asks.
#include "engine/entity/entity.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/trace/surface.hpp"
#include "engine/scene/scene.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/world/raycast.hpp"

namespace blocky {

struct SceneHit {
    float t = 0.0f;
    Vec3  position{};
    Vec3  normal{};      // unit, world space, facing the incoming ray

    // Material, already resolved -- textures sampled, tints applied.
    Vec3  albedo{};
    Vec3  emission{};
    float roughness = 1.0f;
    float metallic = 0.0f;

    // Dielectric specular coat, written only by Scene::materialStyle. Zero on
    // every hit of an unstyled scene, which is what keeps the realistic path
    // bit-for-bit what it was before styles existed.
    float coat = 0.0f;
    float coatRoughness = 0.14f;

    bool  transmissive = false;
    float ior = 1.5f;
    Vec3  absorption{};

    // Where it came from. Block hits carry their voxel data so that the
    // direct renderer can still compute corner ambient occlusion; entity hits
    // have no such neighbourhood.
    bool    isBlock = true;
    BlockId blockId = block::Air;
    IVec3   block{};
    IVec3   blockNormal{};
    int     axis = 0;
};

// Nearest surface along the ray. `filter` is passed through to the voxel
// traversal, so a ray travelling inside water still reports where the water
// ends -- while entities, sprites and props are always tested, since a
// character can stand in the sea and a bubble can hang in it.
bool intersectScene(const Scene& scene, const Ray& ray, float maxDistance, RayFilter filter,
                    SceneHit& hit);

// Fraction of light surviving the ray: voxel media attenuate it, opaque
// blocks, entities, sprites and props stop it dead.
Vec3 sceneTransmittance(const Scene& scene, const Ray& ray, float maxDistance);

} // namespace blocky
