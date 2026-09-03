#include "game/animator.hpp"

#include "game/player.hpp"

#include "engine/anim/ease.hpp"
#include "engine/world/raycast.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

namespace {

// Exponential approach, framerate independent. `1 - exp(-rate * dt)` rather
// than `rate * dt` because the second one changes how fast it converges when
// the framerate changes -- which is the same mistake as feeding a solver a
// variable step, one floor down.
float approach(float current, float target, float rate, float dt) {
    return current + (target - current) * (1.0f - std::exp(-rate * dt));
}

float wrapDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

// The shape of one swing: up fast, down slower. Symmetric would read as a
// wave rather than as a strike -- the force is on the way down, and the way
// down is the half that should take longer to watch.
float swingLift(float t) {
    const float peak = 0.32f;
    if (t <= peak) return ease::out(t / peak);
    return 1.0f - ease::in((t - peak) / (1.0f - peak));
}

float horizontalSpeed(Vec3 velocity) {
    return std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
}

} // namespace

void startSwing(Animator& animator) {
    animator.swing = 0.0f;
    animator.swinging = true;
}

void animate(Animator& animator, const Player& player, float dt) {
    const AnimatorSettings& s = animator.settings;
    if (dt < 0.0f) dt = 0.0f;
    dt = std::min(dt, 0.1f);   // a stall must not launch the spring

    const Character& body = player.body;

    if (!animator.started) {
        animator.previousFeetY = body.position.y;
        animator.bodyYawDegrees = body.yawDegrees;
        animator.wasOnGround = body.onGround;
        animator.airBlend = body.onGround ? 0.0f : 1.0f;
        animator.started = true;
    }

    animator.clock += dt;

    const float speed = horizontalSpeed(body.velocity);

    // ------------------------------------------------------------- the gait
    // Distance, not time. Stand against a wall holding W and the legs stop,
    // because the ground stopped going past.
    if (body.onGround && s.blocksPerStride > 0.0f) {
        animator.stridePhase += (speed * dt) * (kTwoPi / s.blocksPerStride);
        if (animator.stridePhase > kTwoPi)
            animator.stridePhase -= kTwoPi * std::floor(animator.stridePhase / kTwoPi);
    }

    const float walk = std::max(0.1f, player.settings.walkSpeed);
    const float run = std::max(walk + 0.1f, player.settings.runSpeed);

    animator.speedBlend = approach(animator.speedBlend, saturate(speed / run), s.speedBlendRate, dt);
    animator.runBlend =
        approach(animator.runBlend, saturate((speed - walk * 0.92f) / (run - walk * 0.92f)),
                 s.speedBlendRate, dt);
    animator.airBlend = approach(animator.airBlend, body.onGround ? 0.0f : 1.0f, 10.0f, dt);

    // ----------------------------------------------------------- the swing
    if (animator.swinging && s.swingSeconds > 0.0f) {
        animator.swing += dt / s.swingSeconds;
        if (animator.swing >= 1.0f) {
            animator.swing = 0.0f;
            animator.swinging = false;
        }
    }

    // ------------------------------------------------------- stepping up
    // `stepHeight` is 1.05, so walking onto a block lifts the feet a whole
    // block between one step and the next. Letting the eye fall behind and
    // catch up is the entire difference between a step and a teleport.
    const float feetY = body.position.y;
    const float rise = feetY - animator.previousFeetY;
    if (body.onGround && animator.wasOnGround && rise > 0.001f &&
        rise <= player.settings.stepHeight + 0.02f) {
        animator.stepLag -= rise;
    }
    animator.stepLag = std::max(-player.settings.stepHeight, std::min(0.0f, animator.stepLag));
    animator.stepLag = approach(animator.stepLag, 0.0f, s.stepCatchUpRate, dt);

    // --------------------------------------------------------- the landing
    // The controller zeroes the vertical velocity as it plants the feet, so
    // the speed that mattered is the one from the frame before. Sampled while
    // still in the air, used on the frame the ground arrives.
    if (body.onGround && !animator.wasOnGround) {
        const float impact = animator.fallSpeed;
        if (impact > s.landMinSpeed) {
            const float dip =
                std::min(s.landDipMax, (impact - s.landMinSpeed) * s.landDipPerSpeed);

            // A critically damped spring kicked with velocity v peaks at
            // v / (omega * e). Solving that for the dip we asked for is why
            // the kick carries an e in it, rather than a number found by
            // trying some.
            animator.landVelocity -= dip * s.landStiffness * 2.71828f;
        }
    }
    animator.fallSpeed = body.onGround ? 0.0f : std::max(0.0f, -body.velocity.y);

    const float omega = s.landStiffness;
    animator.landVelocity +=
        (-2.0f * omega * animator.landVelocity - omega * omega * animator.landDip) * dt;
    animator.landDip += animator.landVelocity * dt;

    // ------------------------------------------------------- sprint field
    animator.fovBoost = approach(animator.fovBoost, s.sprintFovDegrees * animator.runBlend,
                                 s.fovRate, dt);

    // --------------------------------------------------- body follows head
    // Moving, the shoulders go where the feet go. Standing, the head turns
    // alone until it runs out of neck, and then the body comes round after it.
    const float lead = wrapDegrees(body.yawDegrees - animator.bodyYawDegrees);
    if (speed > 0.15f) {
        animator.bodyYawDegrees += lead * (1.0f - std::exp(-s.bodyYawRate * dt));
    } else if (std::fabs(lead) > s.bodyYawSlack) {
        animator.bodyYawDegrees += lead - std::copysign(s.bodyYawSlack, lead);
    }
    animator.bodyYawDegrees = wrapDegrees(animator.bodyYawDegrees);

    animator.previousFeetY = feetY;
    animator.wasOnGround = body.onGround;
}

