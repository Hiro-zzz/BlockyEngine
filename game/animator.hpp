#pragma once
// Everything about the player that exists only to be *seen*.
//
// The split this file defends: `Character` decides where the player is, and
// nothing here is allowed to argue with it. A walk cycle that pushed the feet,
// or a camera dip that changed what the crosshair hits, would make the
// controls answer to the animation -- which is the difference between a game
// that feels responsive and one that feels like it is arguing with you.
//
// So this reads the character and writes two things, neither of which feeds
// back: a `Pose` for the body, and a `Camera` for the eye.
//
// ------------------------------------------------------------------- timing
//
// The gait runs on **distance walked, not on the clock**. Legs driven by time
// keep striding when a wall stops the player, which is the moonwalk; legs
// driven by ground covered stop when the ground does, and speed up when the
// player does, without either being asked for. Only the things that really are
// events in time -- the swing of an arm, the recovery from a landing -- use
// seconds.
//
// This is the same rule the animation layer states for a take, one floor down:
// see docs/api-anim.md. The difference is that a take is a pure function of
// its frame and this is a state machine, because a swing has to remember that
// it started.
#include "engine/entity/face.hpp"
#include "engine/rig/rig.hpp"
#include "engine/scene/camera.hpp"
#include "engine/world/world.hpp"

namespace game {

struct Player;

struct AnimatorSettings {
    // ------------------------------------------------------------- the gait
    // Blocks covered per full cycle of the arms. One cycle is two steps, so
    // at the walk speed of 4.3 blocks per second this is a little over one
    // cycle a second.
    float blocksPerStride = 3.4f;

    float strideDegreesWalk = 31.0f;
    float strideDegreesRun = 46.0f;

    // How fast the pose reacts to starting and stopping. Not instant, or the
    // legs snap from still to full stride in one frame.
    float speedBlendRate = 9.0f;

    // ---------------------------------------------------------- the view bob
    // Small numbers. Bob is one of the few effects that is unmistakable when
    // absent and nauseating when overdone, and the honest test is whether you
    // notice it while playing rather than whether you can see it in a still.
    float bobVertical = 0.052f;      // blocks
    float bobLateral = 0.043f;
    float bobRollDegrees = 0.85f;

    // ------------------------------------------------------------- landing
    // The dip is a knee bend, so it scales with how hard the landing was and
    // then returns like something with a spring in it rather than a timer.
    float landDipPerSpeed = 0.018f;  // blocks of dip per block/s of impact
    float landDipMax = 0.32f;
    float landStiffness = 17.0f;     // rad/s; damping is critical by construction
    float landMinSpeed = 3.0f;       // below this a landing is not felt at all

    // ------------------------------------------------------------ stepping
    // Walking onto a block moves the feet a whole block in one step, and a
    // camera bolted to the feet teleports with them. The eye is allowed to
    // lag and catch up, which is what turns a teleport into a step.
    float stepCatchUpRate = 13.0f;   // 1/s

    // ------------------------------------------------------------- sprinting
    float sprintFovDegrees = 7.0f;
    float fovRate = 7.0f;

    // -------------------------------------------------------------- actions
    float swingSeconds = 0.26f;

    // --------------------------------------------------------- third person
    float thirdPersonDistance = 3.6f;
    float thirdPersonHeadroom = 0.25f;   // stop this far short of a wall

    // ---------------------------------------------------------- the view arm
    // World units per arm voxel. A real arm voxel is 1/16 of a block; this is
    // about half of that, which is the amount that makes an arm read as an arm
    // at the distance it has to sit.
    float viewArmVoxel = 0.032f;

    // Where the fist sits in front of the eye: right, down, forward. Further
    // out than an arm could reach, which is the other half of the same lie --
    // and the half that decides whether it looks like a limb or like a wall.
    blocky::Vec3 viewArmFist{0.24f, -0.21f, 0.62f};

    // How far the swing carries the fist, on top of that.
    float viewArmSwingLift = 0.09f;

