#include "game/ragdoll.hpp"

#include "engine/entity/model.hpp"

#include <cmath>

namespace game {

using namespace blocky;

namespace {

// One voxel is one model pixel is a sixteenth of a block. The same number the
// entity renderer uses to flatten a box, and the reason a voxelised limb comes
// out exactly the size of the limb it was cut from.
constexpr float kVoxel = 1.0f / 16.0f;

// The joints a body actually bends at, as a table rather than as code.
//
// `anchor` is in **entity model pixels**, in the same frame the model boxes
// are authored in: feet at y zero, +Y up, the character's own right hand at
// +X. Written where the joint really is rather than where the boxes meet,
// because a shoulder is inside the torso and a hip is inside the pelvis.
//
// `cone` and `twist` are how far that joint may bend and spin, in degrees,
// and they are what turns six boxes on ball sockets into something with a
// shape. Without them the solver is not wrong -- a ball socket really does
// leave all three rotations free -- but the result is a bag of parts: the
// head rotates all the way round, the legs fold up through the chest, and
// every landing ends in a pose no body has ever been in.
//
// The numbers are roughly a person's range and deliberately generous. A
// ragdoll that is limited too tightly stops going slack and starts looking
// like a mannequin being posed, which is a worse failure than a loose one
// because it reads as stiffness rather than as death.
struct JointSpec {
    SkinPart parent;
    SkinPart child;
    Vec3 anchor;
    float cone;
    float twist;
};

const JointSpec kJoints[] = {
    // A neck turns a long way and tilts less; both are small next to a limb.
    {PartBody, PartHead, {0.0f, 24.0f, 0.0f}, 42.0f, 70.0f},

    // Shoulders are the loosest joint on a body -- an arm reaches nearly
    // straight up and across the chest -- and the least free to twist.
    {PartBody, PartRightArm, {5.0f, 23.0f, 0.0f}, 100.0f, 45.0f},
    {PartBody, PartLeftArm, {-5.0f, 23.0f, 0.0f}, 100.0f, 45.0f},

    // Hips swing well forward, much less back, and hardly twist. The cone is
    // symmetric, so it is set by the larger of the two and the smaller is
    // paid for by the leg passing a little further behind than it should --
    // which nobody has ever noticed on a body that has stopped moving.
    {PartBody, PartRightLeg, {2.0f, 12.0f, 0.0f}, 75.0f, 30.0f},
    {PartBody, PartLeftLeg, {-2.0f, 12.0f, 0.0f}, 75.0f, 30.0f},
};

// Where a part's base-layer box starts, read off the model rather than
// written down here.
//
// The alternative is a second table of origins, which would agree with
// `buildPlayerModel` right up until somebody changed an arm width -- and slim
// arms are three pixels wide, so that day has already happened once.
bool partOrigin(const EntityModel& model, SkinPart part, Vec3& out) {
    for (const ModelBox& box : model.boxes) {
        if (box.part != part || box.layer != LayerBase) continue;
        out = box.origin;
        return true;
    }
    return false;
}

Vec3 rotateY(Vec3 v, float yawRadians) {
    const float s = std::sin(yawRadians), c = std::cos(yawRadians);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

} // namespace

bool spawnRagdoll(Ragdoll& ragdoll, PhysicsWorld& physics, const Avatar& avatar,
                  Vec3 feetPosition, float yawDegrees, Vec3 launchDirection,
                  const RagdollSettings& settings) {
    const float yaw = radians(yawDegrees);

    // Reserved up front. `RigidBody::collider` is a bare pointer into this
    // vector, so a reallocation part way through would leave every body built
    // so far pointing at freed memory -- the kind of bug that survives a
    // hundred spawns and then crashes on the hundred and first.
    ragdoll.colliders.clear();
    ragdoll.colliders.reserve(PartCount);

    // The parts that make up a body. The outer layers are not among them: a
    // jacket is a shell over an arm, not a seventh limb.
    const SkinPart parts[] = {PartHead, PartBody, PartRightArm,
                              PartLeftArm, PartRightLeg, PartLeftLeg};

    // Thrown as one rigid body rather than as six parts sharing a velocity.
    //
    // The difference is the whole of how a spawn looks. Six identical
    // velocities is a mannequin sliding through the air with its arms out; a
    // linear velocity plus a shared spin about the chest is a person being
    // knocked over, and it costs one cross product per limb. The joints then
    // do what joints do -- the limbs fall behind the turn and the thing goes
    // slack in the air.
    const Vec3 pivot = feetPosition + Vec3{0.0f, 1.1f, 0.0f};
    const Vec3 launch = launchDirection * settings.launchSpeed;

    // About the horizontal axis across the throw, so the tumble is forwards.
    Vec3 spin{};
    if (lengthSq(launchDirection) > 1e-6f) {
        Vec3 across = cross(Vec3{0.0f, 1.0f, 0.0f}, normalize(launchDirection));
        if (lengthSq(across) > 1e-6f) spin = normalize(across) * settings.launchSpin;
    }

    bool built[PartCount] = {};

    for (SkinPart part : parts) {
        const VoxelModel& model = avatar.parts[part];
        if (model.empty()) continue;

        Vec3 origin{};
        if (!partOrigin(avatar.world, part, origin)) continue;

        ragdoll.colliders.push_back(buildCollider(model, kVoxel));
        const Collider& collider = ragdoll.colliders.back();
        if (collider.empty()) {
            ragdoll.colliders.pop_back();
            continue;
        }

        RigidBody body;
        body.collider = &collider;
        body.voxelSize = kVoxel;
        body.restitution = settings.restitution;
        body.friction = settings.friction;
        body.orientation = Quat::axisAngle({0.0f, 1.0f, 0.0f}, yaw);
        setMassFromCollider(body, collider, settings.density);

        // The centre of mass, in the world. The limb's min corner sits at
        // `origin` in entity pixels; the centre of mass sits a further
        // `centreOfMassVoxel` voxels into it, and both are turned by the yaw
        // before being added to the feet.
        const Vec3 local = origin * kVoxel + collider.centreOfMassVoxel * kVoxel;
        body.position = feetPosition + rotateY(local, yaw);
        body.linearVelocity = launch + cross(spin, body.position - pivot);
        body.angularVelocity = spin;

        built[part] = true;
        ragdoll.bodies[part] = physics.add(body);
    }

    if (!built[PartBody]) return false;

    for (const JointSpec& spec : kJoints) {
        if (!built[spec.parent] || !built[spec.child]) continue;

        const int a = ragdoll.bodies[spec.parent];
        const int b = ragdoll.bodies[spec.child];
        const Vec3 world = feetPosition + rotateY(spec.anchor * kVoxel, yaw);

        // `ballSocketAt` records the anchor in each body's own frame from
        // where the bodies are **right now**, so a joint made in place holds
        // them exactly where they were built rather than snapping them
        // together. That is why the bodies are added first and tied second.
        Constraint joint = ballSocketAt(physics.body(a), a, physics.body(b), b, world);
        joint.friction = settings.jointFriction;

        // The limb's own axis, in the torso's frame: every part of a player
        // model hangs downwards from its joint, so the cone opens around -Y
        // and the twist is the spin about it. The cone is symmetric, so the
        // sign only decides which way a twist counts as positive.
        joint.axisB = {0.0f, -1.0f, 0.0f};
        joint.coneLimitDegrees = spec.cone * settings.limitScale;
        joint.twistLimitDegrees = spec.twist * settings.limitScale;

        physics.addConstraint(joint);
    }

    return true;
}

void despawnRagdoll(Ragdoll& ragdoll, PhysicsWorld& physics) {
    // The joints go with the bodies -- `PhysicsWorld::remove` retires every
    // constraint naming a body it removes -- so this only has to say which
    // bodies. Clearing the ids afterwards is not tidiness: a slot is handed
    // out again, so a stale index here would name somebody else's arm.
    for (int part = 0; part < blocky::PartCount; ++part) {
        if (ragdoll.bodies[part] >= 0) physics.remove(ragdoll.bodies[part]);
        ragdoll.bodies[part] = -1;
    }
    ragdoll.colliders.clear();
}

bool ragdollAsleep(const Ragdoll& ragdoll, const PhysicsWorld& physics) {
    bool any = false;
    for (int part = 0; part < blocky::PartCount; ++part) {
        const int index = ragdoll.bodies[part];
        if (index < 0 || !physics.alive(index)) continue;
        any = true;
        if (!physics.body(index).sleeping) return false;
    }
    return any;
}

Vec3 ragdollCentre(const Ragdoll& ragdoll, const PhysicsWorld& physics) {
    Vec3 sum{};
    int count = 0;
    for (int part = 0; part < blocky::PartCount; ++part) {
        const int index = ragdoll.bodies[part];
        if (index < 0 || !physics.alive(index)) continue;
        sum = sum + physics.body(index).position;
        ++count;
    }
    return count > 0 ? sum / float(count) : Vec3{};
}

void addRagdollProps(PropSet& props, const Ragdoll& ragdoll, const PhysicsWorld& physics,
                     const Avatar& avatar) {
    for (int part = 0; part < PartCount; ++part) {
        const int index = ragdoll.bodies[part];
        if (!physics.alive(index)) continue;
        if (avatar.parts[part].empty()) continue;

        props.addTransformed(&avatar.parts[part], bodyToWorld(physics.body(index)));
    }
}

} // namespace game
