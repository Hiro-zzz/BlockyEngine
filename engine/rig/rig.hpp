#pragma once
// A skeleton and a pose for it.
//
// Rigging is not animation. A rig is the structure you *pose* through, and a
// still frame needs that more than a moving one does: getting a hand where you
// want it by writing six independent rotations and hoping is the thing this
// replaces. Nothing here moves over time, and nothing here needs to.
//
// What the flat six-rotation Pose could not do, and this can:
//
//   - Parenting. Turning the torso takes the head and arms with it, because
//     they hang off it. Before, every part rotated about its own pivot in
//     isolation and a twisted torso left its own arms behind in mid-air.
//   - Detail. Joints are not limited to the six named body parts, so an arm
//     can gain an elbow, a head a jaw, a face a brow -- see entity/rigging.hpp.
//   - Reuse. A skeleton knows nothing about skins or voxels, so the same one
//     drives an entity's boxes and a prop's grids.
//
// Units are the model's own: pixels for entity models (sixteen to a block),
// voxels for props. The skeleton never converts; whoever places the model does.
#include "engine/core/math.hpp"

#include <string>
#include <vector>

namespace blocky {

struct Joint {
    std::string name;
    int  parent = -1;  // index of the parent joint, -1 for a root
    Vec3 pivot{};      // the point this joint turns about, in model units
};

// What one joint does, on top of its rest position.
struct JointPose {
    Vec3  rotationDegrees{};  // about the joint pivot: Z, then Y, then X
    Vec3  offset{};           // translation, in model units, applied after
    float scale = 1.0f;       // uniform, about the pivot
};

// Joint indices of the standard humanoid skeleton, fixed in this order so a
// pose can name them without holding a skeleton -- the same trick the block
// palette uses. Anything a rig grows afterwards lands past HumanoidCount.
namespace joint {
inline constexpr int Root     = 0;
inline constexpr int Body     = 1;
inline constexpr int Head     = 2;
inline constexpr int RightArm = 3;
inline constexpr int LeftArm  = 4;
inline constexpr int RightLeg = 5;
inline constexpr int LeftLeg  = 6;
inline constexpr int HumanoidCount = 7;
} // namespace joint

class Pose;

class Skeleton {
public:
    // Joints are stored parents-first: `parent` must already exist, which
    // makes the storage order topological and resolve() a single forward pass.
    // Returns the new joint's index, or -1 if the parent is not valid.
    int add(const std::string& name, int parent, Vec3 pivot);

    int find(const std::string& name) const;  // -1 when unknown
    bool has(const std::string& name) const { return find(name) >= 0; }

    size_t size() const { return joints_.size(); }
    bool   empty() const { return joints_.empty(); }

    const Joint& operator[](int index) const { return joints_[size_t(index)]; }
    Joint&       mutableJoint(int index) { return joints_[size_t(index)]; }

    // True if `ancestor` is `index` or any of its parents. Used by the
    // rigging tools to keep a re-parent from making a cycle.
    bool descendsFrom(int index, int ancestor) const;

    // Compose each joint's local transform with its parent's, into one matrix
    // per joint. `out` is resized to size().
    void resolve(const Pose& pose, std::vector<Mat4>& out) const;

    // The same, with every joint at rest.
    void resolveRest(std::vector<Mat4>& out) const;

private:
    std::vector<Joint> joints_;
};

// Per-joint local transforms, addressed by index.
//
// A pose is deliberately independent of any one skeleton: a joint it has
// nothing to say about is simply at rest. That is what lets Pose::striding()
// exist as a plain value, and what keeps a pose usable after a model grows a
// joint it has never heard of.
class Pose {
public:
    // Grows to fit. Use this to set.
    JointPose& operator[](int index);

    // Never grows; out of range reads back as rest.
    const JointPose& operator[](int index) const;

    // By name, which needs the skeleton to look it up. Returns a scratch
    // entry for an unknown name rather than failing, so a pose written for a
    // rig with a jaw still applies to one without.
    JointPose& at(const Skeleton& skeleton, const std::string& name);

    void clear() { joints_.clear(); }
    size_t size() const { return joints_.size(); }

    // ------------------------------------------------------- humanoid poses
    // These address the fixed joint indices above, so they need no skeleton.
    // A rig that lacks one of those joints simply ignores that part.
    static Pose standing() { return {}; }
    static Pose striding(float degrees);
    static Pose waving(float degrees = 150.0f);
    static Pose tPose();

private:
    std::vector<JointPose> joints_;
    JointPose scratch_;  // handed out for names a skeleton does not know
};

// The transform one joint contributes, before its parent is applied.
Mat4 localTransform(const Joint& joint, const JointPose& pose);

} // namespace blocky
