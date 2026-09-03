#pragma once
// Voxel models placed in the world: position, orientation, and how big one
// voxel is.
//
// A prop is not part of the voxel grid and deliberately so. Stamping it into
// the World would lock it to the lattice -- no free rotation, and nothing
// smaller than a block, which rules out the entire point of an item that is
// sixteen voxels tall and one block high.
//
// The set flattens each placement into a rigid-plus-uniform-scale transform
// once, and indexes them with the same BVH the sprites use. That was the
// reason to write it not knowing what it holds.
#include "engine/core/bvh.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/world/raycast.hpp"

#include <vector>

namespace blocky {

struct Prop {
    // Where `position` sits on the model.
    enum class Anchor {
        BottomCentre,  // standing on the ground: the usual one
        Centre,        // spinning in mid-air
        Min,           // the model's own origin corner
    };

    const VoxelModel* model = nullptr;  // must outlive the set

    Vec3 position{};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float rollDegrees = 0.0f;

    // World units per voxel. A sixteen-voxel item at 1/16 stands one block
    // tall, which is how Minecraft sizes its own item models.
    float voxelSize = 1.0f / 16.0f;

    Anchor anchor = Anchor::BottomCentre;

    // Multiply through the model's palette, so one model serves as several
    // props without a copy per colour.
    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 emissionScale{1.0f, 1.0f, 1.0f};
};

struct PropHit {
    float t = 0.0f;
    Vec3  position{};
    Vec3  normal{};   // world space, unit, pointing back along the ray
    Vec3  albedo{};
    Vec3  emission{};
    float roughness = 1.0f;
    float metallic = 0.0f;
    int   propIndex = -1;
    IVec3 voxel{};
};

class PropSet {
public:
    void clear();

    // Flattens immediately. A prop with no model, or an empty one, is ignored.
    void add(const Prop& prop);
    void add(const std::vector<Prop>& props);

    // Place a model by an explicit transform mapping voxel coordinates to
    // world space. It must be rigid plus a uniform scale, for the reason in
    // add() above. This is what a rig uses, since a posed part's transform
    // comes out of the joint chain rather than out of three angles.
    //
    // The Prop recorded in props() carries the model and the tints; its
    // position and angles are not meaningful for parts placed this way.
    void addTransformed(const VoxelModel* model, const Mat4& toWorld,
                        Vec3 tint = {1.0f, 1.0f, 1.0f},
                        Vec3 emissionScale = {1.0f, 1.0f, 1.0f});

    // Builds the BVH. Call once after the last add; the set is inert before it.
    void build();

    bool   empty() const { return flats_.empty(); }
    size_t size() const { return flats_.size(); }
    bool   built() const { return !bvh_.empty(); }

    // Total solid voxels placed, which is the honest measure of how heavy the
    // set is -- a hundred props sharing one model cost one model's memory.
    uint64_t voxelCount() const { return voxels_; }

    bool intersect(const Ray& ray, float maxDistance, PropHit& hit) const;
    bool occluded(const Ray& ray, float maxDistance) const;
    bool bounds(Vec3& lo, Vec3& hi) const;

    const std::vector<Prop>& props() const { return props_; }
    const Bvh& bvh() const { return bvh_; }

    struct Flat {
        Mat4 toWorld;  // rotation, uniform scale and translation
        Mat4 toLocal;
        const VoxelModel* model = nullptr;
        Vec3 tint{1.0f, 1.0f, 1.0f};
        Vec3 emissionScale{1.0f, 1.0f, 1.0f};
    };

    // The flattened placements, so a shader can be handed the same ones the
    // CPU walks. Flattening them a second time would be a second chance to
    // disagree about a transform, and the disagreement would look like a
    // misplaced object rather than like a bug.
    const std::vector<Flat>& flats() const { return flats_; }

private:
    // The one path both add() and addTransformed() funnel through.
    void place(const VoxelModel* model, const Mat4& toWorld, Vec3 tint, Vec3 emissionScale,
               const Prop* source);

    std::vector<Flat> flats_;
    std::vector<Prop> props_;
    std::vector<Aabb> boxes_;
    Bvh bvh_;
    uint64_t voxels_ = 0;

    Vec3 boundsLo_{}, boundsHi_{};
    bool hasBounds_ = false;
};

} // namespace blocky
