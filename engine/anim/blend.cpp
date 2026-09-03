#include "engine/anim/blend.hpp"

#include <algorithm>

namespace blocky {

JointPose animLerp(const JointPose& a, const JointPose& b, float t) {
    JointPose r;
    r.rotationDegrees = animLerp(a.rotationDegrees, b.rotationDegrees, t);
    r.offset          = animLerp(a.offset, b.offset, t);
    r.scale           = animLerp(a.scale, b.scale, t);
    return r;
}

Pose animLerp(const Pose& a, const Pose& b, float t) {
    Pose out;
    const int count = int(std::max(a.size(), b.size()));
    for (int i = 0; i < count; ++i) out[i] = animLerp(a[i], b[i], t);
    return out;
}

Camera animLerp(const Camera& a, const Camera& b, float t) {
    Camera r = a;
    r.position      = animLerp(a.position, b.position, t);
    r.target        = animLerp(a.target, b.target, t);
    r.up            = animLerp(a.up, b.up, t);
    r.fovY          = animLerp(a.fovY, b.fovY, t);
    r.orthoHeight   = animLerp(a.orthoHeight, b.orthoHeight, t);
    r.aspect        = animLerp(a.aspect, b.aspect, t);
    r.aperture      = animLerp(a.aperture, b.aperture, t);
    r.focusDistance = animLerp(a.focusDistance, b.focusDistance, t);
    return r;
}

Pose poseAdd(const Pose& base, const Pose& overlay, float weight) {
    Pose out;
    const int count = int(std::max(base.size(), overlay.size()));
    for (int i = 0; i < count; ++i) {
        const JointPose& x = base[i];
        const JointPose& y = overlay[i];
        JointPose& r = out[i];
        r.rotationDegrees = x.rotationDegrees + y.rotationDegrees * weight;
        r.offset          = x.offset + y.offset * weight;
        r.scale           = x.scale * (1.0f + (y.scale - 1.0f) * weight);
    }
    return out;
}

} // namespace blocky
