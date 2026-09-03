#pragma once
// Voxel elements hung off an entity's joints: a tool in a fist, a lantern on
// a belt, a horn bolted to a helmet.
//
// Both ends of this existed and did not meet. `rigging::attach` hangs a new
// *box* off a joint, but a box wears the skin -- it is more character, not a
// separate object. `PropRig` moves whole `VoxelModel`s, but along its own
// skeleton rather than a character's.
//
// So scenes bridged it by hand, and two of them did it the same way and by
// copy: rebuild the entity's placement matrix, resolve the skeleton, find the
// fist, find the grip voxel, multiply the five together. That is a second
// implementation of what `EntitySet::add` does living in scene code -- the
// arrangement this engine refuses everywhere else, because the two agree
// right up until one of them changes and then a hand and the thing in it
// drift apart quietly.
//
// ---------------------------------------------------------------- the units
//
// An attachment sits in its joint's own space, and that space is model
// pixels. One voxel is one model pixel by default, which is not a tidiness
// convention: a Minecraft item is sixteen voxels tall and a block is sixteen
// model pixels, so an item held in a fist comes out the size the game draws
// it. It follows `Entity::scale` for free, because the joint matrix already
// carries it.
//
// ------------------------------------------------------------ where it goes
//
// Into a `PropSet`, like any other prop -- not into the `EntitySet`. By the
// time the tracer sees it, a lantern in a hand is a prop, and `intersectScene`
// keeps its four branches instead of learning that an entity can also be a
// voxel grid. The set must still be `build()`-ed after the last add, and a
// take rebuilds both every frame, exactly as it already does for props.
#include "engine/entity/entity.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxel_model.hpp"

#include <vector>

namespace blocky {

struct VoxelAttachment {
    const VoxelModel* model = nullptr;   // must outlive the call
    int   joint = -1;                    // a joint of the entity's own skeleton

    // Where on that joint it sits, in model pixels, in the joint's rest space.
    // `jointTip` finds the end of a limb; the rest is taste.
    Vec3 offset{};

    // Which voxel of the model lands on `offset` -- the grip of a tool rather
    // than the corner of its bounding box. See `gripVoxel`.
    Vec3 anchor{};

    // Applied about the anchor, in the order X, then Y, then Z.
    Vec3 rotationDegrees{};

    // Model pixels per voxel. One is game-sized; the field exists because a
    // prop authored at a different resolution is otherwise unusable here.
    float voxelScale = 1.0f;

    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 emissionScale{1.0f, 1.0f, 1.0f};
};

// The transform taking the attachment's voxel coordinates into world space.
// Rigid plus a uniform scale, which is what `PropSet` requires: a non-uniform
// one would stretch the local ray direction unevenly and `t` would stop
// meaning the same thing on both sides of it.
//
// False when there is no model or no such joint, leaving `out` untouched.
bool attachmentToWorld(const Entity& entity, const VoxelAttachment& attachment, Mat4& out);

// The same, for a caller that already resolved the skeleton and is placing
// several things on one entity.
bool attachmentToWorld(const std::vector<Mat4>& jointToWorld, const VoxelAttachment& attachment,
                       Mat4& out);

// Place one, or a list. An attachment with no model, an empty model or an
// unknown joint is skipped rather than treated as an error: a scene that
// failed to load one item should lose that item, not the character.
bool addAttachment(PropSet& props, const Entity& entity, const VoxelAttachment& attachment);
int  addAttachments(PropSet& props, const Entity& entity,
                    const std::vector<VoxelAttachment>& attachments);

// ------------------------------------------- the two points a hand needs

// The fist: bottom centre of the base-layer boxes carried by `joint`, in
// model pixels and in that joint's own space. A limb hangs downwards from its
// joint, so its far end is the bottom of its lowest box.
//
// A joint carrying no boxes returns the joint itself, which is where a thing
// with no limb to hold it should hang.
Vec3 jointTip(const EntityModel& model, int joint);

// The grip: centroid of the solid voxels in the bottom `fraction` of the
// model. A tool is held near its base, and a handle is rarely in the middle
// of the bounding box -- a pickaxe's is not even close.
//
// An empty model gives the centre of its bounds, so the caller still gets a
// usable point rather than the origin corner.
Vec3 gripVoxel(const VoxelModel& model, float fraction = 0.25f);

} // namespace blocky