Pose playerPose(const Animator& animator, const Player& player, bool firstPerson) {
    const AnimatorSettings& s = animator.settings;
    Pose pose;

    // ---------------------------------------------------------- walk cycle
    // The same contralateral arrangement `Pose::striding` uses: a positive
    // rotation about X carries a limb forward, and a limb hangs below its
    // pivot, so the right arm takes the negative of what the right leg takes.
    const float amplitude =
        lerp(s.strideDegreesWalk, s.strideDegreesRun, animator.runBlend) * animator.speedBlend;
    const float legAngle = std::sin(animator.stridePhase) * amplitude;

    // Arms swing a little shorter than legs. Equal amplitudes read as a
    // marching toy: on a real gait the arms are along for the ride.
    const float armAngle = legAngle * 0.72f;

    Vec3 rightLeg{legAngle, 0.0f, 0.0f};
    Vec3 leftLeg{-legAngle, 0.0f, 0.0f};
    Vec3 rightArm{-armAngle, 0.0f, 0.0f};
    Vec3 leftArm{armAngle, 0.0f, 0.0f};

    // ------------------------------------------------------------ airborne
    // Legs split and the arms come up and out: the silhouette of somebody
    // who has left the ground, blended in rather than switched to.
    if (animator.airBlend > 0.001f) {
        const float t = animator.airBlend;
        rightLeg = lerp(rightLeg, Vec3{-16.0f, 0.0f, 4.0f}, t);
        leftLeg = lerp(leftLeg, Vec3{22.0f, 0.0f, -4.0f}, t);
        rightArm = lerp(rightArm, Vec3{-30.0f, 0.0f, 11.0f}, t);
        leftArm = lerp(leftArm, Vec3{-30.0f, 0.0f, -11.0f}, t);
    }

    // ---------------------------------------------------------- idle sway
    // Breathing, essentially. Only visible when everything else has stopped,
    // which is exactly when a completely motionless character stops reading
    // as a character and starts reading as a prop.
    const float idle = (1.0f - animator.speedBlend) * (1.0f - animator.airBlend);
    if (idle > 0.001f) {
        const float breath = std::sin(animator.clock * 1.5f) * idle;
        rightArm.z += 2.2f + breath * 1.4f;
        leftArm.z -= 2.2f + breath * 1.4f;
        rightArm.x += breath * 1.1f;
        leftArm.x += breath * 1.1f;
    }

    // -------------------------------------------------------------- swing
    if (animator.swinging) {
        const float lift = swingLift(animator.swing);

        // Shorter in first person, and that is geometry rather than taste. The
        // shoulder is a quarter of a block from the eye, so an arm brought all
        // the way up to level passes within centimetres of the camera and
        // stops reading as an arm at all. Swinging to about half that keeps
        // the hand in the lower corner where it can be seen and understood.
        rightArm.x += lift * (firstPerson ? 34.0f : 64.0f);
        rightArm.z -= lift * (firstPerson ? 8.0f : 15.0f);
    }

    pose[joint::RightLeg].rotationDegrees = rightLeg;
    pose[joint::LeftLeg].rotationDegrees = leftLeg;
    pose[joint::RightArm].rotationDegrees = rightArm;
    pose[joint::LeftArm].rotationDegrees = leftArm;

    // ---------------------------------------------------------------- head
    // A part that rises above its pivot turns the other way round from a limb
    // that hangs below one, so looking up -- a positive pitch -- is a positive
    // rotation about X here. The trap is written up in docs/conventions.md.
    const float lead = wrapDegrees(player.body.yawDegrees - animator.bodyYawDegrees);
    pose[joint::Head].rotationDegrees = {std::max(-62.0f, std::min(62.0f, player.body.pitchDegrees)),
                                         lead, 0.0f};

    return pose;
}

