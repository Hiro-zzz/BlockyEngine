#include "game/physgun.hpp"

#include "engine/physics/pick.hpp"
#include "engine/platform/window.hpp"
#include "engine/render/gl/overlay.hpp"
#include "engine/world/raycast.hpp"

#include "game/player.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

// ------------------------------------------------------------- the model
//
// Written as boxes rather than drawn, which is the same bargain the block
// textures take: no asset file, no importer, and the shape is readable in the
// place it is defined. Seven voxels across, sixteen tall, twenty-two long,
// with the muzzle at z = 0 because the model faces -Z like everything else.
//
// The silhouette is doing the work here. At the size a view model is drawn --
// a few hundred pixels of screen -- nobody reads a detail, they read an
// outline, and the outline of this is a claw with something burning in it.
// The rest is there so the claw has a reason to exist.
PhysgunModel buildPhysgunModel() {
    PhysgunModel gun;
    gun.voxels.resize({7, 16, 26});

    auto material = [&](Vec3 srgb, Vec3 emission = Vec3{0.0f}, float roughness = 0.75f,
                        float metallic = 0.0f) {
        VoxelMaterial m;
        m.albedo = srgbToLinear(srgb / 255.0f);
        m.emission = emission;
        m.roughness = roughness;
        m.metallic = metallic;
        return gun.voxels.addMaterial(m);
    };

    // Gunmetal, a lighter plate to catch the light, near-black rubber, and one
    // warm accent so the blue reads as *cold* rather than as the only colour
    // in the thing.
    const uint16_t casing = material({58, 63, 72}, Vec3{0.0f}, 0.6f, 0.25f);
    const uint16_t plate = material({126, 136, 149}, Vec3{0.0f}, 0.45f, 0.55f);
    const uint16_t dark = material({26, 28, 33}, Vec3{0.0f}, 0.8f);
    const uint16_t rubber = material({40, 42, 50}, Vec3{0.0f}, 0.95f);
    const uint16_t amber = material({216, 138, 44}, Vec3{0.08f, 0.03f, 0.0f}, 0.5f);

    // Emission above one on purpose: the viewport's bloom threshold sits at
    // 1.15 so that ordinary lit surfaces cannot reach it, and a core that
    // stops short of it is a pale blue block rather than a light.
    const uint16_t glow = material({150, 226, 255}, {0.9f, 3.4f, 5.0f}, 0.3f);
    const uint16_t ember = material({120, 198, 240}, {0.25f, 1.0f, 1.6f}, 0.4f);

    auto box = [&](int x0, int x1, int y0, int y1, int z0, int z1, uint16_t id) {
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) gun.voxels.set({x, y, z}, id);
    };

    // ------------------------------------------------------------- the claw
    // Three prongs round an open space, and the open space is the point: the
    // shape has to be legible held at arm's length in the corner of a screen,
    // which means a hole in the middle of it. Each prong bends inwards over
    // its last few voxels -- one voxel of offset, and the difference between
    // a claw and three sticks.
    for (int side = 0; side < 2; ++side) {
        const int x0 = side == 0 ? 0 : 5;
        box(x0, x0 + 1, 10, 12, 5, 9, casing);   // root
        box(x0, x0 + 1, 9, 11, 2, 4, casing);    // bent in
        box(x0, x0 + 1, 9, 10, 0, 1, plate);     // tip
        box(x0, x0 + 1, 10, 10, 6, 8, ember);    // a light down its length
    }
    box(2, 4, 2, 4, 5, 9, casing);
    box(2, 4, 3, 5, 2, 4, casing);
    box(2, 4, 4, 5, 0, 1, plate);
    box(3, 3, 3, 3, 6, 8, ember);

    // ------------------------------------------------------------- the core
    // The one thing on the model anybody actually looks at. A cube with its
    // corners knocked off reads as round at this size; a sphere would be the
    // same voxels and more code.
    box(2, 4, 6, 9, 2, 7, ember);
    box(2, 4, 7, 8, 3, 6, glow);
    box(3, 3, 6, 9, 3, 6, glow);

    // The neck the claw is bolted to. Thin on purpose: it is the gap between
    // this and the body that lets the claw read as a separate thing.
    box(2, 4, 6, 9, 10, 11, plate);
    box(3, 3, 7, 8, 8, 11, ember);

    // ------------------------------------------------------------- the body
    box(1, 5, 5, 11, 12, 21, casing);
    box(0, 0, 6, 10, 13, 20, plate);
    box(6, 6, 6, 10, 13, 20, plate);

    // Vents cut into the side plates, and a strip of light between them --
    // the give-away that the thing is powered rather than machined.
    for (int x : {0, 6}) {
        box(x, x, 7, 7, 14, 19, dark);
        box(x, x, 9, 9, 14, 19, dark);
        box(x, x, 8, 8, 14, 19, ember);
    }

    // A rail along the top with a warm strip at the front of it.
    box(2, 4, 12, 12, 13, 20, plate);
    box(3, 3, 13, 13, 14, 19, casing);
    box(2, 4, 12, 12, 12, 13, amber);

    // ------------------------------------------------------------- the back
    box(1, 5, 6, 10, 22, 24, casing);
    box(2, 4, 7, 9, 25, 25, plate);
    box(3, 3, 8, 8, 25, 25, glow);

    // ------------------------------------------------------------- the grip
    // Slanted: every two voxels down, one voxel back. A vertical grip reads as
    // a handle on a suitcase.
    box(2, 4, 4, 5, 18, 21, rubber);
    box(2, 4, 2, 3, 19, 22, rubber);
    box(2, 4, 0, 1, 20, 23, rubber);
    box(2, 4, 4, 4, 18, 18, plate);   // the collar the hand stops against

    // Trigger, and a guard in front of it. One voxel thick, because at this
    // scale two is a block with a hole in it.
    box(3, 3, 3, 4, 17, 17, dark);
    box(3, 3, 2, 2, 15, 17, dark);
    box(3, 3, 3, 4, 15, 15, dark);

    gun.gripVoxel = {3.5f, 4.0f, 20.5f};
    gun.muzzleVoxel = {3.5f, 7.5f, 1.5f};
    return gun;
}

