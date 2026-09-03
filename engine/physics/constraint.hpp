#pragma once
// Joints between bodies: the rope, the ball socket, the weld and the axle.
//
// This is the part a sandbox is actually made of. Bodies that fall and stack
// are a demo; bodies that can be *fastened together* are a toy, and every tool
// in the genre -- the welder, the rope, the hydraulic, the thruster on a
// hinge -- is one of these plus a way to point at two things.
//
// They are solved in the same sequential-impulse loop as contacts and in the
// same two passes, so nothing here is a separate system. A constraint is a
// row like any other; the only differences are that its rows may pull as well
// as push, and that it survives from step to step, which means it also joins
// its two bodies into one sleeping island.
#include "engine/core/math.hpp"

namespace blocky {

enum class ConstraintKind {
    // One row along the line between the anchors. `rope` makes it one-sided:
    // a rope stops things drifting apart and does nothing when they approach,
    // which is the whole difference between a rope and a rod.
    Distance,

    // Three rows holding the two anchor points together, leaving all
    // rotation free.
    BallSocket,

    // A ball socket plus three angular rows holding the relative orientation
    // the bodies had when the weld was made.
    Weld,

    // A ball socket plus two angular rows keeping one axis of each body
    // parallel, leaving rotation about that axis free.
    Hinge,
};

struct Constraint {
    ConstraintKind kind = ConstraintKind::BallSocket;

    int bodyA = -1;
    int bodyB = -1;   // < 0 anchors to the world, and anchorB is a world point

    // Attachment points, in each body's own frame and measured from its
    // centre of mass -- the same frame `ColliderBox::center` uses, so a point
    // picked off the collider needs no conversion.
    Vec3 anchorA{};
    Vec3 anchorB{};

    float distance = 0.0f;    // Distance: the length to hold
    bool  rope = false;       // Distance: resist stretching only

    Vec3 axisA{0.0f, 1.0f, 0.0f};   // Hinge: the axle, in A's frame
    Vec3 axisB{0.0f, 1.0f, 0.0f};   // Hinge: the axle, in B's frame

    // Weld, Hinge and a limited BallSocket: A's orientation expressed in B's
    // frame at the moment the joint was made. `weldBodies` and friends fill
    // it in; setting it by hand means deciding what "unbent" looks like.
    Quat rest = Quat::identity();

    // ---------------------------------------------------------- joint limits
    // `BallSocket` only: how far it may bend away from `rest`, and how far it
    // may spin about its own axis. Zero on either leaves that free.
    //
    // A ragdoll is what these are for, and what makes them not optional. Six
    // limbs on unlimited ball sockets is a bag of parts: the knee folds the
    // wrong way, the head turns right round, the elbow passes through the
    // chest. None of that is the solver being wrong -- a ball socket really
    // does leave all three rotations free -- and no amount of friction fixes
    // it, because friction resists motion rather than forbidding a pose.
    //
    // Two angles rather than one, because a joint does not bend and spin by
    // the same amount: a shoulder swings most of a half-circle and twists
    // barely at all. The pair is a swing-twist decomposition of the drift
    // about `axisB`, which for a limited ball socket means the limb's own
    // axis in B's frame -- the default +Y is right for anything hanging off
    // a body, and the cone is symmetric so which way it points does not
    // matter.
    float coneLimitDegrees = 0.0f;
    float twistLimitDegrees = 0.0f;

    // Impulse per step above which the joint gives way. Zero never breaks.
    // A sandbox needs this: the interesting half of building a contraption is
    // watching it fail.
    float breakImpulse = 0.0f;
    bool  broken = false;

    // ------------------------------------------------- friction and driving
    // Resistance to motion in whatever the joint leaves *free*. A frictionless
    // hinge is a pendulum that never stops, which is correct physics and wrong
    // for a sandbox -- things there are meant to come to rest so the island
    // can go to sleep and stop costing anything.
    //
    // Stated as a maximum impulse per step, like contact friction. A contact
    // gets its limit from the normal force it is already carrying; a joint has
    // no equivalent, so the limit is given rather than derived.
    //
    // Which directions are free depends on the kind: the axle for a `Hinge`,
    // all three rotations for a `BallSocket`, the line for a `Distance`. A
    // `Weld` leaves nothing free, so friction on one does nothing.
    float friction = 0.0f;

    // `Hinge` only: a driven axle -- the wheel, the winch, the powered joint
    // a contraption is built around.
    //
    // `motorSpeed` is the relative angular speed about the axle in radians per
    // second. `motorTorque` caps the impulse per step it may spend getting
    // there, and is not optional in spirit: a motor with no limit reaches any
    // speed in one step and turns the joint into a lever that moves the world.
    //
    // A running motor replaces friction rather than adding to it, so a motor
    // set to speed zero is a brake.
    bool  motor = false;
    float motorSpeed = 0.0f;
    float motorTorque = 0.0f;

    // Solver scratch. Kept across steps for the same reason contacts keep
    // theirs -- a joint under steady load should not rediscover its force
    // from zero sixty times a second.
    Vec3 linearImpulse{};
    Vec3 angularImpulse{};
    float distanceImpulse = 0.0f;

    // The position pass has its own accumulators, and unlike the ones above
    // they start at zero every step: they describe an error being removed,
    // not a load being held.
    Vec3 pseudoLinearImpulse{};
    Vec3 pseudoAngularImpulse{};
    float pseudoDistanceImpulse = 0.0f;

    // Friction accumulates per free direction: `.x` alone for a hinge's axle
    // and for a distance joint's line, all three for a ball socket.
    Vec3  frictionImpulse{};
    float motorImpulse = 0.0f;

    // The limit rows. One-sided -- a limit may push back and never pull --
    // so each keeps its own running total to clamp against, like a contact.
    float coneImpulse = 0.0f, twistImpulse = 0.0f;
    float pseudoConeImpulse = 0.0f, pseudoTwistImpulse = 0.0f;
};

// Helpers that record the current relative pose, so a joint made in place
// holds the bodies exactly where they already are rather than snapping them
// to some canonical arrangement.
struct RigidBody;

Constraint weldAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB, Vec3 worldPoint);
Constraint ballSocketAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                        Vec3 worldPoint);
Constraint hingeAt(const RigidBody& a, int indexA, const RigidBody& b, int indexB, Vec3 worldPoint,
                   Vec3 worldAxis);
Constraint ropeBetween(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                       Vec3 worldPointA, Vec3 worldPointB, float length);

// The two-sided version: it pushes as well as pulls, so changing `distance`
// while it runs makes it a hydraulic. That is the whole of a piston -- there
// is no separate joint kind for one, only a rod whose length someone drives.
Constraint rodBetween(const RigidBody& a, int indexA, const RigidBody& b, int indexB,
                      Vec3 worldPointA, Vec3 worldPointB, float length);

} // namespace blocky
