#include "engine/physics/constraint.hpp"

#include "engine/physics/body.hpp"

namespace blocky {
namespace {

// A world point expressed in a body's own frame, measured from its centre of
// mass. The inverse of `position + rotate(orientation, local)`.
Vec3 toLocal(const RigidBody& body, Vec3 worldPoint) {
    return rotate(conjugate(body.orientation), worldPoint - body.position);
}

}  // namespace

Constraint ballSocketAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                        Vec3 worldPoint) {
    Constraint joint;
    joint.kind = ConstraintKind::BallSocket;
    joint.bodyA = indexA;
    joint.bodyB = indexB;
    joint.anchorA = toLocal(a, worldPoint);
    joint.anchorB = indexB >= 0 ? toLocal(b, worldPoint) : worldPoint;

    // Recorded even though a plain ball socket never reads it: `rest` is what
    // a cone limit measures the bend *from*, and a joint that acquired its
    // limits after being made would otherwise be limited around whatever
    // orientation the identity quaternion happens to mean.
    joint.rest = indexB >= 0 ? normalize(conjugate(b.orientation) * a.orientation) : a.orientation;
    return joint;
}

Constraint weldAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB, Vec3 worldPoint) {
    Constraint joint = ballSocketAt(a, indexA, b, indexB, worldPoint);
    joint.kind = ConstraintKind::Weld;

    // `rest` -- A's orientation seen from B -- comes from `ballSocketAt`,
    // which records it for every point joint. Made in place, the weld holds
    // whatever angle the two happen to be at, which is what a player expects
    // from pointing a tool at two props.
    return joint;
}

Constraint hingeAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB, Vec3 worldPoint,
                   Vec3 worldAxis) {
    Constraint joint = ballSocketAt(a, indexA, b, indexB, worldPoint);
    joint.kind = ConstraintKind::Hinge;

    Vec3 axis = normalize(worldAxis);
    joint.axisA = rotate(conjugate(a.orientation), axis);
    joint.axisB = indexB >= 0 ? rotate(conjugate(b.orientation), axis) : axis;
    return joint;
}

Constraint ropeBetween(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                       Vec3 worldPointA, Vec3 worldPointB, float length) {
    Constraint joint;
    joint.kind = ConstraintKind::Distance;
    joint.rope = true;
    joint.bodyA = indexA;
    joint.bodyB = indexB;
    joint.anchorA = toLocal(a, worldPointA);
    joint.anchorB = indexB >= 0 ? toLocal(b, worldPointB) : worldPointB;
    joint.distance = length > 0.0f ? length : blocky::length(worldPointA - worldPointB);
    return joint;
}

Constraint rodBetween(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                      Vec3 worldPointA, Vec3 worldPointB, float length) {
    Constraint joint = ropeBetween(a, indexA, b, indexB, worldPointA, worldPointB, length);
    joint.rope = false;
    return joint;
}

} // namespace blocky
