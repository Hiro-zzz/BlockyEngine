#pragma once
// A small dense voxel grid with its own palette: the shape of a prop, before
// anything decides where to put it.
//
// This is deliberately not a World. A World is a hash of 16-block chunks whose
// ids index a global registry -- right for terrain, wrong for a sword that is
// sixteen voxels tall and needs a hundred arbitrary colours nobody else shares.
// A prop is small, dense, owns its palette, and is placed by a transform
// rather than by lattice coordinates.
//
// Traversal is a single-level DDA. The two-level walk the world uses exists to
// skip empty chunks; a grid this size has no emptiness worth skipping.
#include "engine/core/math.hpp"

#include <cstdint>
#include <vector>

namespace blocky {

struct VoxelMaterial {
    Vec3  albedo{0.8f, 0.8f, 0.8f};  // linear light, never sRGB
    Vec3  emission{0.0f, 0.0f, 0.0f};
    float roughness = 1.0f;
    float metallic = 0.0f;
};

struct VoxelHit {
    float    t = 0.0f;
    Vec3     position{};   // in voxel units, model-local
    Vec3     normal{};     // model-local, unit, axis aligned
    IVec3    voxel{};
    uint16_t material = 0;
};

class VoxelModel {
public:
    // Index 0 always means empty, so a palette entry is never zero.
    static constexpr uint16_t kEmpty = 0;

    void clear();
    void resize(IVec3 dims);  // clears the contents

    IVec3  dims() const { return dims_; }
    bool   empty() const { return solid_ == 0; }
    size_t solidCount() const { return solid_; }

    bool inside(IVec3 v) const {
        return v.x >= 0 && v.y >= 0 && v.z >= 0 && v.x < dims_.x && v.y < dims_.y && v.z < dims_.z;
    }

    uint16_t at(IVec3 v) const { return inside(v) ? voxels_[index(v)] : kEmpty; }
    void     set(IVec3 v, uint16_t material);

    // Returns the index to store in a voxel. Identical materials are folded
    // together, which matters: an extruded item texture asks for one per
    // texel and most of them are the same colour.
    uint16_t addMaterial(const VoxelMaterial& material);
    const VoxelMaterial& material(uint16_t index) const;
    size_t materialCount() const { return palette_.size() - 1; }

    // Shrink to the occupied box. An item texture is mostly empty, and the
    // untrimmed grid would make every prop's bounds a 16-cube.
    void trim();

    // Occupied box in voxel units, inclusive. Meaningless when empty.
    IVec3 minVoxel() const { return min_; }
    IVec3 maxVoxel() const { return max_; }

    // Ray against the grid, in voxel units. `direction` need not be unit: the
    // caller scales it by the same factor as the space, which is exactly what
    // keeps the ray parameter the same on both sides of the transform.
    bool trace(Vec3 origin, Vec3 direction, float tMin, float tMax, VoxelHit& hit) const;

    // Cheaper: first solid voxel, without surface data.
    bool occluded(Vec3 origin, Vec3 direction, float tMin, float tMax) const;

    // ------------------------------------------------------------- staleness
    // When this model last changed. Anything that keeps something built from
    // it -- the viewport's mesh cache is the one that exists -- remembers the
    // stamp it built at and compares. Same pull-based arrangement as
    // `World::chunkStamp`, and for the same reason: a dirty flag has to know
    // when it may be cleared, which means knowing how many caches there are.
    //
    // The counter is **global to the process**, not per model, because those
    // caches are keyed by address. A per-model counter starting at one would
    // let a freed model and a new one allocated at the same address agree on
    // a stamp they never shared, and a cache holding the old mesh would call
    // itself current.
    uint64_t stamp() const { return stamp_; }

private:
    // Every mutator ends here, so there is exactly one place that can forget.
    void touch();

    size_t index(IVec3 v) const {
        return (size_t(v.y) * size_t(dims_.z) + size_t(v.z)) * size_t(dims_.x) + size_t(v.x);
    }

    IVec3 dims_{0, 0, 0};
    std::vector<uint16_t> voxels_;
    std::vector<VoxelMaterial> palette_{VoxelMaterial{}};  // slot 0 is the empty sentinel
    size_t solid_ = 0;
    IVec3 min_{0, 0, 0}, max_{0, 0, 0};

    // Zero exactly while the model has never been sized or written to, which
    // is also the value a cache that has never built anything holds. The two
    // agreeing is correct: there is nothing to build.
    uint64_t stamp_ = 0;
};

} // namespace blocky