Vec3 animatedEye(const Animator& animator, const Player& player) {
    const AnimatorSettings& s = animator.settings;
    const Character& body = player.body;

    Vec3 eye = body.eye(player.settings);
    eye.y += animator.stepLag + animator.landDip;

    // The bob is grounded motion. In the air there is nothing to push off, so
    // there is nothing to rock the head.
    const float amount = animator.speedBlend * (1.0f - animator.airBlend);
    if (amount > 0.001f) {
        eye.y -= std::fabs(std::cos(animator.stridePhase)) * s.bobVertical * amount;
        eye += body.right() * (std::sin(animator.stridePhase) * s.bobLateral * amount);
    }
    return eye;
}

void aimEyes(Animator& animator, const Player& player, Vec3 target, float dt) {
    const Character& body = player.body;

    // ------------------------------------------------------------- blinking
    // Scheduled, not sampled. A coin flipped every frame gives a blink rate
    // that rises with the framerate, and the character develops a twitch on a
    // fast machine.
    animator.nextBlink -= dt;
    if (!animator.blinking && animator.nextBlink <= 0.0f) {
        animator.blinking = true;
        animator.blink = 0.0f;
    }
    if (animator.blinking) {
        // A tenth of a second, closed and open again. Real blinks are about
        // that, and anything slower reads as somebody falling asleep.
        animator.blink += dt / 0.11f;
        if (animator.blink >= 2.0f) {
            animator.blinking = false;
            animator.blink = 0.0f;

            // Between two and six seconds, from the animator's own clock so a
            // snapshot run blinks in the same places every time.
            const float r = std::fabs(std::sin(animator.clock * 12.9898f) * 43758.5453f);
            animator.nextBlink = 2.0f + (r - std::floor(r)) * 4.0f;
        }
    }

    // ---------------------------------------------------------- the glance
    // Where the target is, in the head's own frame. Yaw comes off the body
    // rather than the feet because the head has already turned part of the
    // way there by itself -- asking the eyes to cover the whole angle again
    // would double it.
    const Vec3 head = body.eye(player.settings);
    Vec3 toTarget = target - head;

    float wantX = 0.0f, wantY = 0.0f;
    if (lengthSq(toTarget) > 1e-4f) {
        toTarget = normalize(toTarget);

        const float yaw = radians(animator.bodyYawDegrees);
        const Vec3 facing{-std::sin(yaw), 0.0f, -std::cos(yaw)};
        const Vec3 right{-facing.z, 0.0f, facing.x};

        const float ahead = dot(toTarget, facing);
        const float across = dot(toTarget, right);

        // Behind the character there is nothing to look at, and an eye that
        // tries reads as a broken one. The pupils centre instead.
        if (ahead > 0.15f) {
            // Positive `x` is the entity's own right, and its right hand is at
            // +X -- the same axis `right` was built on, so no sign to flip.
            wantX = std::max(-1.0f, std::min(1.0f, across / ahead * 1.6f));
            wantY = std::max(-1.0f, std::min(1.0f, toTarget.y / std::max(ahead, 0.2f) * 1.2f));
        }
    }

    animator.gazeX = approach(animator.gazeX, wantX, 9.0f, dt);
    animator.gazeY = approach(animator.gazeY, wantY, 9.0f, dt);
}

