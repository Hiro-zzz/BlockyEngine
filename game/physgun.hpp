#pragma once
// The physgun: point at a thing, and the thing goes where you point.
//
// This is the tool the sandbox was aimed at from the start. A world you can
// only add blocks to is a builder; a world where anything loose can be picked
// up, turned, carried and pinned in mid-air is a toy, and the difference is
// one tool rather than one subsystem -- everything under it already exists.
//
// ----------------------------------------------------------- how it holds
//
// Not with a joint, and the reason is worth writing down because a joint is
// the obvious answer and it does not work here.
//
// The solver's joints are made of two passes: a velocity pass that stops the
// error growing, and a position pass that removes the error it already has --
// and the second is deliberately slow. `maxCorrectionSpeed` is one block a
// second, because a joint correcting faster than that turns a badly-placed
// contraption into a catapult. A physgun is the opposite case: the player
// swings the muzzle across the room and the crate is expected to arrive.
// Bolting a weld to a world anchor and dragging the anchor gets a crate that
// trails a metre behind and never catches up.
//
// So the hold is a **servo**, run once per fixed step and before the solver:
// work out where the grip point should be, set the velocity that puts it
// there, and let the step take it from there. That is what makes it feel
// direct, and -- because it is a *velocity* and not a teleport -- what keeps
// the crate solid on the way: gravity, contacts and joints all still run
// afterwards, so a held thing dragged into a wall stops at the wall instead
// of passing through it.
//
// ------------------------------------------------------------- and freezes
//
// Freezing is a body state, not a tool state (`RigidBody::freeze`), which
// means a frozen plank is still something to stack on and something the next
// grab can pick up again. The tool only says when.
#include "engine/physics/physics_world.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/scene/camera.hpp"
#include "engine/world/world.hpp"

namespace blocky {
class Window;
class Overlay;
}

namespace game {

struct Player;

// The tool itself, as voxels, plus the two points on it anybody needs.
//
// Drawn rather than described: a tool with nothing in the hand is a tool
// nobody can tell they are holding, and "press F and the interface says
// PHYSGUN" is not the same thing as holding one. It is a `VoxelModel` for the
// same reason the first-person arm is -- a prop takes a whole matrix and any
// scale, which is what a view model needs and what an `Entity` cannot give.
//
// Built in code, like the block textures and the fallback skin: the game
// promises to need no asset files, and a gun is not something a block palette
// can generate, so it is written out shape by shape below.
//
// The model faces **-Z**, the same way an entity does, with +Y up and +X the
// character's right. So a voxel's z is how far back from the muzzle it is.
struct PhysgunModel {
    blocky::VoxelModel voxels;

    // Which voxel sits in the fist. A tool held by its bounding box centre
    // hangs in the air in front of the hand; held by its grip it is held.
    blocky::Vec3 gripVoxel{};

    // Where the beam leaves it. The overlay draws from here, so the beam
    // follows the swing and the walk without being told about either.
    blocky::Vec3 muzzleVoxel{};
};

PhysgunModel buildPhysgunModel();

struct PhysgunSettings {
    // How far it can reach to pick something up, and the range it can then
    // hold it at. The near limit keeps a grabbed crate out of the camera; the
    // far one is what stops the beam becoming a way to rearrange a hill from
    // the other side of the valley.
    float reach = 32.0f;
    float minDistance = 1.8f;
    float maxDistance = 28.0f;

    // Blocks per wheel notch, and how fast the mouse turns a held thing.
    float scrollStep = 1.2f;
    float turnDegreesPerCount = 0.35f;

    // How hard the servo pulls, as a rate: the held thing closes the gap to
    // where it should be at this many times the distance per second. High
    // enough to feel direct, low enough that a crate wrenched free of a heap
    // does not arrive faster than the solver can find the contacts it just
    // left.
    float followRate = 16.0f;
    float turnRate = 12.0f;