namespace {

Vec3 clampLength(Vec3 v, float limit) {
    float len = length(v);
    return len > limit && len > kEps ? v * (limit / len) : v;
}

// The nearest body along the aim that is not behind a wall.
//
// Both queries have to be made and compared. Against bodies alone, a crate on
// the far side of a hill is grabbable through the hill, which nobody reads as
// reach -- they read it as the wall not being there.
bool aimedBody(const World& world, const PhysicsWorld& physics, Vec3 eye, Vec3 aim, float reach,
               BodyPick& pick) {
    if (!pickBody(physics, Ray{eye, aim}, reach, pick)) return false;

    RayHit block;
    if (raycast(world, Ray{eye, aim}, reach, block) && block.t < pick.distance) return false;
    return true;
}

}  // namespace

namespace {

// Takes hold of a body that is already in the world.
void takeHold(Physgun& gun, PhysicsWorld& physics, int index, Vec3 at, float distance) {
    RigidBody& body = physics.body(index);

    // Grabbing something frozen picks it up rather than refusing: the freeze
    // was a way of parking it, and having to unfreeze first would make the
    // tool argue with the last thing it was told.
    physics.setFrozen(index, false);
    body.wake();

    gun.held = index;
    gun.gripLocal = rotate(conjugate(body.orientation), at - body.position);
    gun.holdOrientation = body.orientation;
    gun.distance = std::clamp(distance, gun.settings.minDistance, gun.settings.maxDistance);
}

}  // namespace

bool grabPhysgun(Physgun& gun, const PhysgunReach& reach, Vec3 eye, Vec3 aim) {
    if (!reach.valid()) return false;
    World& world = *reach.world;
    PhysicsWorld& physics = *reach.physics;

    BodyPick pick;
    if (aimedBody(world, physics, eye, aim, gun.settings.reach, pick)) {
        takeHold(gun, physics, pick.body, pick.position, pick.distance);
        return true;
    }

    // Nothing loose under the crosshair. The block itself, then -- lifted out
    // of the lattice and stood up as a body in the same place, which is the
    // only way a thing that has to turn can stop being a cell.
    if (!reach.yard) return false;

    RayHit hit;
    if (!raycast(world, Ray{eye, aim}, gun.settings.reach, hit)) return false;
    if (!world.registry().isSolid(hit.id)) return false;

    const int body = reach.yard->liftBlock(physics, world, hit.block, reach.textures);
    if (body < 0) return false;

    // Held by where the ray met it, like anything else: a block taken by its
    // top face hangs from that face rather than snapping its centre to the
    // crosshair.
    takeHold(gun, physics, body, hit.position, hit.t);
    return true;
}

