#pragma once
// Contacts: between a rigid body and the voxel world, and between two bodies.
//
// The first is the half that voxels make *easier*, and it is worth saying why.
// A body in Source meets a soup of triangles: it needs a mesh acceleration
// structure, and thin or badly-wound triangles are a permanent source of
// bodies falling through the floor. Here the world is a lattice of unit cubes.
// The broad phase is integer arithmetic on a chunk map -- the same structure
// the raycaster already walks -- and every candidate is the same shape.
//
// So both halves reduce to one narrow-phase test, an oriented box against
// another oriented box, with the world's cells being the trivially oriented
// ones. The only thing the lattice adds is a veto, described below.
#include "engine/physics/body.hpp"
#include "engine/world/world.hpp"

#include <vector>

namespace blocky {

struct Contact {
    Vec3  position{};       // world space
    Vec3  normal{};         // unit, pointing from B towards A
    float depth = 0.0f;     // penetration along the normal, world units

    // Which two things are touching. `bodyB < 0` means the static world,
    // which is not a body and needs no entry: an immovable thing and a thing
    // with infinite mass behave identically in the solver, so the world is
    // simply the case where B contributes nothing.
    int bodyA = -1;
    int bodyB = -1;

    // Which pair of features produced this point: the collider boxes involved
    // and which corner of which of them it is.
    //
    // Contacts are regenerated from nothing every step, so without a name
    // there is no way to say "this is the same corner on the same block as
    // last step" -- and that sentence is the whole of warm starting. The id
    // is a hash rather than a packed field because cell coordinates are
    // unbounded; a collision costs one badly-seeded contact, not correctness.
    uint64_t feature = 0;

    // Solver scratch, kept on the contact so the impulse accumulated over
    // iterations can be clamped against its running total rather than per
    // iteration -- the difference between a box settling and a box shivering.
    float normalImpulse = 0.0f;
    float tangentImpulse[2]{0.0f, 0.0f};

    // The separate accumulator for the position pass. Not warm started: it
    // describes an overlap that is being removed, not a force that persists.
    float pseudoImpulse = 0.0f;
};

// Whether a cell stops a body, and whether it is a fluid, are asked of the
// world: `World::collides` and `World::isFluid`, which read `BlockDef::solid`
// and `BlockDef::fluid` out of the palette that world was built with.
//
// They used to live here as `blockCollides(BlockId)` and `blockIsFluid(id)`,
// comparing against the built-in water and lava. That was honest for exactly
// one palette; anything a caller registered itself came out solid whether it
// said so or not, and physics had to know a block by name to work at all.
// The flags are the fix that comment asked for.

// World-space bounds of a body's collider, for the broad phase.
void bodyBounds(const RigidBody& body, Vec3& lo, Vec3& hi);

// Appends every contact between `body` and the world. The caller clears, and
// fills in `bodyA` afterwards -- the generator does not know the index.
void collideBodyWithWorld(const RigidBody& body, const World& world,
                          std::vector<Contact>& out);

// Appends every contact between two bodies, with the normal pointing from
// `b` towards `a`. Same convention as above, so the solver has one case.
//
// Depth here is the single SAT overlap for every point of a pair of boxes,
// not the per-point figure the lattice case can compute. Against a cell the
// contact plane is a known face of a known cube; between two turned boxes it
// is whichever axis happened to separate them least, and measuring each
// corner against it would be inventing precision. The cost is a mild
// over-correction of the shallower corners of a tilted contact, which the
// slop absorbs.
void collideBodies(const RigidBody& a, const RigidBody& b, std::vector<Contact>& out);

} // namespace blocky
