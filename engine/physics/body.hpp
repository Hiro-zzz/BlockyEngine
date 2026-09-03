#pragma once
// A rigid body: the state that a voxel model needs in order to fall.
//
// `Prop` already holds position, three angles and a uniform voxel size, and
// says in its own comment that the set flattens each placement into a
// rigid-plus-uniform-scale transform. That is the same state this integrates,
// which is why physics did not need a new object model -- only a place to put
// velocity and mass.
//
// The one substitution is orientation. Euler angles are exact everywhere else
// in this engine because every joint it has is a single-axis hinge, and the
// animation layer says so explicitly. A body under torque turns about an axis
// that itself moves, so it is the case that argument excludes, and the
// quaternion in `Quat` was added for it.
#include "engine/core/math.hpp"
#include "engine/physics/collider.hpp"

namespace blocky {

struct RigidBody {
    // ---------------------------------------------------------------- state
    // `position` is the world position of the **centre of mass**, not of the
    // model's corner: a free body rotates about its centre of mass, and any
    // other origin would need the offset re-applied every step.
    Vec3 position{};
    Quat orientation = Quat::identity();
    Vec3 linearVelocity{};
    Vec3 angularVelocity{};

    // ------------------------------------------------------- mass properties
    // Inverse mass of zero means infinite: the body is immovable. Nothing in
    // the solver branches on "static" -- an infinite mass falls out of the
    // same arithmetic as a heavy one, which is the usual reason to store the
    // inverse rather than the mass.
    float invMass = 0.0f;
    Mat3  invInertiaLocal{};

    // ------------------------------------------------------------- material
    float restitution = 0.0f;   // 0 lands dead, 1 would bounce back forever
    float friction = 0.6f;

    // --------------------------------------------------------------- shape
    const Collider* collider = nullptr;  // must outlive the body
    float voxelSize = 1.0f / 16.0f;      // world units per voxel, as in Prop

    // ------------------------------------------------------------ sleeping
    // A body that has been slow for long enough stops being integrated at
    // all. Without this a resting box never quite stops: the solver leaves a
    // little energy every step and it shivers in place forever.
    bool  sleeping = false;
    float restSeconds = 0.0f;

    // ------------------------------------------------------------- frozen
    // Held where it is until somebody lets it go. The third state a body can
    // be in, and it is not either of the other two.
    //
    // *Sleeping* is the solver deciding a body has stopped; anything that
    // touches it wakes it up, which is exactly what must not happen here.
    // *Static* is a body that was never going to move at all. Frozen is a
    // dynamic body that has been told to stop being one, and the thing that
    // makes it a feature rather than a flag is that it must still be solid:
    // in a sandbox, freezing is how a wall gets built one plank at a time.
    //
    // Implemented as its own mass rather than as a branch. The mass goes to
    // infinity and the real one waits here, so every line in the solver --
    // effective mass, impulses, islands, sleep -- treats it exactly as it
    // already treats something immovable, and there is no new case to forget.
    bool  frozen = false;
    float unfrozenInvMass = 0.0f;
    Mat3  unfrozenInvInertia{};

    // ------------------------------------------------------- solver scratch
    // Pushing bodies apart is a correction to *position*, and doing it by
    // adding real velocity means the energy stays in the body after the
    // overlap is gone. In a stack that energy has nowhere to go and the pile
    // hums and creeps.
    //
    // So the correction is solved into a second, parallel velocity that moves
    // the body during integration and is then thrown away. Transient: zero
    // outside a step, and not part of the body's state.
    Vec3 pseudoLinear{};
    Vec3 pseudoAngular{};

    bool isStatic() const { return invMass == 0.0f; }

    Mat3 invInertiaWorld() const {
        Mat3 r = toMat3(orientation);
        return r * invInertiaLocal * transpose(r);
    }

    Vec3 velocityAt(Vec3 worldPoint) const {
        return linearVelocity + cross(angularVelocity, worldPoint - position);
    }

    void applyImpulse(Vec3 impulse, Vec3 worldPoint) {
        if (isStatic()) return;
        linearVelocity = linearVelocity + impulse * invMass;
        angularVelocity = angularVelocity + invInertiaWorld() * cross(worldPoint - position, impulse);
    }

    Vec3 pseudoVelocityAt(Vec3 worldPoint) const {
        return pseudoLinear + cross(pseudoAngular, worldPoint - position);
    }

    void applyPseudoImpulse(Vec3 impulse, Vec3 worldPoint) {
        if (isStatic()) return;
        pseudoLinear = pseudoLinear + impulse * invMass;
        pseudoAngular = pseudoAngular + invInertiaWorld() * cross(worldPoint - position, impulse);
    }

    // A couple: turns the body without moving it. Joints need this -- holding
    // two things at a fixed angle is a constraint on rotation alone.
    void applyAngularImpulse(Vec3 impulse) {
        if (isStatic()) return;
        angularVelocity = angularVelocity + invInertiaWorld() * impulse;
    }

    void applyPseudoAngularImpulse(Vec3 impulse) {
        if (isStatic()) return;
        pseudoAngular = pseudoAngular + invInertiaWorld() * impulse;
    }

    void wake() {
        sleeping = false;
        restSeconds = 0.0f;
    }

    // Freezing is reversible and the body remembers its own mass, so nothing
    // outside has to keep a list of what things weighed. Prefer
    // `PhysicsWorld::setFrozen`, which also wakes whatever was resting on it.
    void freeze() {
        if (frozen || isStatic()) return;
        frozen = true;
        unfrozenInvMass = invMass;
        unfrozenInvInertia = invInertiaLocal;
        invMass = 0.0f;
        invInertiaLocal = Mat3{};
        linearVelocity = Vec3{};
        angularVelocity = Vec3{};
    }

    void unfreeze() {
        if (!frozen) return;
        frozen = false;
        invMass = unfrozenInvMass;
        invInertiaLocal = unfrozenInvInertia;
        wake();
    }
};

// Fills invMass and invInertiaLocal from the collider at a uniform density.
// A density of 0, or an empty collider, leaves the body static.
void setMassFromCollider(RigidBody& body, const Collider& collider, float density);

// The transform taking the model's **voxel** coordinates into world space --
// what `PropSet::addTransformed` wants.
//
// The physics works in a frame centred on the centre of mass and the renderer
// works from the model's corner, so something has to hold the offset between
// them. It is here, once, for the same reason `entityJointToWorld` exists:
// two copies of a placement formula agree until one of them changes, and then
// a body and its picture drift apart in a way that reads as a bad model
// rather than as a bug.
Mat4 bodyToWorld(const RigidBody& body);

} // namespace blocky
