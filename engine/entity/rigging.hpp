#pragma once
// Editing a rig after the model is built: cutting parts into segments,
// hanging new boxes off joints, and giving a layer a joint of its own.
//
// This is where the fine detail lives. The skeleton in rig.hpp gives
// parenting; these give it something to parent. An arm becomes an upper arm
// and a forearm with an elbow between them, a head becomes a skull and a
// hinged jaw, a hat stops being welded to the scalp.
//
// The painstaking part is the skin. A box knows six rectangles of the skin
// image, and cutting the box in half means cutting those rectangles in half
// the same way -- along the right texture axis, in the right direction, per
// face. Get it wrong and a forearm wears the shoulder's half of its sleeve.
// faceAxes() in model.hpp is the inverse of boxFaceUv() and is what makes the
// two agree.
#include "engine/entity/model.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace rigging {

// Which end of a split becomes the root of the chain.
enum class Chain {
    // Each segment is the parent of the next one along the axis, starting at
    // the low end. Rotating segment k carries every segment above it.
    FromLow,
    // The same from the high end. This is what a limb wants: an arm hangs
    // from a shoulder at its top, so the upper segment is the root and the
    // forearm hinges off it.
    FromHigh,
    // No chain -- every segment hangs directly off the original joint.
    Siblings,
};

// Cut every box on `joint` into `segments` pieces along `axis` (0=X, 1=Y,
// 2=Z), giving each piece a new joint. Returns the new joints ordered along
// the axis, index 0 being the low end. The original joint keeps no boxes.
//
// Skin rectangles are cut to match. The two faces looking along the split
// axis are a special case: every segment keeps the original rectangle for
// them, so an interior cut reads as the limb's end cap rather than as a hole.
// (Marking them invalid would be truer to a solid limb and much worse to
// look at -- an invalid face lets the ray pass straight through the box.)
//
// A rectangle whose size does not divide evenly by `segments` splits as
// evenly as integers allow; pieces can differ by a pixel.
std::vector<int> split(EntityModel& model, int joint, int axis, int segments, Chain chain,
                       const std::string& prefix);

// An arm or a leg gains an elbow or a knee: the limb is cut in half across
// its length and the far half hinges off the near one. Returns the new joint
// for the far half -- the one you rotate to bend it.
int addHinge(EntityModel& model, int joint, const std::string& name);

// A head gains a jaw: the lower `jawPixels` of it hinge off the skull.
// Returns the jaw joint. The hinge sits at the back of the head, where a jaw
// actually pivots, rather than in the middle where a plain split would put it.
int addJaw(EntityModel& model, int headJoint, float jawPixels = 3.0f,
           const std::string& name = "jaw");

// A new box on a new joint, hanging off `parent`.
struct Attachment {
    std::string name;
    int   parent = -1;
    Vec3  pivot{};    // model pixels; where this joint turns
    Vec3  origin{};   // min corner of the box
    Vec3  size{};     // extent
    float inflate = 0.0f;
    bool  cutout = false;

    // Reported back by EntityHit, so a ray can say what it struck.
    SkinPart  part  = PartHead;
    SkinLayer layer = LayerBase;

    // Where each face samples the skin. Leave a face invalid to omit it --
    // but note that an invalid face lets rays through, so a box wants all six
    // unless it is deliberately open.
    SkinRect faces[SkinFaceCount];
};
int attach(EntityModel& model, const Attachment& attachment);

// Every face of the attachment samples the same rectangle. Enough for a small
// feature cut from a flat patch of skin -- a brow, an ear, a stud.
Attachment attachmentFrom(const std::string& name, int parent, Vec3 origin, Vec3 size,
                          const SkinRect& rect);

// A sub-rectangle of an existing one, in pixels from its top-left. This is
// how a feature borrows a few texels of the face it sits on.
SkinRect subRect(const SkinRect& rect, int x, int y, int width, int height);

// Move every box of `part`/`layer` onto a new joint, child of whatever joint
// they are on now. That is what lets a hat tilt without the head following,
// or a jacket hang while the torso turns. Returns the new joint, or -1 if
// there were no such boxes.
int detachLayer(EntityModel& model, SkinPart part, SkinLayer layer, const std::string& name);

// Re-parent a joint. Refuses to make a cycle, and refuses to move a joint
// above its own parent in the list -- the parents-first order is what keeps
// resolving a single forward pass.
bool reparent(EntityModel& model, int joint, int newParent);

} // namespace rigging
} // namespace blocky