void aimPhysgun(Physgun& gun, const PhysgunReach& reach, const Window& window, Vec3 eye,
                Vec3 aim) {
    gun.aiming = false;
    gun.aimingAtBlock = false;
    if (!gun.equipped || !reach.valid()) {
        releasePhysgun(gun);
        return;
    }

    World& world = *reach.world;
    PhysicsWorld& physics = *reach.physics;

    // A grab that outlived what it was holding. The ragdoll budget can retire
    // a body while the physgun has hold of one of its arms, and an index is
    // reused -- so this is checked every frame rather than trusted.
    if (gun.holding() && !physics.alive(gun.held)) releasePhysgun(gun);

    // ------------------------------------------------------------ the grab
    if (window.mouseLeftPressed() && !gun.holding()) grabPhysgun(gun, reach, eye, aim);

    if (gun.holding() && !window.mouseLeftDown()) releasePhysgun(gun);

    // ------------------------------------------------------------ the wheel
    // Pushing and pulling what is held; with nothing held it does nothing,
    // rather than falling through to the hotbar. A tool in hand owns its
    // controls even when it is idle, or the player has to remember which of
    // two things the wheel is currently doing.
    float wheel = window.wheelDelta();
    if (wheel != 0.0f && gun.holding()) {
        gun.distance = std::clamp(gun.distance + wheel * gun.settings.scrollStep,
                                  gun.settings.minDistance, gun.settings.maxDistance);
    }

    // ----------------------------------------------------------- the freeze
    // On what is held if anything is, and otherwise on what is under the
    // crosshair -- so freezing a row of planks is aim-and-click rather than
    // grab, freeze, release, grab the next.
    if (window.mouseRightPressed()) {
        if (gun.holding()) {
            physics.setFrozen(gun.held, true);
            releasePhysgun(gun);
        } else {
            BodyPick pick;
            if (aimedBody(world, physics, eye, aim, gun.settings.reach, pick))
                physics.setFrozen(pick.body, true);
        }
    }

    // ------------------------------------------------------- what to draw
    // Only the hover half. Where the *held* thing is comes from the step, so
    // that a run with no window -- which never reaches this function -- still
    // has a beam to photograph.
    if (gun.holding()) return;

    BodyPick pick;
    if (aimedBody(world, physics, eye, aim, gun.settings.reach, pick)) {
        gun.aiming = true;
        gun.aimPoint = pick.position;
        return;
    }

    // A block counts as something to aim at, but says so separately: the
    // interface offers to LIFT rather than to GRAB, because a trigger pull
    // that quietly takes a wall apart should have announced itself first.
    if (!reach.yard) return;
    RayHit hit;
    if (raycast(world, Ray{eye, aim}, gun.settings.reach, hit) && world.registry().isSolid(hit.id)) {
        gun.aiming = true;
        gun.aimingAtBlock = true;
        gun.aimPoint = hit.position;
    }
}

void stepPhysgun(Physgun& gun, PhysicsWorld& physics, Vec3 eye, Vec3 aim, float dt) {
    if (!gun.holding() || dt <= 0.0f) return;
    if (!physics.alive(gun.held)) {
        releasePhysgun(gun);
        return;
    }

    RigidBody& body = physics.body(gun.held);
    if (body.isStatic()) return;
    body.wake();

    // Where the beam ends. Kept here rather than with the rest of the drawing
    // state because this is the one part of the tool that runs without a
    // window, and a snapshot has to have something to draw.
    gun.gripWorld = body.position + rotate(body.orientation, gun.gripLocal);
    gun.aiming = true;
    gun.aimPoint = gun.gripWorld;

    // ---------------------------------------------------------- where it goes
    // The target is where the *grip* should be, so the body's centre has to
    // be worked back from it through the orientation the servo is also
    // driving. Using the centre directly would make a plank held by one end
    // swing round to be held in the middle as soon as it was picked up.
    const Vec3 target = eye + aim * gun.distance;
    const Vec3 wantCentre = target - rotate(body.orientation, gun.gripLocal);

    body.linearVelocity =
        clampLength((wantCentre - body.position) * gun.settings.followRate, gun.settings.maxSpeed);

    // ---------------------------------------------------------- and how it sits
    // The rotation from where it is to where it should be, as an angular
    // velocity. Taking the short way round matters: without the flip a
    // quarter turn one way is solved as three quarters the other, and a
    // grabbed crate spins the long way for no reason anybody can see.
    Quat drift = normalize(gun.holdOrientation * conjugate(body.orientation));
    if (drift.w < 0.0f) drift = drift * -1.0f;

    Vec3 axis{drift.x, drift.y, drift.z};
    float sine = length(axis);
    if (sine > kEps) {
        float angle = 2.0f * std::atan2(sine, drift.w);
        body.angularVelocity = clampLength((axis / sine) * angle * gun.settings.turnRate,
                                           gun.settings.maxAngularSpeed);
    } else {
        body.angularVelocity = Vec3{};
    }
}