void poseEyes(const Animator& animator, const face::EyeRig& rig, Pose& pose) {
    if (!rig.built) return;

    face::gaze(rig, pose, animator.gazeX, animator.gazeY);

    if (!animator.blinking) return;

    // A blink, with the tools this rig has.
    //
    // The eye is a flat plate on its own joint, and `JointPose` carries a
    // rotation, an offset and a **uniform** scale. No lid can be drawn and the
    // plate cannot be squashed, so a blink is the plate going away: sunk
    // straight back into the head until the face closes over it.
    //
    // Turning it edge-on was the first attempt and it is wrong, for a reason
    // worth keeping. The five faces of an eye box that are not its front are
    // clothed in one flat colour -- the shade the engine found around the eye,
    // meant to be glimpsed at an angle and never looked at. Rotating the plate
    // makes that colour the whole eye, so a blink shows whatever the skin
    // happens to have beside the eyes. On a character with a fringe that is
    // hair, and the blink flashes a band of hair colour across the face.
    //
    // Sinking shows the head's own front instead, which is the face.
    const float t = animator.blink <= 1.0f ? animator.blink : 2.0f - animator.blink;
    const float shut = ease::smooth(std::max(0.0f, std::min(1.0f, t)));

    for (int joint : {rig.right, rig.left}) {
        if (joint < 0) continue;

        // +Z is into the head: the model looks towards -Z, so the eye stands
        // proud of the face in -Z and retreats the other way. Far enough to
        // clear its own relief and its depth, with a little to spare.
        pose[joint].offset.z += shut * 1.1f;
    }
}

Mat4 viewArmTransform(const Animator& animator, const Player& player, IVec3 armDims,
                      Vec3 fistOffset) {
    const AnimatorSettings& s = animator.settings;

    // The camera's own frame: -Z ahead, +X right, +Y up. Yaw then pitch, in
    // the same order and with the same signs `entityToWorld` uses for yaw --
    // which is what makes the arm agree with the view instead of only nearly
    // agreeing with it.
    const Mat4 head = translate(animatedEye(animator, player)) *
                      rotateAxis({0.0f, 1.0f, 0.0f}, radians(player.body.yawDegrees)) *
                      rotateAxis({1.0f, 0.0f, 0.0f}, radians(player.body.pitchDegrees));

    const float lift = animator.swinging ? swingLift(animator.swing) : 0.0f;

    // The fist, in that frame. The swing throws it up and forward; the walk
    // rocks it, out of phase with the step so the hand trails the body.
    const float bob = animator.speedBlend * (1.0f - animator.airBlend);
    // The hand stays in its corner through the swing. What travels is the
    // *angle*: an arm that leaves the corner reads as a thrown object rather
    // than as a strike, because there is no shoulder on screen to hinge it.
    Vec3 fist{s.viewArmFist.x + fistOffset.x - lift * s.viewArmSwingLift * 0.35f +
                  std::sin(animator.stridePhase) * 0.012f * bob,
              s.viewArmFist.y + fistOffset.y - lift * s.viewArmSwingLift * 0.30f -
                  std::fabs(std::cos(animator.stridePhase)) * 0.016f * bob,
              -s.viewArmFist.z - fistOffset.z - lift * s.viewArmSwingLift};

    // Angles that put the shoulder behind, below and outboard of the fist, so
    // the arm runs from the bottom-right corner towards the middle of the
    // screen. The swing rolls it up towards level and sweeps it inwards.
    const float pitchBack = 99.0f - lift * 13.0f;
    const float spread = -17.5f + lift * 22.0f;

    const Mat4 orient = rotateAxis({1.0f, 0.0f, 0.0f}, radians(pitchBack)) *
                        rotateAxis({0.0f, 0.0f, 1.0f}, radians(spread));

    // Anchor the model on its fist -- the middle of the bottom face -- so
    // every number above is about where the hand is, which is the only part
    // anybody looks at.
    const Vec3 anchor{float(armDims.x) * 0.5f, 0.0f, float(armDims.z) * 0.5f};

    return head * translate(fist) * orient * scale(Vec3{s.viewArmVoxel}) * translate(anchor * -1.0f);
}

Mat4 viewHeldTransform(const Animator& animator, const Mat4& arm, IVec3 armDims,
                       IVec3 blockDims) {
    // Expressed in the arm's own voxel space, so the swing carries it without
    // either of them being told about the other.
    const Vec3 inHand{float(armDims.x) * 0.5f, -1.6f, float(armDims.z) * 0.5f};

    // `arm` already scales voxels into world units, so this only says how
    // much bigger a block voxel is than an arm voxel.
    const float relative = animator.settings.viewBlockSize / animator.settings.viewArmVoxel;
    const Vec3 centre{float(blockDims.x) * 0.5f, float(blockDims.y) * 0.5f,
                      float(blockDims.z) * 0.5f};

    return arm * translate(inHand) * rotateAxis({0.0f, 1.0f, 0.0f}, radians(-28.0f)) *
           rotateAxis({1.0f, 0.0f, 0.0f}, radians(14.0f)) * scale(Vec3{relative}) *
           translate(centre * -1.0f);
}

