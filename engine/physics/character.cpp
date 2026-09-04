#include "engine/physics/character.hpp"

#include "engine/physics/contact.hpp"

#include <algorithm>
#include <cmath>

namespace blocky {
namespace {

// Clearance kept on every resolved face. Without it a box resolved exactly
// against a plane counts as overlapping it on the next test, and the character
// spends every step being pushed out of the floor it is standing on.
constexpr float kSkin = 1e-3f;

// A separate, much smaller epsilon for deciding which cells a box covers, and
// the two must not be the same number.
//
// Using the clearance here was a real bug: standing at a skin's height above
// the floor, a step of gravity moved the feet down by less than the skin, the
// cell range still began *above* the floor, the move was not blocked, and the
// character sank a third of a millimetre a step -- through the floor in about
// a minute, invisibly. The clearance says how far apart things are put; this
// says what counts as touching, and it has to be far finer.
constexpr float kProbe = 1e-5f;

struct Box {
    Vec3 lo, hi;
};

Box boxAt(const CharacterSettings& settings, Vec3 feet) {
    float half = settings.width * 0.5f;
    return {{feet.x - half, feet.y, feet.z - half},
            {feet.x + half, feet.y + settings.height, feet.z + half}};
}

bool overlapsSolid(const World& world, const Box& box) {
    IVec3 lo = floorToInt(box.lo + Vec3{kProbe});
    IVec3 hi = floorToInt(box.hi - Vec3{kProbe});

    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                if (world.collides({x, y, z})) return true;
    return false;
}

// How much of the box is in fluid, as a fraction of its height.
//
// Measured by layer rather than by cell, and that is the whole trick. What a
// swimmer cares about is *how deep* they are; whether the layer at that depth
// is water is a yes-or-no question about the layer. So a body standing half
// on the bank and half in the lake counts as being at the depth it is at,
// rather than at half of it -- which is right, because the water is over that
// much of them either way.
float submersionOf(const World& world, const CharacterSettings& settings, Vec3 feet,
                   BlockId* fluidOut = nullptr) {
    if (fluidOut) *fluidOut = block::Air;

    Box box = boxAt(settings, feet);
    float height = box.hi.y - box.lo.y;
    if (height <= kProbe) return 0.0f;

    IVec3 lo = floorToInt(box.lo + Vec3{kProbe});
    IVec3 hi = floorToInt(box.hi - Vec3{kProbe});

    float wet = 0.0f;
    for (int y = lo.y; y <= hi.y; ++y) {
        bool fluid = false;
        for (int z = lo.z; z <= hi.z && !fluid; ++z) {
            for (int x = lo.x; x <= hi.x && !fluid; ++x) {
                BlockId id = world.get({x, y, z});
                if (!world.registry().isFluid(id)) continue;
                fluid = true;

                // The lowest wet layer names the fluid: standing in lava with
                // your head in the steam above it is standing in lava.
                if (fluidOut && *fluidOut == block::Air) *fluidOut = id;
            }
        }
        if (!fluid) continue;

        // How much of this cell's y range the box actually occupies. The top
        // and bottom layers are partial, and ignoring that makes a body
        // standing in a puddle count as a tenth submerged when its toes are
        // wet.
        float top = std::min(box.hi.y, float(y + 1));
        float bottom = std::max(box.lo.y, float(y));
        wet += std::max(0.0f, top - bottom);
    }
    return saturate(wet / height);
}

// Moves along one axis and pushes back out of whatever it ran into.
//
// One axis at a time is what makes this simple enough to be right. Moved
// diagonally into a corner, a box has no single correct push-out direction;
// resolved per axis, each answer is unambiguous, and sliding along a wall
// falls out of it rather than being special-cased.
bool moveAxis(const World& world, const CharacterSettings& settings, Vec3& feet, int axis,
              float delta) {
    if (delta == 0.0f) return false;

    feet[axis] += delta;
    Box box = boxAt(settings, feet);

    IVec3 lo = floorToInt(box.lo + Vec3{kProbe});
    IVec3 hi = floorToInt(box.hi - Vec3{kProbe});

    bool blocked = false;
    float limit = feet[axis];

    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                IVec3 cell{x, y, z};
                if (!world.collides(cell)) continue;

                blocked = true;
                // The box's extent along this axis, measured from `feet`.
                float lower = axis == 1 ? 0.0f : -settings.width * 0.5f;
                float upper = axis == 1 ? settings.height : settings.width * 0.5f;

                if (delta > 0.0f) {
                    limit = std::min(limit, float(cell[axis]) - upper - kSkin);
                } else {
                    limit = std::max(limit, float(cell[axis] + 1) - lower + kSkin);
                }
            }
        }
    }

    if (blocked) feet[axis] = limit;
    return blocked;
}

}  // namespace

