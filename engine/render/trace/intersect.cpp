#include "engine/render/trace/intersect.hpp"

namespace blocky {
namespace {

void fillFromBlock(const Scene& scene, const RayHit& raw, SceneHit& hit) {
    const BlockDef& def = scene.world.registry()[raw.id];

    hit.t = raw.t;
    hit.position = raw.position;
    hit.normal = toVec3(raw.normal);

    hit.albedo = surfaceAlbedo(scene, def, raw);
    hit.emission = def.emission;
    hit.roughness = def.roughness;
    hit.metallic = def.metallic;
    hit.transmissive = def.transmissive();
    hit.ior = def.ior;
    hit.absorption = def.absorption;

    hit.isBlock = true;
    hit.blockId = raw.id;
    hit.block = raw.block;
    hit.blockNormal = raw.normal;
    hit.axis = raw.axis;
}

void fillFromProp(const PropHit& raw, SceneHit& hit) {
    hit.t = raw.t;
    hit.position = raw.position;
    hit.normal = raw.normal;

    // A prop carries a full material, metallic included -- so a gold ingot is
    // an actual GGX conductor rather than a yellow diffuse brick. It is never
    // a medium: refraction is tracked by the block a ray is inside, and a prop
    // is not a block.
    hit.albedo = raw.albedo;
    hit.emission = raw.emission;
    hit.roughness = raw.roughness;
    hit.metallic = raw.metallic;
    hit.transmissive = false;
    hit.absorption = Vec3{0.0f};

    hit.isBlock = false;
    hit.blockId = block::Air;
}

void fillFromSprite(const SpriteHit& raw, SceneHit& hit) {
    hit.t = raw.t;
    hit.position = raw.position;
    hit.normal = raw.normal;

    // Paper, ash and glowing dust: matte, never metallic, never a medium.
    // Emission does come through, so an ember reads as an ember -- though it
    // lights only itself; see the note on LightSet in sprite_set.hpp.
    hit.albedo = raw.albedo;
    hit.emission = raw.emission;
    hit.roughness = raw.roughness;
    hit.metallic = 0.0f;
    hit.transmissive = false;
    hit.absorption = Vec3{0.0f};

    // Not a block, so the direct renderer skips corner AO and the path tracer
    // never mistakes it for the far wall of a medium it is travelling inside.
    hit.isBlock = false;
    hit.blockId = block::Air;
}

void fillFromEntity(const EntityHit& raw, SceneHit& hit) {
    hit.t = raw.t;
    hit.position = raw.position;
    hit.normal = raw.normal;

    // Skin, cloth and leather: matte, never metallic, never transmissive.
    hit.albedo = raw.albedo;
    hit.emission = Vec3{0.0f};
    hit.roughness = 1.0f;
    hit.metallic = 0.0f;
    hit.transmissive = false;
    hit.absorption = Vec3{0.0f};

    hit.isBlock = false;
    hit.blockId = block::Air;
}

// The scene's material style, applied at the one point every kind of geometry
// has already been reduced to a material. Doing it here rather than in each
// fillFrom keeps blocks, entities, sprites and props impossible to style
// inconsistently -- the same reason surfaceAlbedo exists.
//
// Two things are deliberately left alone. A refracting medium keeps its own
// roughness and ior, because a style that turned water plastic would stop it
// being water; and emission is never touched, so a lamp is still a lamp.
void applyStyle(const Scene& scene, SceneHit& hit) {
    if (scene.materialStyle.identity() || hit.transmissive) return;

    StyledMaterial m = scene.materialStyle.apply(hit.albedo, hit.roughness, hit.metallic);
    hit.albedo = m.albedo;
    hit.roughness = m.roughness;
    hit.metallic = m.metallic;
    hit.coat = m.coat;
    hit.coatRoughness = m.coatRoughness;
}

} // namespace

bool intersectScene(const Scene& scene, const Ray& ray, float maxDistance, RayFilter filter,
                    SceneHit& hit) {
    RayHit blockHit;
    bool hitBlock = raycast(scene.world, ray, maxDistance, blockHit, filter);

    // Neither entities nor sprites can be skipped when the ray is inside a
    // medium: a character standing in the shallows is still there, and so is
    // a bubble hanging in the water above them.
    float nearest = hitBlock ? blockHit.t : maxDistance;

    EntityHit entityHit;
    bool hitEntity = scene.entities && scene.entities->intersect(ray, nearest, entityHit);
    if (hitEntity) nearest = entityHit.t;

    SpriteHit spriteHit;
    bool hitSprite = scene.sprites && scene.sprites->intersect(ray, nearest, spriteHit);
    if (hitSprite) nearest = spriteHit.t;

    PropHit propHit;
    bool hitProp = scene.props && scene.props->intersect(ray, nearest, propHit);

    if (hitProp) {
        fillFromProp(propHit, hit);
    } else if (hitSprite) {
        fillFromSprite(spriteHit, hit);
    } else if (hitEntity) {
        fillFromEntity(entityHit, hit);
    } else if (hitBlock) {
        fillFromBlock(scene, blockHit, hit);
    } else {
        return false;
    }

    applyStyle(scene, hit);
    return true;
}

Vec3 sceneTransmittance(const Scene& scene, const Ray& ray, float maxDistance) {
    if (scene.entities && scene.entities->occluded(ray, maxDistance)) return Vec3{0.0f};
    if (scene.sprites && scene.sprites->occluded(ray, maxDistance)) return Vec3{0.0f};
    if (scene.props && scene.props->occluded(ray, maxDistance)) return Vec3{0.0f};
    return rayTransmittance(scene.world, ray, maxDistance);
}

} // namespace blocky