Mat4 viewToolTransform(const Animator& animator, const Mat4& arm, IVec3 armDims, Vec3 gripVoxel) {
    // The grip lands **on the fist**, in the arm's own voxel space, a voxel up
    // into the hand rather than balanced on the fingertips. Anywhere else and
    // the tool reads as floating beside the hand -- which it did, an inch and
    // a half out, until this was the fist point and nothing else.
    //
    // Where the pair *sits on screen* is the arm's business, through
    // `fistOffset`, so that moving it moves both.
    const Vec3 inHand{float(armDims.x) * 0.5f, 1.2f, float(armDims.z) * 0.5f};

    const float relative = animator.settings.viewToolVoxel / animator.settings.viewArmVoxel;

    // The arm points along its own -Y: the shoulder is at the top of the model
    // and the fist at the bottom, and `viewArmTransform` lays that down the
    // view axis. A quarter turn about X therefore takes the tool's -Z, which
    // is where it faces, onto the arm's -Y, which is where the arm points --
    // and the tool's +Y onto the arm's -Z, which is up on screen.
    //
    // Then two small angles, and the first one is **not** the cant you get.
    // The arm carries its own spread -- `viewArmTransform` runs it from the
    // bottom-right corner towards the middle of the screen -- and that is
    // another seventeen degrees the tool inherits. Written as thirty-eight
    // here, the muzzle came out fifty-five degrees off the view axis, which
    // is not a weapon held at an angle, it is a weapon held sideways. Eight
    // gives the twenty-five a held thing wants: enough to see that it has a
    // side, not enough to stop it pointing where the player is looking.
    const Mat4 orient = rotateAxis({1.0f, 0.0f, 0.0f}, radians(-90.0f)) *
                        rotateAxis({0.0f, 1.0f, 0.0f}, radians(8.0f)) *
                        rotateAxis({1.0f, 0.0f, 0.0f}, radians(-6.0f));

    return arm * translate(inHand) * orient * scale(Vec3{relative}) * translate(gripVoxel * -1.0f);
}

Camera playerCamera(const Animator& animator, const Player& player, const World& world,
                    float fovDegrees, float aspect) {
    const AnimatorSettings& s = animator.settings;
    const Character& body = player.body;

    Camera camera;
    camera.projection = Camera::Projection::Perspective;
    camera.fovY = radians(fovDegrees + animator.fovBoost);
    camera.aspect = aspect;

    const Vec3 eye = animatedEye(animator, player);
    const Vec3 forward = body.forward();

    // Roll the horizon with the step. A degree of it is under the threshold
    // where anyone can point at what changed, and well over the one where
    // they can tell it is gone.
    Vec3 up{0.0f, 1.0f, 0.0f};
    const float amount = animator.speedBlend * (1.0f - animator.airBlend);
    if (amount > 0.001f) {
        const float roll = std::sin(animator.stridePhase) * s.bobRollDegrees * amount;
        up = normalize(transformDir(rotateAxis(forward, radians(roll)), up));
    }

    if (player.view == Player::View::First) {
        camera.lookAt(eye, eye + forward, up);
        return camera;
    }

    // Third person: back off along the view ray, or ahead of it, and stop
    // short of whatever is there. Without the cast the camera reverses through
    // the wall and the player is looking at the inside of the world.
    //
    // The front view pulls out along +forward and then looks back down it, so
    // the character is between the camera and where they are aiming. It is not
    // the back view mirrored: mirroring would also mirror the aim, and the
    // crosshair would stop meaning anything.
    const bool front = player.view == Player::View::ThirdFront;
    const Vec3 away = front ? forward : forward * -1.0f;

    float distance = s.thirdPersonDistance;

    Ray probe;
    probe.origin = eye;
    probe.direction = away;

    RayHit hit;
    RayFilter filter;
    if (raycast(world, probe, distance + s.thirdPersonHeadroom, hit, filter))
        distance = std::max(0.0f, hit.t - s.thirdPersonHeadroom);

    const Vec3 position = eye + away * distance;
    camera.lookAt(position, position - away * 1.0f, up);
    return camera;
}

} // namespace game