Vec3 Character::forward() const {
    float y = radians(yawDegrees), p = radians(pitchDegrees);
    return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

Vec3 Character::walkForward() const {
    float y = radians(yawDegrees);
    return {-std::sin(y), 0.0f, -std::cos(y)};
}

Vec3 Character::right() const {
    Vec3 f = walkForward();
    return {-f.z, 0.0f, f.x};
}

bool characterFits(const World& world, const CharacterSettings& settings, Vec3 position) {
    return !overlapsSolid(world, boxAt(settings, position));
}

float characterSubmersion(const World& world, const CharacterSettings& settings, Vec3 position) {
    return submersionOf(world, settings, position);
}

Vec3 dropToGround(const World& world, const CharacterSettings& settings, Vec3 position,
                  float maxFall) {
    // Rise out of anything solid first, then fall to the first floor. Doing it
    // in that order means a spawn point inside a hill comes out on top of the
    // hill rather than in the cave under it.
    Vec3 place = position;
    for (int i = 0; i < 256 && !characterFits(world, settings, place); ++i) place.y += 1.0f;

    for (float fallen = 0.0f; fallen < maxFall; fallen += 0.1f) {
        Vec3 below = place;
        below.y -= 0.1f;
        if (!characterFits(world, settings, below)) break;
        place = below;
    }
    return place;
}

void stepCharacter(Character& character, const World& world, const CharacterSettings& settings,
                   Vec3 wish, bool jump, bool run, float dt) {
    if (dt <= 0.0f) return;

    character.hitWall = false;
    character.hitCeiling = false;

    // ------------------------------------------------------------- the water
    // Measured once, before anything moves, so every force below is answering
    // the same question about the same position.
    const float sub = submersionOf(world, settings, character.position);

    character.submersion = sub;
    character.inWater = sub > 0.0f;

    // A ceiling on how fast water may be fallen through, tightening as the
    // body goes under.
    //
    // This started life as a one-off clamp on the step of entry and that was
    // wrong twice over. It never fired -- the flags are re-measured at the
    // end of every step, so by the time the next one asks "was I in water"
    // the answer is already "I am" -- and even working it would have been the
    // wrong shape: water resists a fast body every step, not only the first.
    // A cap that interpolates with submersion is both the fix and the honest
    // version, and it is what keeps a fifty-block fall from arriving at the
    // lake bed. See `waterEntrySpeed`.
    character.velocity.y = std::max(
        character.velocity.y, -lerp(settings.terminalSpeed, settings.waterEntrySpeed, sub));

    character.swimming =
        character.inWater && !character.onGround && sub >= settings.swimThreshold;

    // ------------------------------------------------------------ steering
    wish.y = 0.0f;
    float wishLength = length(wish);
    if (wishLength > 1.0f) wish = wish / wishLength;

    float target = lerp(run ? settings.runSpeed : settings.walkSpeed, settings.swimSpeed, sub);
    float authority =
        character.onGround ? 1.0f : lerp(settings.airControl, settings.swimControl, sub);

    Vec3 horizontal{character.velocity.x, 0.0f, character.velocity.z};
    if (wishLength > 1e-4f) {
        Vec3 wanted = wish * target;
        // Steering rather than force: a character is meant to feel driven, so
        // it converges on the asked-for velocity instead of accumulating.
        //
        // Which is also why the water's drag is not applied on this branch:
        // a drag fighting a steering term makes the achieved speed a ratio of
        // two rates rather than the number written down, and `swimSpeed`
        // would quietly mean something else.
        horizontal = lerp(horizontal, wanted, saturate(authority * dt * 18.0f));
    } else {
        float keep = character.onGround ? settings.groundFriction : settings.airFriction;
        horizontal = horizontal * std::pow(lerp(keep, settings.waterDrag, sub), dt);
    }

    character.velocity.x = horizontal.x;
    character.velocity.z = horizontal.z;

    // A jump is a jump whenever the feet have something to push off, in a
    // river as much as on a road. With nothing underfoot and water all round,
    // the same key means "up" instead -- which is the whole of swimming, and
    // needs no threshold to decide between the two. Pushing off the bottom of
    // a lake is an ordinary jump that the drag then takes most of.
    const bool jumped = jump && character.onGround;
    if (jumped) {
        character.velocity.y = settings.jumpSpeed;
        character.onGround = false;
    }

    // Sprinting is the one thing that means nothing in water, so the same key
    // dives instead of needing one of its own.
    const bool swimUp = jump && !jumped && character.inWater;
    const bool diving = run && !character.onGround && character.inWater;

    // Gravity, less whatever the water is holding up. At full submersion the
    // remainder is a twentieth of a g: the slow sink of somebody who has
    // stopped swimming.
    character.velocity.y -= settings.gravity * (1.0f - settings.buoyancy * sub) * dt;

    if (character.inWater) {
        if (swimUp || diving) {
            // A target speed, like the walk is, rather than a force pushing
            // against the drag: pressing up should climb at a rate you can
            // predict.
            //
            // Scaled by submersion *squared*, and the exponent is doing real
            // work. Linear, the lift still nearly matches its full value at
            // the waterline and a swimmer holding up rises until only their
            // ankles are wet -- floating like a duck rather than like a
            // person. Squared, the lift fades as the body leaves the water,
            // and the level it settles at is chest-deep.
            float want = (swimUp ? settings.swimUpSpeed : -settings.swimDownSpeed) * sub * sub;
            character.velocity.y = lerp(character.velocity.y, want, saturate(dt * 9.0f));
        }
        character.velocity.y *= std::pow(settings.waterDrag, dt * sub);
    }

    character.velocity.y = std::max(character.velocity.y, -settings.terminalSpeed);

    // ------------------------------------------------------------- moving
    Vec3 delta = character.velocity * dt;

    // Substeps, so that nothing crosses a block in one go. At the terminal
    // speed a single step would be half a metre; the sweep is exact within a
    // step but not across one, and this is where a fast fall would otherwise
    // pass through a floor.
    float longest = std::max({std::fabs(delta.x), std::fabs(delta.y), std::fabs(delta.z)});
    int steps = std::max(1, int(std::ceil(longest / 0.4f)));
    Vec3 slice = delta / float(steps);

    bool grounded = false;

    for (int i = 0; i < steps; ++i) {
        if (moveAxis(world, settings, character.position, 1, slice.y)) {
            if (slice.y <= 0.0f) grounded = true; else character.hitCeiling = true;
            character.velocity.y = 0.0f;
            slice.y = 0.0f;
        }

        // Horizontal, with a step up: when a move is blocked and there is
        // ground under the feet, try the same move from `stepHeight` higher
        // and drop back down. This is what lets a staircase be walked rather
        // than jumped, and it is tried *after* the flat move fails so level
        // ground never pays for it.
        for (int axis : {0, 2}) {
            float amount = axis == 0 ? slice.x : slice.z;
            if (amount == 0.0f) continue;

            Vec3 before = character.position;
            if (!moveAxis(world, settings, character.position, axis, amount)) continue;

            bool stepped = false;
            // In water as well as on ground, and that is how a swimmer gets
            // out of a lake. Floating at the surface there is nothing under
            // the feet, so without this the bank is a wall: you bob against
            // it pressing forward and never climb it.
            if (character.onGround || grounded || character.inWater) {
                Vec3 raised = before;
                raised.y += settings.stepHeight;

                if (characterFits(world, settings, raised)) {
                    Vec3 trial = raised;
                    if (!moveAxis(world, settings, trial, axis, amount)) {
                        // Settle back onto whatever is now underfoot.
                        float drop = settings.stepHeight;
                        moveAxis(world, settings, trial, 1, -drop);
                        if (characterFits(world, settings, trial)) {
                            character.position = trial;
                            stepped = true;
                            grounded = true;
                        }
                    }
                }
            }
            if (!stepped) {
                character.hitWall = true;
                if (axis == 0) character.velocity.x = 0.0f; else character.velocity.z = 0.0f;
            }
        }
    }

    // Standing on something is asked separately rather than inferred from the
    // last move: a character that walked off a ledge this step is not on the
    // ground, and one that was pushed up by a step is.
    Vec3 probe = character.position;
    probe.y -= kSkin * 4.0f;
    character.onGround = grounded || !characterFits(world, settings, probe);
    if (character.onGround && character.velocity.y < 0.0f) character.velocity.y = 0.0f;

    // Re-measured after the move, because these are reported rather than
    // used: they have to describe where the character *is*, not where it was
    // when the forces were chosen. A frame that draws the pose from the first
    // answer is a frame late, and on entering water that is the frame the
    // splash happens in.
    character.submersion = submersionOf(world, settings, character.position, &character.fluid);
    character.inWater = character.submersion > 0.0f;
    character.swimming = character.inWater && !character.onGround &&
                         character.submersion >= settings.swimThreshold;

    const BlockId atEye = world.get(floorToInt(character.eye(settings)));
    character.eyeFluid = world.registry().isFluid(atEye) ? atEye : BlockId(block::Air);
    character.eyesUnderwater = character.eyeFluid != block::Air;
}

} // namespace blocky