    // Size of a held block on screen, in world units. Small on purpose: a
    // block at anything near true size at half a block from the eye is a
    // third of the screen.
    float viewBlockSize = 0.135f;

    // World units per voxel of a *tool* in the hand.
    //
    // Its own number rather than the arm's, because the two are lying about
    // different things. The arm is smaller than a real arm so a shoulder a
    // quarter of a block from the eye does not fill the frame; a tool is
    // drawn at the size that makes it read as an object being held, and on a
    // twenty-two-voxel gun that comes out longer than the hand holding it --
    // which is what every game in the genre does and what nobody notices.
    float viewToolVoxel = 0.0152f;

    // How far the head may turn before the body follows it round.
    float bodyYawSlack = 38.0f;
    float bodyYawRate = 9.0f;
};

struct Animator {
    AnimatorSettings settings;

    // ----------------------------------------------------------- gait state
    float stridePhase = 0.0f;   // radians, advanced by distance
    float speedBlend = 0.0f;    // 0 still .. 1 at run speed
    float runBlend = 0.0f;      // 0 walking .. 1 running, for stride amplitude

    // ---------------------------------------------------------- action state
    float swing = 0.0f;         // 0..1 through a swing, 0 when idle
    bool  swinging = false;

    // ---------------------------------------------------------- camera state
    // All in blocks, all smoothed, none of them ever read by the controller.
    float stepLag = 0.0f;       // <= 0, how far the eye is still behind the feet
    float landDip = 0.0f;
    float landVelocity = 0.0f;
    float fovBoost = 0.0f;

    // The body turns to follow the head rather than with it, so standing
    // still and looking around turns the head first and the shoulders after.
    float bodyYawDegrees = 0.0f;

    // Eases between the grounded pose and the airborne one, so a jump does
    // not switch shape in a single frame.
    float airBlend = 0.0f;

    // ------------------------------------------------------------ the eyes
    // When the next blink is due, and how far through the current one we are.
    //
    // Blinks are scheduled rather than random-per-frame: a coin flipped every
    // frame gives a rate that depends on the framerate, and on a fast machine
    // the character develops a twitch.
    float nextBlink = 2.5f;
    float blink = 0.0f;      // 0 open, 1 shut
    bool  blinking = false;

    // Where the pupils are pointing, in fractions of their travel. Smoothed,
    // because an eye that snaps to its target reads as a doll's.
    float gazeX = 0.0f, gazeY = 0.0f;

    // Seconds since the animator started, accumulated from the same `dt` it
    // is handed. Used only by the idle sway, which really is a thing that
    // happens in time rather than over ground -- and accumulated rather than
    // read off a wall clock so that a snapshot run, whose dt is fixed, gets
    // the same sway every time it is run.
    float clock = 0.0f;

