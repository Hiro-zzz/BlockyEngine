#include "engine/rig/rig.hpp"

namespace blocky {

int Skeleton::add(const std::string& name, int parent, Vec3 pivot) {
    // A parent must already be in the list. That single rule is what keeps
    // the storage topological, and therefore resolve() a forward pass with no
    // recursion and no cycle check.
    if (parent >= int(joints_.size())) return -1;
    if (parent < -1) return -1;

    Joint joint;
    joint.name = name;
    joint.parent = parent;
    joint.pivot = pivot;
    joints_.push_back(std::move(joint));
    return int(joints_.size()) - 1;
}

int Skeleton::find(const std::string& name) const {
    for (size_t i = 0; i < joints_.size(); ++i) {
        if (joints_[i].name == name) return int(i);
    }
    return -1;
}

bool Skeleton::descendsFrom(int index, int ancestor) const {
    while (index >= 0 && index < int(joints_.size())) {
        if (index == ancestor) return true;
        index = joints_[size_t(index)].parent;
    }
    return false;
}

Mat4 localTransform(const Joint& joint, const JointPose& pose) {
    // Turn about the pivot, then shift. Z, then Y, then X, matching the order
    // the flat pose used before there was a skeleton, so the sign conventions
    // in docs/conventions.md still hold.
    Mat4 rotation = rotateAxis({0.0f, 0.0f, 1.0f}, radians(pose.rotationDegrees.z)) *
                    rotateAxis({0.0f, 1.0f, 0.0f}, radians(pose.rotationDegrees.y)) *
                    rotateAxis({1.0f, 0.0f, 0.0f}, radians(pose.rotationDegrees.x));

    if (pose.scale != 1.0f) rotation = rotation * scale(Vec3{pose.scale});

    return translate(joint.pivot + pose.offset) * rotation * translate(-joint.pivot);
}

void Skeleton::resolve(const Pose& pose, std::vector<Mat4>& out) const {
    out.resize(joints_.size());
    for (size_t i = 0; i < joints_.size(); ++i) {
        const Joint& joint = joints_[i];
        Mat4 local = localTransform(joint, pose[int(i)]);
        // The parent is always earlier in the list, so its matrix is ready.
        out[i] = joint.parent >= 0 ? out[size_t(joint.parent)] * local : local;
    }
}

void Skeleton::resolveRest(std::vector<Mat4>& out) const {
    Pose rest;
    resolve(rest, out);
}

JointPose& Pose::operator[](int index) {
    if (index < 0) return scratch_;
    if (size_t(index) >= joints_.size()) joints_.resize(size_t(index) + 1);
    return joints_[size_t(index)];
}

const JointPose& Pose::operator[](int index) const {
    static const JointPose kRest{};
    if (index < 0 || size_t(index) >= joints_.size()) return kRest;
    return joints_[size_t(index)];
}

JointPose& Pose::at(const Skeleton& skeleton, const std::string& name) {
    int index = skeleton.find(name);
    if (index < 0) {
        // A pose written for a rig with a jaw should still apply to one
        // without, minus the jaw. Handing back scratch keeps the call legal
        // and the effect nil.
        scratch_ = JointPose{};
        return scratch_;
    }
    return (*this)[index];
}

Pose Pose::striding(float degrees) {
    Pose pose;
    pose[joint::RightArm].rotationDegrees = {-degrees, 0.0f, 0.0f};
    pose[joint::LeftArm].rotationDegrees  = { degrees, 0.0f, 0.0f};
    pose[joint::RightLeg].rotationDegrees = { degrees, 0.0f, 0.0f};
    pose[joint::LeftLeg].rotationDegrees  = {-degrees, 0.0f, 0.0f};
    return pose;
}

Pose Pose::waving(float degrees) {
    // A limb hangs downwards from its pivot, so a positive rotation about Z
    // swings it towards +X -- outwards for the right arm, across the body for
    // the left. Getting these backwards folds the arms into the torso.
    Pose pose;
    pose[joint::RightArm].rotationDegrees = {0.0f, 0.0f, degrees};
    pose[joint::LeftArm].rotationDegrees  = {6.0f, 0.0f, -4.0f};
    pose[joint::Head].rotationDegrees     = {-8.0f, -14.0f, 0.0f};
    return pose;
}

Pose Pose::tPose() {
    Pose pose;
    pose[joint::RightArm].rotationDegrees = {0.0f, 0.0f,  90.0f};
    pose[joint::LeftArm].rotationDegrees  = {0.0f, 0.0f, -90.0f};
    return pose;
}

} // namespace blocky
