#pragma once
// Pointing at a body: a ray against everything the solver owns.
//
// The renderer already answers this question -- `intersectScene` finds the
// prop under a ray -- and deliberately answers a different one. It works on
// the *drawn* set, a `PropSet` that was flattened and frozen when the frame
// was built, and it hands back a material rather than a body. What a tool
// needs is the thing that will still be there next step, and it needs to know
// which one so it can constrain it.
//
// So this walks the bodies. That is a linear scan with a sphere reject, which
// is the same shape as the broad phase next door and honest for the same
// reason: a scan over a few hundred bodies costs less than the frame it is
// answering for, and when that stops being true both want the same tree.
//
// Against the world, `raycast` in `world/raycast.hpp` is still the right call
// -- and a tool wants both, because grabbing a crate through a wall is not a
// tool, it is a bug. Compare the two distances and take the nearer.
#include "engine/physics/physics_world.hpp"
#include "engine/world/raycast.hpp"

namespace blocky {

struct BodyPick {
    int   body = -1;
    float distance = 0.0f;

    // Where the ray met it, and which way that surface faces. The point is
    // what a grip is anchored at; without it a physgun holds every crate by
    // its middle, and a plank picked up by one end swings round to be held
    // in the centre the instant it is grabbed.
    Vec3  position{};
    Vec3  normal{};
};

// Nearest body along the ray, or false when there is none within
// `maxDistance`. Sleeping bodies are found: being asleep is an optimisation,
// and a tool that could not pick up anything that had settled would be able
// to pick up almost nothing.
bool pickBody(const PhysicsWorld& physics, const Ray& ray, float maxDistance, BodyPick& pick);

} // namespace blocky
