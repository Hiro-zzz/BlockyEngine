#include "engine/prop/prop_rig.hpp"

namespace blocky {

int PropRig::addPart(const std::string& name, int parent, Vec3 pivot, const VoxelModel* model,
                     Vec3 offset) {
    int joint = skeleton.add(name, parent, pivot);
    if (joint < 0) return -1;
    if (model) attach(joint, model, offset);
    return joint;
}

void PropRig::attach(int joint, const VoxelModel* model, Vec3 offset) {
    if (!model || joint < 0) return;
    PropRigPart part;
    part.model = model;
    part.joint = joint;
    part.offset = offset;
    parts.push_back(part);
}

namespace {

Mat4 placementMatrix(const PropPlacement& placement) {
    Mat4 rotation = rotateAxis({0.0f, 1.0f, 0.0f}, radians(placement.yawDegrees)) *
                    rotateAxis({1.0f, 0.0f, 0.0f}, radians(placement.pitchDegrees)) *
                    rotateAxis({0.0f, 0.0f, 1.0f}, radians(placement.rollDegrees));
    return translate(placement.position) * rotation * scale(Vec3{placement.voxelSize});
}

}  // namespace

bool rigBounds(const PropRig& rig, const Pose& pose, Vec3& lo, Vec3& hi) {
    std::vector<Mat4> jointToRig;
    rig.skeleton.resolve(pose, jointToRig);

    bool any = false;
    for (const PropRigPart& part : rig.parts) {
        if (!part.model || part.model->empty()) continue;

        Mat4 jointMatrix = (part.joint >= 0 && size_t(part.joint) < jointToRig.size())
                               ? jointToRig[size_t(part.joint)]
                               : Mat4::identity();
        Mat4 toRig = jointMatrix * translate(part.offset);

        IVec3 dims = part.model->dims();
        for (int corner = 0; corner < 8; ++corner) {
            Vec3 v{(corner & 1) ? float(dims.x) : 0.0f, (corner & 2) ? float(dims.y) : 0.0f,
                   (corner & 4) ? float(dims.z) : 0.0f};
            Vec3 point = transformPoint(toRig, v);
            if (!any) {
                lo = hi = point;
                any = true;
            } else {
                lo = minv(lo, point);
                hi = maxv(hi, point);
            }
        }
    }
    return any;
}

void addRigged(PropSet& set, const PropRig& rig, const Pose& pose,
               const PropPlacement& placement) {
    if (rig.parts.empty() || placement.voxelSize <= 0.0f) return;

    // One resolve for the whole rig, not one per part.
    std::vector<Mat4> jointToRig;
    rig.skeleton.resolve(pose, jointToRig);

    Vec3 recentre{0.0f, 0.0f, 0.0f};
    if (placement.centreOnBounds) {
        Vec3 lo, hi;
        if (rigBounds(rig, pose, lo, hi)) recentre = -(lo + hi) * 0.5f;
    }

    const Mat4 toWorld = placementMatrix(placement);

    for (const PropRigPart& part : rig.parts) {
        if (!part.model || part.model->empty()) continue;

        Mat4 jointMatrix = (part.joint >= 0 && size_t(part.joint) < jointToRig.size())
                               ? jointToRig[size_t(part.joint)]
                               : Mat4::identity();

        // World <- placement <- joint chain <- this part's own offset. The
        // scale sits in the placement and is uniform, so the composed matrix
        // is still rigid-plus-uniform and the ray parameter survives it.
        Mat4 partToWorld = toWorld * translate(recentre) * jointMatrix * translate(part.offset);

        set.addTransformed(part.model, partToWorld, part.tint * placement.tint,
                           part.emissionScale * placement.emissionScale);
    }
}

} // namespace blocky
