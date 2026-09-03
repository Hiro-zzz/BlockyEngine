#pragma once
// The step: gravity, contacts, a sequential-impulse solver, and sleeping.
//
// Everything here is ordinary rigid-body work, and deliberately so -- the
// parts of this engine that had to be invented are elsewhere. What is worth
// knowing is which of the ordinary parts a body needs in order to *stop*,
// because "falls" is easy and "comes to rest" is the whole difficulty:
//
//   - friction, or it slides for ever on a flat floor;
//   - a penetration slop, or position correction and gravity trade a
//     millimetre back and forth every step and the box hums;
//   - warm starting, or a stack spends its whole iteration budget every step
//     rediscovering the forces it had a moment ago, and sags;
//   - sleeping *by island*, or a box resting on a moving one falls asleep
//     under it.
//
// Take any one away and something never settles, which is why the tests
// assert on stillness and on stack height rather than on position alone.
#include "engine/physics/body.hpp"
#include "engine/physics/constraint.hpp"
#include "engine/physics/contact.hpp"
#include "engine/world/world.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace blocky {

struct PhysicsSettings {
    Vec3 gravity{0.0f, -9.81f, 0.0f};

    // Iterations of the velocity solver. Contacts are solved one at a time and
    // each one disturbs the others, so this is how many passes they get to
    // agree. Ten is plenty for a body on terrain; a stack wants more, and
    // gets most of the way there on warm starting instead.
    int iterations = 10;

    // Penetration this deep is left alone. Without a slop the solver chases
    // the last micrometre for ever, and every correction it applies is energy
    // that has to go somewhere.
    float penetrationSlop = 0.005f;

    // Fraction of the remaining penetration corrected per step, and a ceiling
    // on the resulting velocity. The ceiling matters when a body is spawned
    // inside geometry: without it, the first step fires it across the map.
    float baumgarte = 0.2f;
    float maxCorrectionSpeed = 1.0f;

    // Below this closing speed a collision does not bounce, whatever the
    // restitution says. Otherwise a resting body bounces on its own settling
    // velocity and never stops.
    float restitutionThreshold = 1.0f;

    // Seed each contact with the impulse the same contact carried last step,
    // instead of starting every step from zero.
    //
    // The iterations are a fixed budget, and a stack needs more of them than
    // it gets: each step it rediscovers roughly the impulse it had a moment
    // ago, and the part it does not finish discovering shows up as sag.
    // Handing it last step's answer as the starting guess costs one hash
    // lookup and one impulse.
    //
    // Left switchable because it is the kind of claim that should be measured
    // rather than asserted; `test_physics` builds a stack both ways.
    bool warmStart = true;

    float linearDamping = 0.01f;
    float angularDamping = 0.03f;

    // A body slower than both thresholds for this long stops being simulated
    // -- but only when every body it is touching is too. See the island note
    // at the top.
    float sleepLinear = 0.04f;
    float sleepAngular = 0.15f;
    float sleepSeconds = 0.4f;
};

struct StepStats {
    int contacts = 0;
    int brokenConstraints = 0;   // joints that gave way this step
    int bodyPairs = 0;      // pairs that survived the broad phase
    int awakeBodies = 0;
    int sleepingBodies = 0;
    int islands = 0;        // islands containing an awake body
    int warmStarted = 0;    // contacts recognised from the previous step
    double seconds = 0.0;
};

class PhysicsWorld {
public:
    PhysicsSettings settings;

    int add(const RigidBody& body);
    RigidBody&       body(int index)       { return bodies_[size_t(index)]; }
    const RigidBody& body(int index) const { return bodies_[size_t(index)]; }

    // Slots, live or not. Still the right bound for `index < bodyCount()`,
    // which is what every caller was using it for.
    int  bodyCount() const { return int(bodies_.size()); }

    // What the broad phase actually walks.
    int  liveBodyCount() const { return int(active_.size()); }

    bool alive(int index) const {
        return index >= 0 && index < int(bodies_.size()) && alive_[size_t(index)] != 0;
    }

    // Retires a body: it stops being simulated, every joint on it is dropped,
    // and its slot goes back to `add`.
    //
    // The slot really is reused, which makes an index the same kind of thing
    // a pointer is -- valid until the thing it names is removed, and quietly
    // naming something else afterwards. `alive` answers the first half; there
    // is nothing that can answer the second, so whoever holds indices (a
    // ragdoll, a physgun's grip, a script's spawned props) is expected to
    // forget them at the moment it removes them.
    //
    // Retiring slots *without* reuse was the other option and is the one that
    // does not scale: the broad phase is quadratic in the slot count, so a
    // sandbox left running would come to spend its whole frame stepping over
    // the dead. Which is also why removal empties the slot rather than only
    // flagging it -- an emptied `RigidBody` has infinite mass, and every loop
    // in the solver already skips those.
    void remove(int index);

