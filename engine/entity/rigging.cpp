#include "engine/entity/rigging.hpp"

#include <algorithm>

namespace blocky {
namespace rigging {
namespace {

// Cut one rectangle into `segments` along whichever texture direction the
// model axis drives, and hand back piece `index`.
//
// `flip` says the model axis runs backwards across the texture, which is true
// of most faces -- v runs down while +Y runs up. Getting it wrong puts the
// shoulder's half of a sleeve on the forearm, which is exactly the kind of
// mistake that reads as "the skin is a bit off" rather than as a bug.
SkinRect sliceRect(const SkinRect& rect, bool horizontal, bool flip, int index, int segments) {
    if (!rect.valid() || segments <= 1) return rect;

    int piece = flip ? (segments - 1 - index) : index;

    SkinRect out = rect;
    if (horizontal) {
        int x0 = rect.width * piece / segments;
        int x1 = rect.width * (piece + 1) / segments;
        out.x = rect.x + x0;
        out.width = std::max(1, x1 - x0);
    } else {
        int y0 = rect.height * piece / segments;
        int y1 = rect.height * (piece + 1) / segments;
        out.y = rect.y + y0;
        out.height = std::max(1, y1 - y0);
    }
    return out;
}

}  // namespace

SkinRect subRect(const SkinRect& rect, int x, int y, int width, int height) {
    SkinRect out;
    out.x = rect.x + x;
    out.y = rect.y + y;
    out.width = std::min(width, std::max(0, rect.width - x));
    out.height = std::min(height, std::max(0, rect.height - y));
    return out;
}

std::vector<int> split(EntityModel& model, int joint, int axis, int segments, Chain chain,
                       const std::string& prefix) {
    std::vector<int> created;
    if (joint < 0 || joint >= int(model.skeleton.size())) return created;
    if (axis < 0 || axis > 2 || segments < 2) return created;

    std::vector<size_t> indices = model.boxesOn(joint);
    if (indices.empty()) return created;

    // The span to cut is the union of the boxes on this joint, so a base
    // layer and the shell around it are cut at the same places.
    float low = model.boxes[indices[0]].origin[axis];
    float high = low + model.boxes[indices[0]].size[axis];
    for (size_t i : indices) {
        low = std::min(low, model.boxes[i].origin[axis]);
        high = std::max(high, model.boxes[i].origin[axis] + model.boxes[i].size[axis]);
    }
    if (high - low <= 0.0f) return created;

    const Vec3 parentPivot = model.skeleton[joint].pivot;
    const float step = (high - low) / float(segments);

    // ------------------------------------------------------------- joints
    // Each segment's pivot sits on the boundary it shares with its parent,
    // keeping the parent joint's other two coordinates.
    created.assign(size_t(segments), -1);

    auto pivotAt = [&](float along) {
        Vec3 pivot = parentPivot;
        pivot[axis] = along;
        return pivot;
    };

    if (chain == Chain::Siblings) {
        for (int k = 0; k < segments; ++k) {
            created[size_t(k)] =
                model.skeleton.add(prefix + std::to_string(k), joint, pivotAt(low + step * float(k)));
        }
    } else if (chain == Chain::FromLow) {
        int parent = joint;
        for (int k = 0; k < segments; ++k) {
            // Segment 0 keeps the original pivot; the rest hinge on the
            // boundary they share with the segment below.
            Vec3 pivot = k == 0 ? parentPivot : pivotAt(low + step * float(k));
            parent = model.skeleton.add(prefix + std::to_string(k), parent, pivot);
            created[size_t(k)] = parent;
        }
    } else {  // FromHigh -- what a limb wants
        int parent = joint;
        for (int k = segments - 1; k >= 0; --k) {
            Vec3 pivot = k == segments - 1 ? parentPivot : pivotAt(low + step * float(k + 1));
            parent = model.skeleton.add(prefix + std::to_string(k), parent, pivot);
            created[size_t(k)] = parent;
        }
    }

    // -------------------------------------------------------------- boxes
    std::vector<ModelBox> pieces;
    pieces.reserve(indices.size() * size_t(segments));

    for (size_t i : indices) {
        const ModelBox& source = model.boxes[i];

        for (int k = 0; k < segments; ++k) {
            ModelBox piece = source;
            piece.joint = created[size_t(k)];
            piece.origin[axis] = low + step * float(k);
            piece.size[axis] = step;

            for (int f = 0; f < SkinFaceCount; ++f) {
                const SkinRect& rect = source.faces[f];
                if (!rect.valid()) continue;

                FaceAxes fa = faceAxes(SkinFace(f));
                if (axis == fa.uAxis) {
                    piece.faces[f] = sliceRect(rect, true, fa.uFlip, k, segments);
                } else if (axis == fa.vAxis) {
                    piece.faces[f] = sliceRect(rect, false, fa.vFlip, k, segments);
                }
                // Otherwise this face looks along the split axis: keep the
                // whole rectangle, so the cut reads as an end cap.
            }
            pieces.push_back(piece);
        }
    }

    // Drop the originals, keeping everything else in order.
    std::vector<ModelBox> kept;
    kept.reserve(model.boxes.size() + pieces.size());
    for (size_t i = 0; i < model.boxes.size(); ++i) {
        if (model.boxes[i].joint == joint) continue;
        kept.push_back(model.boxes[i]);
    }
    for (ModelBox& piece : pieces) kept.push_back(std::move(piece));
    model.boxes = std::move(kept);

    return created;
}

int addHinge(EntityModel& model, int joint, const std::string& name) {
    // Limbs run along Y and hang from the top, so the near half is the upper
    // one and the far half is what bends.
    std::vector<int> parts = split(model, joint, 1, 2, Chain::FromHigh, name);
    return parts.empty() ? -1 : parts[0];
}

int addJaw(EntityModel& model, int headJoint, float jawPixels, const std::string& name) {
    if (headJoint < 0 || headJoint >= int(model.skeleton.size())) return -1;

    std::vector<size_t> indices = model.boxesOn(headJoint);
    if (indices.empty()) return -1;

    float low = model.boxes[indices[0]].origin.y;
    float high = low + model.boxes[indices[0]].size.y;
    for (size_t i : indices) {
        low = std::min(low, model.boxes[i].origin.y);
        high = std::max(high, model.boxes[i].origin.y + model.boxes[i].size.y);
    }

    float height = high - low;
    if (jawPixels <= 0.0f || jawPixels >= height) return -1;

    // A plain two-way split would cut in the middle. A jaw is a slice off the
    // bottom, so pick the number of even segments that puts a boundary where
    // the jaw ends -- then keep the bottom one and merge the rest.
    int segments = int(std::lround(height / jawPixels));
    if (segments < 2) segments = 2;

    std::vector<int> parts = split(model, headJoint, 1, segments, Chain::FromHigh, name + "_seg");
    if (parts.size() < 2) return -1;

    // Everything above the jaw belongs to the skull, which does not move
    // relative to the head: fold those segments back onto the head joint.
    int jawJoint = parts[0];
    for (size_t k = 1; k < parts.size(); ++k) {
        for (ModelBox& box : model.boxes) {
            if (box.joint == parts[k]) box.joint = headJoint;
        }
    }

    // Re-hang the jaw straight off the head, and put its hinge at the back of
    // the skull rather than in the middle of it -- that is where a jaw turns.
    Joint& hinge = model.skeleton.mutableJoint(jawJoint);
    hinge.parent = headJoint;
    hinge.name = name;
    hinge.pivot.y = low + jawPixels;
    hinge.pivot.z = 4.0f;  // back of a standard 8-deep head

    return jawJoint;
}

Attachment attachmentFrom(const std::string& name, int parent, Vec3 origin, Vec3 size,
                          const SkinRect& rect) {
    Attachment attachment;
    attachment.name = name;
    attachment.parent = parent;
    attachment.origin = origin;
    attachment.size = size;
    attachment.pivot = origin;
    for (int f = 0; f < SkinFaceCount; ++f) attachment.faces[f] = rect;
    return attachment;
}

int attach(EntityModel& model, const Attachment& attachment) {
    if (attachment.parent < -1 || attachment.parent >= int(model.skeleton.size())) return -1;
    if (attachment.size.x <= 0.0f || attachment.size.y <= 0.0f || attachment.size.z <= 0.0f) {
        return -1;
    }

    int joint = model.skeleton.add(attachment.name, attachment.parent, attachment.pivot);
    if (joint < 0) return -1;

    ModelBox box;
    box.part = attachment.part;
    box.layer = attachment.layer;
    box.joint = joint;
    box.origin = attachment.origin;
    box.size = attachment.size;
    box.inflate = attachment.inflate;
    box.cutout = attachment.cutout;
    for (int f = 0; f < SkinFaceCount; ++f) box.faces[f] = attachment.faces[f];
    model.boxes.push_back(box);

    return joint;
}

int detachLayer(EntityModel& model, SkinPart part, SkinLayer layer, const std::string& name) {
    int currentJoint = -1;
    for (const ModelBox& box : model.boxes) {
        if (box.part != part || box.layer != layer) continue;
        currentJoint = box.joint;
        break;
    }
    if (currentJoint < 0) return -1;

    // The new joint shares its parent's pivot, so detaching alone changes
    // nothing about where the layer sits -- only about what moves it.
    int joint = model.skeleton.add(name, currentJoint, model.skeleton[currentJoint].pivot);
    if (joint < 0) return -1;

    for (ModelBox& box : model.boxes) {
        if (box.part == part && box.layer == layer) box.joint = joint;
    }
    return joint;
}

bool reparent(EntityModel& model, int joint, int newParent) {
    const int count = int(model.skeleton.size());
    if (joint <= 0 || joint >= count) return false;          // never the root
    if (newParent < -1 || newParent >= count) return false;
    if (newParent >= joint) return false;                    // parents come first
    if (model.skeleton.descendsFrom(newParent, joint)) return false;  // no cycles

    model.skeleton.mutableJoint(joint).parent = newParent;
    return true;
}

} // namespace rigging
} // namespace blocky