    // -------------------------------------------------------- what was true
    bool  wasOnGround = true;
    float previousFeetY = 0.0f;
    float fallSpeed = 0.0f;     // downward speed while airborne, kept for the landing
    bool  started = false;
};

// One frame of animation. Call once per rendered frame with the frame's own
// `dt`, not once per fixed physics step: the bob and the swing belong to what
// is drawn, and running them at the step rate makes them stutter whenever the
// two rates disagree.
void animate(Animator& animator, const Player& player, float dt);

// Begin a swing, or restart one already running. Mining holds the button down
// and gets a swing per strike, which is what makes the arm keep going.
void startSwing(Animator& animator);

// The pose for the body.
//
// `firstPerson` only shortens the swing, and the reason is worth knowing
// because it is the boundary of what this can do. The arms here are the
// character's real arms at their real size, and the shoulder sits a quarter of
// a block from the eye -- so an arm raised into view passes close enough to
// the camera to stop reading as an arm and start reading as a wall.
//
// Games solve that with a *view model*: a second, smaller arm drawn with its
// own projection, rigidly following the camera through its pitch. `Entity`
// cannot express one, because an entity carries a yaw and no pitch. So the
// arm here hangs where an arm hangs, is seen by looking down at it, and comes
// up only far enough to read when it swings. That is a real limitation and
// not a tuning choice; see docs/api-physics.md and the game section of the
// README for where a voxel view model would go instead.
blocky::Pose playerPose(const Animator& animator, const Player& player, bool firstPerson);

// Adds the eyes to a pose already built by `playerPose`.
//
// Separate because it needs something `playerPose` has no business knowing:
// the rig, which belongs to the avatar, and which may not exist. A character
// whose skin defeated the scanner still poses; it simply keeps the eyes its
// artist painted.
void poseEyes(const Animator& animator, const blocky::face::EyeRig& rig, blocky::Pose& pose);

// Aim the eyes at a world point -- the camera, in practice -- and advance the
// blink clock. Called once a frame, before `poseEyes`.
//
// `headYawDegrees` is where the head is actually pointing after the body-lag
// and the pitch have had their say, because a glance is measured from the face
// rather than from the feet.
void aimEyes(Animator& animator, const Player& player, blocky::Vec3 target, float dt);

// The camera, with the eye lag, the bob, the landing dip and the sprint field
// of view applied.
//
// `world` is only read in third person, to keep the camera from backing
// through a wall. The returned camera is never what `updateReach` aims with:
// the crosshair follows the head, not the bob.
blocky::Camera playerCamera(const Animator& animator, const Player& player,
                            const blocky::World& world, float fovDegrees, float aspect);

// Where the eye actually ends up, for anything that has to agree with the
// camera about it. Same value `playerCamera` uses.
blocky::Vec3 animatedEye(const Animator& animator, const Player& player);

// ------------------------------------------------------------- the view arm
//
// The first-person hand. Everything about it is a lie told carefully: it is
// smaller than the character's real arm, it hangs in front of the camera
// rather than off a shoulder, and it moves with the head instead of with the
// body. All three are what every engine does, and the reason is geometry
// rather than laziness -- a real arm's shoulder is a quarter of a block from
// the eye, and anything that close is a wall rather than a limb.
//
// It is a prop, so it takes a whole matrix and any scale, which is exactly
// what an `Entity` could not give: an entity carries a yaw and no pitch, and a
// view model has to follow the camera when it looks up.

// Voxel coordinates of the arm model into the world. `armDims` is the model's
// own size, so the fist can be found without the model itself.
//
// `fistOffset` moves the hand in the camera's own frame -- right, up, forward.
// It exists because a hand holding a tool is not in the same place as a hand
// holding a block: a block is small and sits on the fingers, and a long thing
// held out at the same spot puts its back end through the player's face. The
// arm and whatever it holds move together, which is the point -- moving only
// the tool is what makes it look like it is floating next to the hand rather
// than in it.
blocky::Mat4 viewArmTransform(const Animator& animator, const Player& player,
                              blocky::IVec3 armDims, blocky::Vec3 fistOffset = {});

// The block in that hand, given the arm's transform. Built on top of it rather
// than beside it, so the two cannot drift: whatever the swing does to the arm
// happens to the block for free.
blocky::Mat4 viewHeldTransform(const Animator& animator, const blocky::Mat4& arm,
                               blocky::IVec3 armDims, blocky::IVec3 blockDims);

// A tool in that hand instead of a block, and not the same problem.
//
// A block is a cube: it has no ends, so it is held by its middle and turned to
// whatever angle looks best. A tool is held **by one particular voxel** and
// points **one particular way**, so this takes the grip rather than the size
// and lines the model's own -Z up with where the player is looking. `gripVoxel`
// is the voxel that lands in the fist.
//
// Built on the arm's transform for the same reason the block is: the swing and
// the walk carry it for free, and neither has to be told there is a tool.
blocky::Mat4 viewToolTransform(const Animator& animator, const blocky::Mat4& arm,
                               blocky::IVec3 armDims, blocky::Vec3 gripVoxel);

} // namespace game
