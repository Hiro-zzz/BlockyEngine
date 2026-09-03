#include "engine/entity/attach.hpp"

#include <algorithm>
#include <limits>

namespace blocky {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

}  // namespace

bool attachmentToWorld(const std::vector<Mat4>& jointToWorld, const VoxelAttachment& attachment,
                       Mat4& out) {
    if (!attachment.model || attachment.model->empty()) return false;
    if (attachment.joint < 0 || size_t(attachment.joint) >= jointToWorld.size()) return false;
    if (attachment.voxelScale <= 0.0f) return false;

    // Read right to left: put the anchor voxel at the origin, size the model,
    // turn it, carry it out to the offset, and hand the lot to the joint --
    // which already carries the pose, the yaw, the pixel-to-block scale and
    // the entity's position.
    out = jointToWorld[size_t(attachment.joint)] * translate(attachment.offset) *
          rotateAxis({1.0f, 0.0f, 0.0f}, radians(attachment.rotationDegrees.x)) *
          rotateAxis({0.0f, 1.0f, 0.0f}, radians(attachment.rotationDegrees.y)) *
          rotateAxis({0.0f, 0.0f, 1.0f}, radians(attachment.rotationDegrees.z)) *
          scale(Vec3{attachment.voxelScale}) * translate(-attachment.anchor);
    return true;
}

bool attachmentToWorld(const Entity& entity, const VoxelAttachment& attachment, Mat4& out) {
    std::vector<Mat4> jointToWorld;
    resolveEntityJoints(entity, jointToWorld);
    return attachmentToWorld(jointToWorld, attachment, out);
}

bool addAttachment(PropSet& props, const Entity& entity, const VoxelAttachment& attachment) {
    Mat4 toWorld;
    if (!attachmentToWorld(entity, attachment, toWorld)) return false;

    props.addTransformed(attachment.model, toWorld, attachment.tint, attachment.emissionScale);
    return true;
}

int addAttachments(PropSet& props, const Entity& entity,
                   const std::vector<VoxelAttachment>& attachments) {
    // Resolved once for the whole list rather than once per item: the chain is
    // seven joints and the list is usually one, but a scene hanging a dozen
    // things off a figure should not pay for the skeleton a dozen times.
    std::vector<Mat4> jointToWorld;
    resolveEntityJoints(entity, jointToWorld);

    int placed = 0;
    for (const VoxelAttachment& attachment : attachments) {
        Mat4 toWorld;
        if (!attachmentToWorld(jointToWorld, attachment, toWorld)) continue;

        props.addTransformed(attachment.model, toWorld, attachment.tint,
                             attachment.emissionScale);
        ++placed;
    }
    return placed;
}

Vec3 jointTip(const EntityModel& model, int joint) {
    if (joint < 0) return {};

    Vec3 lo{kInf, kInf, kInf};
    Vec3 hi{-kInf, -kInf, -kInf};
    bool any = false;

    // Base layer only. An outer layer is the same box inflated, so including
    // it would push the fist a quarter of a pixel out of the sleeve.
    for (const ModelBox& box : model.boxes) {
        if (box.joint != joint || box.layer != LayerBase) continue;
        lo = minv(lo, box.origin);
        hi = maxv(hi, box.origin + box.size);
        any = true;
    }
    if (!any) return {};

    return {(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
}

Vec3 gripVoxel(const VoxelModel& model, float fraction) {
    const IVec3 dims = model.dims();
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return {};

    const Vec3 centre = toVec3(dims) * 0.5f;
    if (fraction <= 0.0f) return centre;

    // At least one layer, so a flat model still answers.
    const float cut = std::max(1.0f, float(dims.y) * fraction);

    Vec3 sum{};
    int count = 0;
    for (int y = 0; y < dims.y; ++y) {
        if (float(y) >= cut) break;
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                if (model.at({x, y, z}) == VoxelModel::kEmpty) continue;
                sum += Vec3{float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f};
                ++count;
            }
        }
    }
    if (count == 0) return centre;
    return sum / float(count);
}

}  // namespace blocky