    // Freezes a body in place, or lets it go again. See `RigidBody::freeze`
    // for what freezing is; this is the version to call, because unfreezing
    // also has to wake whatever had settled against it. Nothing else does:
    // islands are not joined through an immovable body, so the crate asleep
    // on top of a frozen plank has no way of hearing that the plank is a
    // falling object again.
    void setFrozen(int index, bool frozen);

    // Joints are added after the bodies they name. A joint on a removed body
    // is retired with it, and `removeConstraint` retires one by hand; a
    // *broken* joint is a different thing and stays in the list, because it
    // is something that happened rather than something that was undone.
    int addConstraint(const Constraint& joint);
    void removeConstraint(int index);
    Constraint&       constraint(int index)       { return constraints_[size_t(index)]; }
    const Constraint& constraint(int index) const { return constraints_[size_t(index)]; }
    int  constraintCount() const { return int(constraints_.size()); }

    void clear();

    // One step of exactly `dt`. The caller keeps the accumulator and calls
    // this a whole number of times: a solver fed a variable dt changes
    // stiffness with the framerate, so the same drop settles differently on a
    // fast machine -- which is also how it stops being reproducible.
    void step(const World& world, float dt);

    const StepStats& stats() const { return stats_; }

    // The contacts of the last step, for drawing or for a test to count.
    const std::vector<Contact>& contacts() const { return contacts_; }

private:
    struct CachedImpulse {
        float normal = 0.0f;
        float tangent[2]{0.0f, 0.0f};
    };

    // Whether an unbroken joint ties these two together.
    //
    // Jointed bodies do not collide with each other, and a ragdoll is the
    // reason. Its limbs are built touching -- an arm against a torso, a head
    // on a neck -- so the moment one lands, the contact solver is pushing two
    // bodies apart at exactly the point a joint is holding them together. The
    // two fight, the fight pumps energy in, and the character explodes across
    // the map. Every physics engine draws this exclusion for the same reason,
    // and a ragdoll is the case that makes it unavoidable rather than an
    // optimisation.
    //
    // A broken joint stops excluding, which is the behaviour to want: the
    // pieces of something that has come apart should hit each other.
    //
    // This used to be a linear scan of the joint list per candidate pair, and
    // said in a comment that a sandbox with thousands wanted a mask instead.
    // A ragdoll is five joints, so a dozen of them made the scan the largest
    // single cost in the step. The mask is `jointedPairs_`, rebuilt from
    // scratch at the top of every `collide` -- rebuilt rather than maintained
    // because a joint can *break* mid-step, and a set that had to be told
    // about that would be a second thing to keep in step with the first.
    bool jointed(int a, int b) const;

    // A body's identity for the impulse cache: its index, salted with how
    // many times the slot has been reused.
    //
    // Without the salt a new body inherits the warm-start history of whatever
    // died in its slot, and warm starting *applies* its impulse before the
    // first iteration -- so a ragdoll spawned where the last one was deleted
    // would be launched by a force that belonged to somebody else.
    uint64_t bodyKey(int index) const;

    void collide(const World& world);
    void wakeIslands();
    void solveVelocities(float dt);
    void solveConstraints(float dt, bool positionPass);
    void integrateAndSleep(float dt);

    int  islandRoot(int index);

    std::vector<RigidBody>  bodies_;
    std::vector<Constraint> constraints_;
    std::vector<Contact>    contacts_;
    std::vector<float>     targetNormalVelocity_;  // restitution, fixed per step

    // Which slots hold a body, which are free to be handed out again, and how
    // many times each has been reused. Parallel to `bodies_`.
    std::vector<uint8_t>  alive_;
    std::vector<uint32_t> salt_;
    std::vector<int>      freeBodies_;
    std::vector<int>      freeConstraints_;

    // The live bodies, and where each one sits in that list. The second is
    // what makes removal O(1): swap the last live body into the hole rather
    // than searching for the one being removed.
    std::vector<int> active_;
    std::vector<int> activeAt_;

    // Pairs an unbroken joint ties together, rebuilt each step. See jointed().
    std::unordered_set<uint64_t> jointedPairs_;

    // Broad-phase scratch and the union-find that groups touching bodies.
    std::vector<Vec3> boundsLo_, boundsHi_;
    std::vector<int>  island_;

    // Last step's impulses, keyed by the pair and the contact feature, and
    // the map being filled for the next one. Two maps swapped rather than one
    // edited in place, so a contact that stopped existing simply fails to be
    // copied forward instead of needing to be found and erased.
    std::unordered_map<uint64_t, CachedImpulse> previousImpulses_;
    std::unordered_map<uint64_t, CachedImpulse> impulses_;

    StepStats stats_;
};

} // namespace blocky
