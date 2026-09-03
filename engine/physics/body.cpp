#include "engine/physics/body.hpp"

namespace blocky {

void setMassFromCollider(RigidBody& body, const Collider& collider, float density) {
    body.collider = &collider;

    if (collider.empty() || density <= 0.0f) {
        body.invMass = 0.0f;
        body.invInertiaLocal = Mat3{};
        return;
    }

    float mass = 0.0f;
    Mat3 inertia = inertiaTensor(collider, density, &mass);

    if (mass <= 0.0f) {
        body.invMass = 0.0f;
        body.invInertiaLocal = Mat3{};
        return;
    }

    body.invMass = 1.0f / mass;
    body.invInertiaLocal = inverse(inertia);
}

Mat4 bodyToWorld(const RigidBody& body) {
    Mat4 placement = composeTransform(body.position, body.orientation, body.voxelSize);
    if (!body.collider) return placement;

    // Read right to left: shift the model so its centre of mass sits at the
    // origin, then let the placement above put that origin where the body is.
    return placement * translate(-body.collider->centreOfMassVoxel);
}

} // namespace blocky
