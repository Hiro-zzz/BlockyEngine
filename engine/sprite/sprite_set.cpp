#include "engine/sprite/sprite_set.hpp"

namespace blocky {
namespace {

// Matches the entity boxes: far enough out to clear float error at block
// scale, near enough to stay invisible.
constexpr float kMinDistance = 1e-4f;

}  // namespace

void SpriteSet::clear() {
    flats_.clear();
    sprites_.clear();
    boxes_.clear();
    bvh_.clear();
    hasBounds_ = false;
}

void SpriteSet::add(const Sprite& sprite) {
    // Yaw, then pitch about the turned +X, then roll about the quad normal.
    // Rigid by construction -- size lives in `half`, never in the matrix, so
    // a ray parameter in local space is already a ray parameter in world space.
    Mat4 rotation = rotateAxis({0.0f, 1.0f, 0.0f}, radians(sprite.yawDegrees)) *
                    rotateAxis({1.0f, 0.0f, 0.0f}, radians(sprite.pitchDegrees)) *
                    rotateAxis({0.0f, 0.0f, 1.0f}, radians(sprite.rollDegrees));
    Mat4 toWorld = translate(sprite.position) * rotation;

    Flat flat;
    flat.toWorld = toWorld;
    flat.toLocal = inverseAffine(toWorld);
    flat.half = {sprite.size.x * 0.5f, sprite.size.y * 0.5f};

    Aabb box;
    for (int corner = 0; corner < 4; ++corner) {
        float x = (corner & 1) ? flat.half.x : -flat.half.x;
        float y = (corner & 2) ? flat.half.y : -flat.half.y;
        box.expand(transformPoint(toWorld, {x, y, 0.0f}));
    }

    // A quad is flat by definition, and a zero-thickness slab is brittle in
    // exactly the case where the ray runs along it. A hair of padding costs
    // nothing and removes the whole class of edge case.
    const Vec3 pad{1e-4f};
    box.lo -= pad;
    box.hi += pad;

    flats_.push_back(flat);
    sprites_.push_back(sprite);
    boxes_.push_back(box);

    if (!hasBounds_) {
        boundsLo_ = box.lo;
        boundsHi_ = box.hi;
        hasBounds_ = true;
    } else {
        boundsLo_ = minv(boundsLo_, box.lo);
        boundsHi_ = maxv(boundsHi_, box.hi);
    }

    // Anything already built no longer describes the set.
    bvh_.clear();
}

void SpriteSet::add(const std::vector<Sprite>& sprites) {
    for (const Sprite& sprite : sprites) add(sprite);
}

void SpriteSet::build() {
    bvh_.build(boxes_);
}

bool SpriteSet::hitQuad(uint32_t index, const Ray& ray, float tLimit, float& tOut,
                        Vec3& normalOut, Vec3& albedoOut) const {
    const Flat& flat = flats_[index];
    const Sprite& sprite = sprites_[index];

    Vec3 origin = transformPoint(flat.toLocal, ray.origin);
    Vec3 direction = transformDir(flat.toLocal, ray.direction);

    // Running parallel to the plane: there is no crossing to find.
    if (std::fabs(direction.z) < 1e-9f) return false;

    bool fromBehind = direction.z > 0.0f;
    if (fromBehind && !sprite.doubleSided) return false;

    float t = -origin.z / direction.z;
    if (t <= kMinDistance || t >= tLimit) return false;

    float x = origin.x + direction.x * t;
    float y = origin.y + direction.y * t;
    if (std::fabs(x) > flat.half.x || std::fabs(y) > flat.half.y) return false;

    // Local extent -> [0,1] with v running downwards, matching image layout,
    // then into whatever sub-rectangle of the atlas this sprite owns.
    float u = flat.half.x > 0.0f ? (x + flat.half.x) / (2.0f * flat.half.x) : 0.0f;
    float v = flat.half.y > 0.0f ? 1.0f - (y + flat.half.y) / (2.0f * flat.half.y) : 0.0f;

    Vec2 uv{lerp(sprite.uvMin.x, sprite.uvMax.x, saturate(u)),
            lerp(sprite.uvMin.y, sprite.uvMax.y, saturate(v))};

    Vec3 albedo = sprite.tint;
    if (sprite.texture && !sprite.texture->empty()) {
        // The cutout is what makes a glyph a glyph instead of the rectangle
        // it is drawn on: a rejected texel lets the ray carry on untouched.
        if (sprite.texture->sampleAlpha(uv) < sprite.alphaCutoff) return false;
        albedo = albedo * sprite.texture->sample(uv);
    }

    Vec3 normal = normalize(transformDir(flat.toWorld, Vec3{0.0f, 0.0f, 1.0f}));

    tOut = t;
    normalOut = fromBehind ? -normal : normal;
    albedoOut = albedo;
    return true;
}

bool SpriteSet::intersect(const Ray& ray, float maxDistance, SpriteHit& hit) const {
    if (bvh_.empty()) return false;

    Vec3 invDirection = inverseDirection(ray.direction);

    bool found = false;
    bvh_.traverse(ray.origin, invDirection, maxDistance, [&](uint32_t index, float& tMax) {
        float t = 0.0f;
        Vec3 normal, albedo;
        if (!hitQuad(index, ray, tMax, t, normal, albedo)) return false;

        tMax = t;
        found = true;
        hit.t = t;
        hit.normal = normal;
        hit.albedo = albedo;
        hit.emission = sprites_[index].emission;
        hit.roughness = sprites_[index].roughness;
        hit.spriteIndex = int(index);
        return true;
    });

    if (found) hit.position = ray.origin + ray.direction * hit.t;
    return found;
}

bool SpriteSet::occluded(const Ray& ray, float maxDistance) const {
    if (bvh_.empty()) return false;

    Vec3 invDirection = inverseDirection(ray.direction);

    return bvh_.traverse<true>(ray.origin, invDirection, maxDistance,
                               [&](uint32_t index, float& tMax) {
                                   float t = 0.0f;
                                   Vec3 normal, albedo;
                                   return hitQuad(index, ray, tMax, t, normal, albedo);
                               });
}

bool SpriteSet::bounds(Vec3& lo, Vec3& hi) const {
    if (!hasBounds_) return false;
    lo = boundsLo_;
    hi = boundsHi_;
    return true;
}

} // namespace blocky
