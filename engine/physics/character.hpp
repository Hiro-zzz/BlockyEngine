#pragma once
// A player on foot: an upright box that walks, falls, and does not go through
// walls.
//
// Deliberately **not** a rigid body. A `RigidBody` is something the solver
// owns and pushes around; a character is something a person steers, and the
// two want opposite things. A body under a constraint solver has friction,
// restitution, tipping and sleep -- all of which a player experiences as the
// controls being vague. A character is kinematic: it goes where it is told,
// and collision only takes away what it cannot have.
//
// The world makes this the easy case. Against a lattice of unit cubes there is
// no mesh to accelerate, no thin triangle to fall through, and no ambiguity
// about what a surface is: the box is moved one axis at a time, and after each
// move it is pushed out of whatever cells it now overlaps. Three sweeps, no
// solver.
//
// The numbers are Minecraft's rather than Earth's on purpose. Gravity here is
// about 24 m/s^2, not 9.81, and jump velocity is set to clear one block with a
// little to spare. Real gravity at this scale feels like walking underwater --
// the blocks are a metre across, so a realistic fall takes a visibly long time
// to cross one.
#include "engine/core/math.hpp"
#include "engine/world/world.hpp"

namespace blocky {

struct CharacterSettings {
    // Width and depth of the box, and how tall it stands.
    float width = 0.6f;
    float height = 1.8f;

    // Where the camera sits above the feet.
    float eyeHeight = 1.62f;

    float walkSpeed = 4.3f;
    float runSpeed = 5.9f;

    // Enough to clear a block and land back on it.
    float jumpSpeed = 8.6f;
    float gravity = 24.0f;
    float terminalSpeed = 60.0f;

    // How high a ledge can be walked onto without jumping.
    //
    // Just over a block, which is a **choice** and not the game's number:
    // Minecraft uses 0.6, so a full block has to be jumped and only slabs and
    // stairs are walked. A staircase of single blocks is the commonest thing
    // anyone builds in a voxel world, and having to jump each one is the
    // difference between a world you walk through and one you fight. Set it
    // to 0.6 to get the stricter feel back.
    float stepHeight = 1.05f;

    // How much of the wish direction applies with no ground under the feet.
    float airControl = 0.28f;

    // Fraction of horizontal speed kept per second when nothing is asked for.
    float groundFriction = 0.02f;
    float airFriction = 0.85f;

    // ------------------------------------------------------------- swimming
    //
    // Water is neither a wall nor air, and treating it as either is wrong in
    // a way anybody notices in the first second. As air you fall to the
    // bottom of a lake at terminal speed and walk out along the bed; as a
    // wall you stand on the surface. A fluid cell blocks nothing *and* holds
    // up whatever is inside it, and those are two separate facts about the
    // same block -- see `BlockDef::solid` and `BlockDef::fluid`.
    //
    // What follows is a swimmer, not a fluid: how much of the box is under,
    // times a lift that nearly cancels gravity, plus a drag that turns a
    // four-hundred-block fall into a splash. No pressure, no waves, no
    // current -- none of which a player can tell apart from the real thing
    // while treading water in a voxel lake.

    // Horizontal speed when fully submerged. Slower than a walk, and it has
    // to be: water you can run through is water nobody believes.
    float swimSpeed = 2.6f;

    // Holding jump climbs at this; holding the run key dives at it. Reusing
    // run for "down" rather than adding a key is deliberate -- sprinting is
    // the one thing that means nothing while swimming, so the binding is free
    // exactly where it is needed.
    float swimUpSpeed = 3.4f;
    float swimDownSpeed = 3.0f;

    // Buoyancy as a fraction of gravity, at full submersion. Just under one,
    // so letting go of everything sinks you slowly. Above one and the body
    // pops out of the water like a cork -- which reads as a bug even to
    // somebody who could not say what is wrong with it.
    float buoyancy = 0.94f;

    // Fraction of velocity kept per second when fully submerged, applied to
    // all three axes. This is the drag, and it is what makes water feel like
    // water rather than like slow air.
    float waterDrag = 0.02f;

