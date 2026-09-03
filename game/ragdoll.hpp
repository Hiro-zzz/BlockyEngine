#pragma once
// A character that has stopped being steered.
//
// The player is a `Character`: kinematic, upright, and told where to go. A
// ragdoll is the opposite arrangement -- six rigid bodies that the solver owns,
// tied together at the joints a body actually bends at, going wherever physics
// says. The two never turn into each other; a ragdoll is *spawned*, and what it
// wears is the same skin.
//
// -------------------------------------------------------------- why props
//
// Each limb is drawn as a **prop**, not as part of an entity, and that is
// forced rather than chosen. `EntitySet::add` places a model through a
// skeleton: one position, one yaw, and a `Pose` of per-joint Euler angles. A
// ragdoll limb has none of that -- it has a quaternion and a world position of
// its own, arrived at by a solver, and there is no pose that expresses six
// independently tumbling boxes.
//
// `PropSet::addTransformed` takes a whole matrix. So the limbs are voxelised
// out of the skin once, and `bodyToWorld` puts each one where its body is --
// the same seam the sandbox already uses for every other falling thing.
//
// ------------------------------------------------------------ what it is not
//
// Not the player. Nothing here feeds back into the controller, and killing the
// player is not a thing this game has. A ragdoll is furniture that fell over:
// you spawn it, it settles, it goes to sleep, and it stays where it landed --
// until the budget in `main.cpp` decides there are enough of them, which is
// the other half of the arrangement and the reason `despawnRagdoll` exists.
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"

#include "game/avatar.hpp"

#include <vector>

namespace game {

struct Ragdoll {
    // Body indices into the `PhysicsWorld`, one per skin part, in `SkinPart`
    // order. -1 for a part that could not be built.
    int bodies[blocky::PartCount];

    // The colliders the bodies point at. Owned here because a `RigidBody`
    // holds a bare pointer and the physics world will not outlive this, but
    // the caller might rebuild the avatar.
    std::vector<blocky::Collider> colliders;

    Ragdoll() {
        for (int i = 0; i < blocky::PartCount; ++i) bodies[i] = -1;
    }
};

struct RagdollSettings {
    // Kilograms per cubic world unit. A player is about 0.9 cubic blocks of
    // limb, so this puts one at roughly seventy of whatever the solver's mass
    // unit is -- which is the right order next to a spawned crate.
    float density = 78.0f;

    // Joints give a little rather than locking. A ball socket with no friction
    // is a pendulum, and six frictionless pendulums tied together is a body
    // that never stops twitching and never lets its island sleep.
    float jointFriction = 0.9f;

    float restitution = 0.02f;
    float friction = 0.8f;

    // How hard it is thrown when spawned, along the spawn direction, and how
    // fast the whole thing tumbles about its chest while it goes.
    float launchSpeed = 4.0f;
    float launchSpin = 2.2f;   // radians per second

    // Multiplies every angle in the joint table. One leaves them as written;
    // below one is a stiffer body, above one a looser. Here so that "how
    // floppy is this" is one number rather than ten, and because it is the
    // only thing about a ragdoll anybody ever wants to argue about.
    float limitScale = 1.0f;
};

// Build one at `feetPosition`, facing `yawDegrees`, and add every body and
// joint to `physics`. Returns false when the avatar has no voxel limbs to
// build from.
bool spawnRagdoll(Ragdoll& ragdoll, blocky::PhysicsWorld& physics, const Avatar& avatar,
                  blocky::Vec3 feetPosition, float yawDegrees, blocky::Vec3 launchDirection,
                  const RagdollSettings& settings = {});

// Take one out of the world: every body, and with them every joint.
//
// Not optional, and not only about memory. A body that is never removed is a
// body the broad phase keeps testing against every other one, so a game that
// spawns ragdolls and never takes any away spends a growing share of every
// step on things nobody is looking at any more. The caller is expected to
// forget the ragdoll immediately afterwards -- its slots go back into
// circulation, so its indices stop meaning what they meant.
void despawnRagdoll(Ragdoll& ragdoll, blocky::PhysicsWorld& physics);

// Whether every limb has settled. What a budget should retire first: a
// ragdoll that has stopped moving is finished, and one still falling is the
// one being watched.
bool ragdollAsleep(const Ragdoll& ragdoll, const blocky::PhysicsWorld& physics);

// The average of its limbs, for deciding which one is far enough away to go.
blocky::Vec3 ragdollCentre(const Ragdoll& ragdoll, const blocky::PhysicsWorld& physics);

// Draw one: each limb at its body's transform.
void addRagdollProps(blocky::PropSet& props, const Ragdoll& ragdoll,
                     const blocky::PhysicsWorld& physics, const Avatar& avatar);

} // namespace game
