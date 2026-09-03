#pragma once
// Collision geometry for a voxel model: a handful of boxes instead of a grid.
//
// A rigid body needs a shape it can be asked about thousands of times a
// second, and a dense voxel grid is the wrong answer -- testing a tumbling
// model cell by cell against the world is quadratic in the thing that made
// voxels attractive in the first place.
//
// Source models carry collision hulls an artist made, produced by convex
// decomposition, which is a genuinely hard geometry problem. Voxels hand us
// an easier one: the solid set is already a union of axis-aligned unit cubes,
// so merging greedily into larger boxes is exact rather than approximate. A
// sixteen-voxel item usually falls to a few dozen boxes, and a slab or a plank
// to one.
//
// Two invariants make this testable without eyeballing anything, and
// `test_physics` checks both: the boxes **cover every solid voxel** and they
// **never overlap**. Together those say the decomposition is a partition, so
// summing volume and inertia over boxes gives the same answer as summing over
// voxels -- which is the only reason the mass properties below are allowed to
// ignore the grid entirely.
#include "engine/core/math.hpp"
#include "engine/prop/voxel_model.hpp"

#include <vector>

namespace blocky {

// One box of the decomposition, in the body's local frame: world units, and
// positioned relative to the centre of mass rather than the model's corner.
// Bodies rotate about their centre of mass, so anything else would need the
// offset re-applied at every use.
struct ColliderBox {
    Vec3 center{};
    Vec3 halfExtents{};
};

struct Collider {
    std::vector<ColliderBox> boxes;

    // The centre of mass in the model's own **voxel** coordinates. Kept
    // because the transform handed to `PropSet::addTransformed` maps voxel
    // space to the world, so the renderer needs the offset the physics
    // removed. See `bodyToWorld` in body.hpp -- the one place that knows it.
    Vec3 centreOfMassVoxel{};

    // Local-space bounds around the centre of mass, and the radius of a
    // sphere enclosing them. The radius is what the broad phase asks for.
    Vec3  boundsMin{};
    Vec3  boundsMax{};
    float radius = 0.0f;

    float volume = 0.0f;   // world units cubed
    bool  empty() const { return boxes.empty(); }
};

// Greedy merge of the model's solid voxels into boxes. `voxelSize` is world
// units per voxel -- 1/16 for a Minecraft-sized item, matching `Prop`.
Collider buildCollider(const VoxelModel& model, float voxelSize);

// Total mass at a uniform density, and the inertia tensor about the centre of
// mass in body axes. Uniform density is a real assumption and a visible one:
// a hollow lantern tumbles like a solid block of the same silhouette. Fixing
// it means weighting by voxel, which the decomposition would then have to stop
// merging across.
Mat3 inertiaTensor(const Collider& collider, float density, float* massOut);

} // namespace blocky