    // The fastest a fully submerged body may fall.
    //
    // Drag alone is not enough. An exponential decay from terminal speed
    // still carries fifteen blocks down before it bites, so a dive into a
    // lake ends at the bed rather than in the water -- which is the same bug
    // as having no water at all, only slower. The cap interpolates from
    // `terminalSpeed` to this with submersion, so a body meets it as it goes
    // under rather than at a line drawn on the surface.
    float waterEntrySpeed = 9.0f;

    // How much of the wish direction applies while swimming. Water is not
    // ground, but it is far more than air: you can turn in it.
    float swimControl = 0.62f;

    // Submersion at or above which, with nothing underfoot, the character
    // reports itself as *swimming* rather than as falling through water.
    //
    // Only a report: nothing in the step branches on it. What jump means is
    // decided by whether the feet have anything to push off, which needs no
    // threshold and gets wading through a river right for free.
    float swimThreshold = 0.5f;
};

struct Character {
    // The feet: centred in x and z, on the floor in y.
    Vec3  position{};
    Vec3  velocity{};

    float yawDegrees = 0.0f;    // 0 looks towards -Z, matching every entity
    float pitchDegrees = 0.0f;

    bool onGround = false;

    // Set when the last step was stopped by something in that direction --
    // enough for a footstep sound or a wall-slide, without exposing contacts.
    bool hitWall = false;
    bool hitCeiling = false;

    // ------------------------------------------------------------- in water
    // How much of the box is inside fluid, as a fraction of its height. One
    // number rather than a flag, because everything about swimming is a
    // matter of degree: wading is the same physics as floating with a smaller
    // coefficient, and a controller with a boolean here needs a threshold at
    // every use of it instead of one.
    float submersion = 0.0f;

    bool inWater = false;    // any of the box is in fluid
    bool swimming = false;   // in fluid and off the ground: no walk cycle

    // Whether the eye itself is under. What the view is tinted by, and not
    // derivable from `submersion` -- the eye is at 1.62 of 1.8, so a body
    // nine tenths submerged still has its head out.
    bool eyesUnderwater = false;

    // *Which* fluid, at the feet and at the eye, or air for neither.
    //
    // The controller does not care -- water and lava hold a body up the same
    // way, which is why they share every number above. Everything else does:
    // one of them tints the view blue and the other kills you, and a game with
    // a single `inWater` flag has to guess which.
    BlockId fluid = 0;
    BlockId eyeFluid = 0;

    Vec3 eye(const CharacterSettings& settings) const {
        return position + Vec3{0.0f, settings.eyeHeight, 0.0f};
    }

    // Where the character is looking. Same convention as an entity: yaw zero
    // faces -Z, positive pitch looks up.
    Vec3 forward() const;

    // Forward flattened onto the ground, for movement rather than aim: walking
    // while looking at your feet should not slow you down.
    Vec3 walkForward() const;
    Vec3 right() const;
};

// One fixed step.
//
// `wish` is the direction the player is asking to move, in world space and not
// necessarily normalised -- a length above one is clamped, so a controller can
// hand over an analogue stick without thinking about it.
void stepCharacter(Character& character, const World& world, const CharacterSettings& settings,
                   Vec3 wish, bool jump, bool run, float dt);

// How much of the character's box at `position` is inside fluid, 0 to 1 of
// its height. Exposed for the same reason `characterFits` is: a spawner, a
// test or a camera wants the answer without stepping anything.
float characterSubmersion(const World& world, const CharacterSettings& settings, Vec3 position);

// Whether the character's box would overlap anything solid at `position`.
// Exposed because a spawner needs it: dropping a player into a wall is the
// one way to make the controller look broken when it is not.
bool characterFits(const World& world, const CharacterSettings& settings, Vec3 position);

// The first position at or above `position` where the character fits, or the
// position itself when it already does. Used to put someone on the ground
// without knowing the terrain height.
Vec3 dropToGround(const World& world, const CharacterSettings& settings, Vec3 position,
                  float maxFall = 128.0f);

} // namespace blocky
