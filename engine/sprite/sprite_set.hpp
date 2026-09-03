#pragma once
// Sprites flattened into world space and indexed for tracing.
//
// Placing a sprite resolves its orientation into a rigid transform once; the
// renderer only ever sees a flat list of quads, exactly as EntitySet only
// ever shows it a flat list of boxes.
//
// Unlike entity boxes these come in thousands, so the list is walked through
// a BVH rather than scanned. That is the only structural difference between
// the two, and the reason engine/core/bvh.hpp is written to not care what it
// is holding: EntitySet can adopt it unchanged when a scene finally has
// enough characters to need it.
//
// > Emissive sprites glow and appear in reflections, but they are not
// > gathered into LightSet -- that scans the voxel world for exposed emissive
// > faces and nothing else. A glowing mote therefore lights only itself.
// > Deliberate: particles decorate a frame, they do not light it, and adding
// > thousands of tiny emitters to the sampling list would cost far more than
// > the light they contribute is worth.
#include "engine/core/bvh.hpp"
#include "engine/sprite/sprite.hpp"
#include "engine/world/raycast.hpp"

#include <vector>

namespace blocky {

struct SpriteHit {
    float t = 0.0f;
    Vec3  position{};
    Vec3  normal{};     // world space, unit, pointing back along the ray
    Vec3  albedo{};     // linear light, texture times tint
    Vec3  emission{};
    float roughness = 1.0f;
    int   spriteIndex = -1;
};

class SpriteSet {
public:
    void clear();

    // Flattens immediately. The texture a sprite points at must outlive the set.
    void add(const Sprite& sprite);
    void add(const std::vector<Sprite>& sprites);

    // Builds the BVH. Call once after the last add; intersect does nothing
    // useful before it.
    void build();

    bool   empty() const { return flats_.empty(); }
    size_t size() const { return flats_.size(); }
    bool   built() const { return !bvh_.empty(); }

    bool intersect(const Ray& ray, float maxDistance, SpriteHit& hit) const;
    bool occluded(const Ray& ray, float maxDistance) const;
    bool bounds(Vec3& lo, Vec3& hi) const;

    const std::vector<Sprite>& sprites() const { return sprites_; }
    const Bvh& bvh() const { return bvh_; }

    struct Flat {
        Mat4 toWorld;   // rigid: rotation and translation only, so ray t is unscaled
        Mat4 toLocal;
        Vec2 half{};    // half extent in the local XY plane
    };

    // The flattened placements, so a shader can be handed the same ones the
    // CPU walks. Flattening them a second time would be a second chance to
    // disagree about a transform, and the disagreement would look like a
    // misplaced object rather than like a bug.
    const std::vector<Flat>& flats() const { return flats_; }

private:

    // Ray against one quad, in its own space. Returns false when the ray
    // misses, or lands on a texel the cutout throws away.
    bool hitQuad(uint32_t index, const Ray& ray, float tLimit, float& tOut, Vec3& normalOut,
                 Vec3& albedoOut) const;

    std::vector<Flat> flats_;
    std::vector<Sprite> sprites_;
    std::vector<Aabb> boxes_;
    Bvh bvh_;

    Vec3 boundsLo_{}, boundsHi_{};
    bool hasBounds_ = false;
};

} // namespace blocky