bool turnHeldObject(Physgun& gun, const Window& window, const Player& player) {
    if (!gun.equipped || !gun.holding() || !window.keyDown(key::E)) return false;

    Vec2 delta = window.mouseDelta();
    if (delta.x == 0.0f && delta.y == 0.0f) return true;   // still ours to eat

    // Sideways turns it about the world's up, and up-and-down about the axis
    // across the view. Not about the camera's own up and right both: yawing
    // about a tilted axis makes a crate that was level stop being level, and
    // levelling something is most of what turning it is for.
    const float scale = gun.settings.turnDegreesPerCount;
    Quat yaw = Quat::axisAngle({0.0f, 1.0f, 0.0f}, radians(-delta.x * scale));
    Quat pitch = Quat::axisAngle(player.body.right(), radians(-delta.y * scale));

    gun.holdOrientation = normalize(yaw * pitch * gun.holdOrientation);
    return true;
}

void releasePhysgun(Physgun& gun) {
    gun.held = -1;
    gun.gripLocal = Vec3{};
}

int unfreezeAll(PhysicsWorld& physics) {
    int count = 0;
    for (int index = 0; index < physics.bodyCount(); ++index) {
        if (!physics.alive(index) || !physics.body(index).frozen) continue;
        physics.setFrozen(index, false);
        ++count;
    }
    return count;
}

// ------------------------------------------------------------------ drawing
namespace {

// A world point on the screen, in overlay pixels. False when it is behind the
// camera, where the projection would put it on screen mirrored.
bool project(const Camera& camera, Vec3 point, int width, int height, Vec2& out) {
    Vec3 view = transformPoint(camera.viewMatrix(), point);
    if (view.z > -0.05f) return false;

    float f = 1.0f / std::tan(camera.fovY * 0.5f);
    float x = (f / camera.aspect) * view.x / -view.z;
    float y = f * view.y / -view.z;

    out = {(x * 0.5f + 0.5f) * float(width), (0.5f - y * 0.5f) * float(height)};
    return true;
}

}  // namespace

void drawPhysgun(const Physgun& gun, Overlay& overlay, const Camera& camera) {
    if (!gun.equipped) return;

    const float w = float(overlay.width());
    const float h = float(overlay.height());
    const float scale = std::max(1.0f, std::floor(h / 320.0f));

    // The tool's name is the hotbar's job now -- see `Hud::tool`. What is
    // left here is the part that has to be drawn in the world's coordinates
    // rather than the interface's.
    if (!gun.aiming) return;

    Vec2 end;
    if (!project(camera, gun.aimPoint, overlay.width(), overlay.height(), end)) return;

    // The muzzle, projected from the model that is actually being held -- so
    // the beam swings when the hand does, and starts at the tip of the claw
    // rather than at a corner that happens to be near it.
    //
    // The fallback is that corner, and it is not dead code: in third person
    // behind the player the muzzle can be off screen or behind the camera,
    // and a beam that vanishes because its start point did is worse than a
    // beam drawn from slightly the wrong place.
    Vec2 muzzle{w * 0.74f, h * 0.88f};
    if (gun.muzzleKnown) {
        Vec2 projected;
        if (project(camera, gun.muzzleWorld, overlay.width(), overlay.height(), projected))
            muzzle = projected;
    }

    // A line of marks rather than a line, because the overlay draws
    // axis-aligned rectangles and nothing else. Which turns out to be the
    // better look anyway: a beam made of segments reads as energy, and one
    // solid diagonal of stair-stepped pixels reads as a mistake.
    const int marks = 22;
    const Rgba colour = gun.holding() ? rgb8(120, 200, 255) : rgb8(120, 200, 255, 0.45f);

    for (int i = 1; i <= marks; ++i) {
        float t = float(i) / float(marks);
        float x = muzzle.x + (end.x - muzzle.x) * t;
        float y = muzzle.y + (end.y - muzzle.y) * t;

        // Thicker towards the far end, so the beam reads as going away from
        // the player rather than as a line drawn on the glass.
        float size = scale * (1.5f + 1.8f * t);
        Rgba fade = colour;
        fade.a *= 0.45f + 0.55f * t;

        // A dark mark under each, one pixel down and right. The same trick
        // the text uses and for the same reason: a pale blue line over a
        // sunlit plank floor is invisible, and the interface does not get to
        // depend on what the world happens to be doing behind it.
        Rgba shade = rgba(0.0f, 0.05f, 0.12f, fade.a * 0.7f);
        overlay.rect(x - size * 0.5f + scale, y - size * 0.5f + scale, size, size, shade);
        overlay.rect(x - size * 0.5f, y - size * 0.5f, size, size, fade);
    }

    // A box on what it has hold of.
    const float box = scale * (gun.holding() ? 7.0f : 5.0f);
    overlay.frame(end.x - box, end.y - box, box * 2.0f, box * 2.0f, scale, colour);
}

} // namespace game
