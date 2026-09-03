#pragma once
// What the player has left: health, stamina, breath.
//
// ---------------------------------------------------- why there is anything
//
// A health bar that cannot move is decoration, and a decoration that looks
// like a mechanic is worse than no bar at all -- it promises the world can
// hurt you and then does not deliver. So this file is mostly *sources*: three
// ways to lose health, one way to get it back, and one thing stamina is for.
// The bar came last.
//
// Every one of them is read off the character rather than invented here, and
// that is the point of putting it in `game/` above `engine/physics/`:
//
//   - **falling** is `Character::onGround` going true with speed behind it,
//     the same edge the animator already bends the knees on;
//   - **drowning** is `eyesUnderwater` for longer than a lungful;
//   - **lava** is `fluid`, which is why the controller reports *which* fluid
//     it is in rather than only that it is in one;
//   - **sprinting** costs stamina, and an empty bar means the run key stops
//     working -- which is a real rule about movement rather than a number
//     going down next to one.
//
// Nothing here writes to the character. The controller decides where the
// player is; this decides what it costs, and the one place they meet is the
// game asking "may I sprint" before it passes `run` down.
//
// ------------------------------------------------------------------- death
//
// There is no death animation and no game over: at zero you wake up at the
// spawn with everything full. A sandbox is a place to try things, and the
// punishment for a mistake is the walk back to where you were.
#include "engine/physics/character.hpp"

namespace game {

struct VitalSettings {
    float maxHealth = 20.0f;

    // ------------------------------------------------------------- falling
    // The speed a landing starts to hurt at, and what it costs above it.
    // Chosen from the jump: a jump lands at about 8.6 m/s, so anything you
    // can do to yourself on flat ground is free.
    float safeLandingSpeed = 13.0f;
    float fallDamagePerSpeed = 0.9f;    // health per m/s over the threshold

    // ----------------------------------------------------------- breathing
    float breathSeconds = 12.0f;        // a lungful
    float drownDamagePerSecond = 2.4f;
    float breathRefillRate = 4.0f;      // lungfuls per second at the surface

    // ---------------------------------------------------------------- lava
    // Not survivable, and not meant to be: a few seconds is enough to get out
    // if you were quick about it.
    float lavaDamagePerSecond = 7.0f;

    // ---------------------------------------------------------------- void
    // Below this there is no world left, and falling through the bottom of
    // one is the one accident a player cannot recover from on their own: the
    // fall never lands, so nothing else here ever fires.
    //
    // It became reachable the moment fluids stopped being solid -- a pool of
    // lava two blocks deep at the bottom of a pit is something you fall
    // *through* -- which is also why the map under it grew a floor. This is
    // the backstop for the next hole nobody thought of.
    float voidY = -64.0f;

    // ------------------------------------------------------------ stamina
    float maxStamina = 10.0f;
    float sprintDrainPerSecond = 1.35f;
    float swimDrainPerSecond = 0.55f;
    float staminaRefillPerSecond = 2.1f;
    float staminaRefillDelay = 0.8f;    // after the last thing that spent it

    // Hysteresis. Without a gap between "can start" and "must stop", a player
    // at the bottom of the bar sprints for one frame in every three and the
    // camera's field of view flickers.
    float sprintCost = 0.6f;            // must have this much to start
    float sprintStop = 0.05f;           // below this the run key goes quiet

    // -------------------------------------------------------------- health
    float regenPerSecond = 0.55f;
    float regenDelay = 6.0f;            // after the last damage

    // Regeneration is paid for out of stamina, so a player who has just
    // sprinted across a valley does not also heal on the way.
    float regenStaminaCost = 0.35f;     // stamina per health point
};

struct Vitals {
    VitalSettings settings;

    float health = 20.0f;
    float stamina = 10.0f;
    float breath = 1.0f;      // 0 to 1, a fraction of a lungful

    // Counts down from the last damage; regeneration waits for it.
    float sinceDamage = 100.0f;
    float sinceSpend = 100.0f;

    // Set for one frame when something hurt, so the interface can flash and
    // the host can decide what a hit sounds like.
    bool  hurt = false;
    float hurtFlash = 0.0f;   // 1 at the moment of the hit, fading

    // Set for one frame when health reached zero. The host respawns; nothing
    // here knows where the spawn is.
    bool  died = false;

    bool sprinting = false;   // what `maySprint` last allowed

    // ------------------------------------------------------- what was true
    // How fast the fall was going the frame before the ground arrived. The
    // controller zeroes the vertical velocity as it plants the feet, so by
    // the time `onGround` is true the number that mattered is gone -- the
    // same sample the animator keeps for the same reason.
    float lastFallSpeed = 0.0f;
    bool  wasOnGround = true;

    float healthFraction() const { return settings.maxHealth > 0.0f ? health / settings.maxHealth : 0.0f; }
    float staminaFraction() const { return settings.maxStamina > 0.0f ? stamina / settings.maxStamina : 0.0f; }
};

// Whether the run key means anything right now.
//
// Asked *before* the step, because the answer changes what the character is
// told to do. The alternative -- letting the character sprint and taking the
// stamina afterwards -- lets a player at zero sprint for one more step every
// step, which is the same as not having a limit.
bool maySprint(const Vitals& vitals, const blocky::Character& body, bool wantsToRun);

// One frame. Not one fixed step: none of this is a solver, and a health bar
// that ticks at 120 Hz and a screen that draws at 60 disagree about how long a
// second is by exactly nothing -- but the fall edge has to be sampled where
// the animator samples it, which is here.
void stepVitals(Vitals& vitals, const blocky::Character& body,
                const blocky::CharacterSettings& settings, bool running, float dt);

// Back to full. The host calls this after putting the player somewhere.
void reviveVitals(Vitals& vitals);

} // namespace game