    // Ceilings on what the servo may ask for. Without them a grab made
    // through a wall -- or at the far end of the reach -- starts by asking
    // for a hundred blocks a second, and the step that follows tunnels.
    float maxSpeed = 26.0f;
    float maxAngularSpeed = 12.0f;
};

struct Physgun {
    PhysgunSettings settings;

    // Whether the tool is in hand. While it is, the mouse buttons belong to
    // it and not to breaking and placing blocks -- a tool that shared them
    // would mine the wall behind whatever it failed to grab.
    bool equipped = false;

    // ------------------------------------------------------------ the grip
    int  held = -1;          // body index, or -1
    float distance = 0.0f;   // how far out along the aim it is held

    // Where on the body it was taken hold of, in the body's own frame. A
    // plank grabbed by one end stays held by that end; without this every
    // grab snaps the thing's middle to the crosshair.
    blocky::Vec3 gripLocal{};

    // The orientation the servo drives it to. Starts as whatever it was at
    // the moment of the grab, so picking something up does not turn it, and
    // moves only when the player asks.
    blocky::Quat holdOrientation = blocky::Quat::identity();

    // ------------------------------------------------------- for the drawing
    blocky::Vec3 gripWorld{};   // where the beam ends, valid while holding
    bool  aiming = false;       // something grabbable under the crosshair
    blocky::Vec3 aimPoint{};

    // Where the model's muzzle ended up this frame, filled in by whoever
    // placed the view model. The beam starts there rather than at a fixed
    // corner of the screen, so it swings when the hand does; false when
    // nothing placed it, and then the corner is the fallback.
    bool  muzzleKnown = false;
    blocky::Vec3 muzzleWorld{};

    bool holding() const { return held >= 0; }
};

// What the crosshair is on, and whether the trigger is down: one call a
// frame, before the fixed steps.
//
// `eye` and `aim` are the character's true eye and look direction rather than
// the animated camera's, for the same reason `updateReach` uses them: a beam
// that wobbled with the view bob would miss what the crosshair is on.
void aimPhysgun(Physgun& gun, const blocky::World& world, blocky::PhysicsWorld& physics,
                const blocky::Window& window, blocky::Vec3 eye, blocky::Vec3 aim);

// Takes hold of whatever is in front of the eye, with no mouse involved.
// Returns false when there was nothing to take.
//
// The click path calls this, and so does a snapshot: an interface that can
// only be reached with a hand on a mouse cannot be checked from a build
// script, which is the same argument the snapshot mode itself rests on.
bool grabPhysgun(Physgun& gun, const blocky::World& world, blocky::PhysicsWorld& physics,
                 blocky::Vec3 eye, blocky::Vec3 aim);

// One fixed step of holding. Does nothing when nothing is held.
void stepPhysgun(Physgun& gun, blocky::PhysicsWorld& physics, blocky::Vec3 eye, blocky::Vec3 aim,
                 float dt);

// Turns what is held, from this frame's mouse movement. Returns true when it
// consumed the mouse, in which case the *view* must not turn as well --
// otherwise the room spins while the crate does.
bool turnHeldObject(Physgun& gun, const blocky::Window& window, const Player& player);

// Lets go of whatever is held, if anything.
void releasePhysgun(Physgun& gun);

// Unfreezes everything in the world. Returns how many bodies were let go, so
// the interface can say so -- a key that silently does nothing when there was
// nothing frozen is a key people press twice.
int unfreezeAll(blocky::PhysicsWorld& physics);

// The beam, and the tool's name. Drawn over a finished frame like the rest of
// the interface; the beam is a line of screen-space marks between the muzzle
// and the grip rather than geometry in the world, because the viewport draws
// blocks, entities and props and there is deliberately no fourth thing it can
// be handed.
void drawPhysgun(const Physgun& gun, blocky::Overlay& overlay, const blocky::Camera& camera);

} // namespace game
