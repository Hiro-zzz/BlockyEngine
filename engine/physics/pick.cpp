#include "engine/physics/pick.hpp"

#include <cmath>

namespace blocky {
namespace {

// Slab test against one box of the decomposition, in the body's own frame
// where every box is axis aligned. Returns the entry distance, or a negative
// number for a miss.
//
// A ray starting *inside* the box reports zero rather than the exit: a tool
// pointed at something it is standing in has hit it.
float hitBox(const ColliderBox& box, Vec3 origin, Vec3 direction, float maxDistance,
             Vec3& normalOut) {
    Vec3 lo = box.center - box.halfExtents;
    Vec3 hi = box.center + box.halfExtents;

    float entry = 0.0f, exitAt = maxDistance;
    int axis = 0;
    float sign = 1.0f;

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(direction[i]) < 1e-8f) {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return -1.0f;
            continue;
        }
        float inverse = 1.0f / direction[i];
        float t0 = (lo[i] - origin[i]) * inverse;
        float t1 = (hi[i] - origin[i]) * inverse;
        float enterSign = -1.0f;
        if (t0 > t1) {
            float swap = t0;
            t0 = t1;
            t1 = swap;
            enterSign = 1.0f;
        }
        if (t0 > entry) {
            entry = t0;
            axis = i;
            sign = enterSign;
        }
        exitAt = std::min(exitAt, t1);
        if (entry > exitAt) return -1.0f;
    }

    normalOut = Vec3{};
    normalOut[axis] = sign;
    return entry;
}

}  // namespace

bool pickBody(const PhysicsWorld& physics, const Ray& ray, float maxDistance, BodyPick& pick) {
    pick = BodyPick{};

    float nearest = maxDistance;
    bool found = false;

    for (int index = 0; index < physics.bodyCount(); ++index) {
        if (!physics.alive(index)) continue;

        const RigidBody& body = physics.body(index);
        if (!body.collider || body.collider->empty()) continue;

        // Into the body's frame, where the collider boxes are axis aligned.
        // A rigid transform, so distances along the ray mean the same on both
        // sides of it and the near-hit comparison below is valid without
        // converting anything back.
        Quat inverse = conjugate(body.orientation);
        Vec3 origin = rotate(inverse, ray.origin - body.position);
        Vec3 direction = rotate(inverse, ray.direction);

        // Sphere reject first: the bound the broad phase already keeps.
        Vec3 toCentre = -origin;
        float along = dot(toCentre, direction);
        float perpendicular = lengthSq(toCentre) - along * along;
        float radius = body.collider->radius;
        if (perpendicular > radius * radius) continue;
        if (along < -radius || along - radius > nearest) continue;

        for (const ColliderBox& box : body.collider->boxes) {
            Vec3 localNormal{};
            float t = hitBox(box, origin, direction, nearest, localNormal);
            if (t < 0.0f || t >= nearest) continue;

            nearest = t;
            found = true;
            pick.body = index;
            pick.distance = t;
            pick.position = ray.origin + ray.direction * t;
            pick.normal = rotate(body.orientation, localNormal);
        }
    }

    return found;
}

} // namespace blocky
