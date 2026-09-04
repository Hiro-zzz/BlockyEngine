#include "game/player.hpp"
#include "game/blocks.hpp"

#include "engine/physics/contact.hpp"
#include "engine/world/raycast.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

void aimPlayer(Player& player, const Window& window, float dt) {
    (void)dt;   // mouse movement is already a delta, not a rate

    Vec2 delta = window.mouseDelta();
    player.body.yawDegrees -= delta.x * player.mouseSensitivity;
    player.body.pitchDegrees -= delta.y * player.mouseSensitivity;

    // Just short of straight up and straight down. Exactly vertical makes the
    // forward vector parallel to the world up, and the camera basis stops
    // being defined.
    player.body.pitchDegrees = std::max(-89.5f, std::min(89.5f, player.body.pitchDegrees));
}

void stepPlayer(Player& player, const World& world, const Window& window, bool mayRun, float dt) {
    Vec3 forward = player.body.walkForward();
    Vec3 right = player.body.right();

    Vec3 wish{};
    if (window.keyDown(key::W)) wish += forward;
    if (window.keyDown(key::S)) wish -= forward;
    if (window.keyDown(key::D)) wish += right;
    if (window.keyDown(key::A)) wish -= right;

    bool jump = window.keyDown(key::Space);

    // The run key still dives in water whatever the stamina says: `mayRun`
    // is about sprinting, and underwater the same key means something else
    // entirely. Swimming has its own drain.
    bool run = window.keyDown(key::Shift) && (mayRun || player.body.swimming);

    stepCharacter(player.body, world, player.settings, wish, jump, run, dt);
}

void updateReach(Player& player, World& world, const Window& window, float dt) {
    player.edited = false;
    player.acted = false;

    if (player.mineCooldown > 0.0f) player.mineCooldown -= dt;
    if (!window.mouseLeftDown()) player.mineCooldown = 0.0f;

    Ray ray;
    ray.origin = player.body.eye(player.settings);
    ray.direction = player.body.forward();

    RayHit hit;
    RayFilter filter;
    player.looking = raycast(world, ray, player.reach, hit, filter);
    if (player.looking) {
        player.target = hit.block;
        player.targetNormal = hit.normal;
    }

    // Breaking is the block under the crosshair; placing is the empty cell in
    // front of the face that was hit. Using the face normal rather than the
    // last empty cell the ray passed through matters at a corner, where those
    // two differ and the wrong one puts the block behind you.
    //
    // The click is answered before the crosshair is consulted, so swinging at
    // thin air still swings.
    const bool mine = window.mouseLeftPressed() ||
                      (window.mouseLeftDown() && player.mineCooldown <= 0.0f);
    if (mine) {
        player.acted = true;
        player.mineCooldown = player.mineInterval;

        if (player.looking && world.collides(hit.block)) {
            world.set(hit.block, block::Air);
            player.edited = true;
        }
        return;
    }

    if (window.mouseRightPressed()) {
        player.acted = true;
        if (!player.looking) return;

        IVec3 target = hit.block + hit.normal;
        if (world.get(target) != block::Air) return;

        // Refuse to build inside yourself. Without this, aiming down and
        // clicking entombs the player, which is a bug report rather than a
        // feature.
        Vec3 feet = player.body.position;
        float half = player.settings.width * 0.5f;
        Vec3 lo{feet.x - half, feet.y, feet.z - half};
        Vec3 hi{feet.x + half, feet.y + player.settings.height, feet.z + half};

        bool insidePlayer = float(target.x) < hi.x && float(target.x + 1) > lo.x &&
                            float(target.y) < hi.y && float(target.y + 1) > lo.y &&
                            float(target.z) < hi.z && float(target.z + 1) > lo.z;
        if (insidePlayer) return;

        world.set(target, player.heldBlock());
        player.edited = true;
    }
}

} // namespace game
