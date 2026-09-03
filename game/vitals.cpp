#include "game/vitals.hpp"
#include "game/blocks.hpp"

#include "engine/world/block.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

bool maySprint(const Vitals& vitals, const Character& body, bool wantsToRun) {
    if (!wantsToRun) return false;
    if (body.swimming) return false;   // in water the run key means dive

    // Two thresholds, not one. Starting takes a real amount in the tank;
    // continuing only takes not being empty. With a single number the bar sits
    // exactly at it and the sprint stutters on and off every other frame.
    const float needed = vitals.sprinting ? vitals.settings.sprintStop : vitals.settings.sprintCost;
    return vitals.stamina > needed;
}

void stepVitals(Vitals& vitals, const Character& body, const CharacterSettings& settings,
                bool running, float dt) {
    if (dt <= 0.0f) return;
    const VitalSettings& s = vitals.settings;

    vitals.hurt = false;
    vitals.died = false;
    vitals.sinceDamage += dt;
    vitals.sinceSpend += dt;
    vitals.hurtFlash = std::max(0.0f, vitals.hurtFlash - dt * 2.2f);

    const float before = vitals.health;

    // ------------------------------------------------------------- falling
    // The edge, not the state: the one frame the ground arrives, judged on
    // the speed carried in from the frame before.
    if (body.onGround && !vitals.wasOnGround && vitals.lastFallSpeed > s.safeLandingSpeed &&
        !body.inWater) {
        // Water is the exception, and it needs no arithmetic of its own:
        // jumping off a cliff into a lake is the whole reason anybody looks
        // for one, and the controller has already capped the speed on the way
        // in to well under the threshold. The guard is here for the case of
        // reaching the *bed* of a shallow pool.
        vitals.health -= (vitals.lastFallSpeed - s.safeLandingSpeed) * s.fallDamagePerSpeed;
    }
    vitals.lastFallSpeed = body.onGround ? 0.0f : std::max(0.0f, -body.velocity.y);
    vitals.wasOnGround = body.onGround;

    // ------------------------------------------------------------ the lungs
    if (body.eyeFluid != block::Air) {
        // Lava does not let you hold your breath in it, but it is not what
        // kills you there either.
        vitals.breath -= dt / std::max(0.01f, s.breathSeconds);
        if (vitals.breath <= 0.0f) {
            vitals.breath = 0.0f;
            vitals.health -= s.drownDamagePerSecond * dt;
        }
    } else {
        vitals.breath = std::min(1.0f, vitals.breath + s.breathRefillRate * dt);
    }

    // ---------------------------------------------------------------- lava
    // Measured on submersion so a toe in it is not the same as falling in.
    if (body.fluid == block::Lava)
        vitals.health -= s.lavaDamagePerSecond * dt * std::max(0.25f, body.submersion);

    // ------------------------------------------------------------- stamina
    const float horizontal =
        std::sqrt(body.velocity.x * body.velocity.x + body.velocity.z * body.velocity.z);

    // Sprinting costs while it is *doing* something. Holding the run key
    // against a wall is not a sprint, and the gait already knows that -- the
    // legs stop when the ground stops going past.
    const bool sprintingNow = running && body.onGround && horizontal > settings.walkSpeed * 0.85f;
    vitals.sprinting = sprintingNow;

    float spend = 0.0f;
    if (sprintingNow) spend += s.sprintDrainPerSecond;
    if (body.swimming) spend += s.swimDrainPerSecond;

    if (spend > 0.0f) {
        vitals.stamina = std::max(0.0f, vitals.stamina - spend * dt);
        vitals.sinceSpend = 0.0f;
    } else if (vitals.sinceSpend > s.staminaRefillDelay) {
        vitals.stamina = std::min(s.maxStamina, vitals.stamina + s.staminaRefillPerSecond * dt);
    }

    // -------------------------------------------------------------- health
    if (vitals.health < before) {
        vitals.hurt = true;
        vitals.hurtFlash = 1.0f;
        vitals.sinceDamage = 0.0f;
    } else if (vitals.sinceDamage > s.regenDelay && vitals.health < s.maxHealth &&
               vitals.stamina > 0.0f) {
        // Paid for out of stamina, so somebody who has just run across a
        // valley does not also heal on the way. It is the only coupling
        // between the two bars and it is what stops them being two separate
        // decorations.
        const float wanted = std::min(s.regenPerSecond * dt, s.maxHealth - vitals.health);
        const float afford = s.regenStaminaCost > 0.0f ? vitals.stamina / s.regenStaminaCost : wanted;
        const float healed = std::min(wanted, afford);

        vitals.health += healed;
        vitals.stamina = std::max(0.0f, vitals.stamina - healed * s.regenStaminaCost);
    }

    // ---------------------------------------------------------------- void
    // Out of the bottom of the world, where nothing else can reach: the fall
    // has no landing, so it would otherwise go on for ever.
    if (body.position.y < s.voidY) {
        vitals.health = 0.0f;
        vitals.hurt = true;
    }

    if (vitals.health <= 0.0f) {
        vitals.health = 0.0f;
        vitals.died = true;
    }
}

void reviveVitals(Vitals& vitals) {
    vitals.health = vitals.settings.maxHealth;
    vitals.stamina = vitals.settings.maxStamina;
    vitals.breath = 1.0f;
    vitals.sinceDamage = 100.0f;
    vitals.sinceSpend = 100.0f;
    vitals.lastFallSpeed = 0.0f;
    vitals.wasOnGround = true;
    vitals.hurt = false;
    vitals.hurtFlash = 0.0f;
    vitals.died = false;
}

} // namespace game
