#pragma once
// A rig for props: several voxel models hung off one skeleton.
//
// The same idea as an entity's rig, one level coarser. An entity binds boxes
// to joints; a prop rig binds whole models. That is the natural grain for a
// thing built out of separate pieces -- a chest with a lid, a lamp on a
// bracket, a crane with a jib -- where each piece is already its own grid.
//
// The skeleton is the one from engine/rig/rig.hpp, unchanged. It never knew
// what it was holding, which is exactly why it can hold this.
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/rig/rig.hpp"

#include <string>
#include <vector>

namespace blocky {

struct PropRigPart {
    const VoxelModel* model = nullptr;  // must outlive the rig
    int  joint = -1;
    Vec3 offset{};  // voxel units, within the joint's own space
    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 emissionScale{1.0f, 1.0f, 1.0f};
};

struct PropRig {
    Skeleton skeleton;
    std::vector<PropRigPart> parts;

    // Convenience: add a joint and hang a model off it in one call.
    int addPart(const std::string& name, int parent, Vec3 pivot, const VoxelModel* model,
                Vec3 offset = {});

    // Hang another model off a joint that already exists.
    void attach(int joint, const VoxelModel* model, Vec3 offset = {});
};

// Where and how big a whole rigged prop stands.
struct PropPlacement {
    Vec3  position{};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float rollDegrees = 0.0f;
    float voxelSize = 1.0f / 16.0f;

    // Where `position` sits: the rig's own origin, or the middle of its
    // resting bounds.
    bool centreOnBounds = false;

    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 emissionScale{1.0f, 1.0f, 1.0f};
};

// Resolve the pose and emit one placement per part. Every part lands in the
// same PropSet as any other prop, so a rigged thing is indistinguishable from
// a loose one by the time the tracer sees it.
void addRigged(PropSet& set, const PropRig& rig, const Pose& pose,
               const PropPlacement& placement);

// Bounds of the rig at rest, in voxel units. Useful for choosing a voxelSize
// or for centring.
bool rigBounds(const PropRig& rig, const Pose& pose, Vec3& lo, Vec3& hi);

} // namespace blocky
