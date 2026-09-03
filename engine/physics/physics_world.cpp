#include "engine/physics/physics_world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace blocky {
namespace {

// Effective mass of a contact row for one body: how much velocity one unit of
// impulse buys along `axis`, counting the rotation it also causes. A pair sums
// the two, and the static world contributes nothing.
float effectiveMass(const RigidBody& body, const Mat3& invInertia, Vec3 r, Vec3 axis) {
    Vec3 rXa = cross(r, axis);
    Vec3 angular = cross(invInertia * rXa, r);
    return body.invMass + dot(axis, angular);
}

// The contact's name, made unique across pairs. Two bodies resting on the
// same block would otherwise share a feature id and steal each other's
// impulse history. The two ids are salted slots rather than plain indices --
// see `bodyKey`.
uint64_t impulseKey(uint64_t bodyA, uint64_t bodyB, uint64_t feature) {
    uint64_t h = feature ^ (bodyA * 0x9e3779b97f4a7c15ull);
    h ^= (bodyB + 1) * 0xc2b2ae3d27d4eb4full;
    return h ^ (h >> 29);
}

// An unordered pair of body indices, as one number.
uint64_t pairKey(int a, int b) {
    uint32_t lo = uint32_t(std::min(a, b));
    uint32_t hi = uint32_t(std::max(a, b));
    return (uint64_t(hi) << 32) | lo;
}

bool boundsOverlap(Vec3 aLo, Vec3 aHi, Vec3 bLo, Vec3 bHi) {
    return aLo.x <= bHi.x && aHi.x >= bLo.x &&
           aLo.y <= bHi.y && aHi.y >= bLo.y &&
           aLo.z <= bHi.z && aHi.z >= bLo.z;
}

}  // namespace

// ------------------------------------------------------------- the body list
uint64_t PhysicsWorld::bodyKey(int index) const {
    // Zero is reserved for the static world, so slots start at one: without
    // that, body zero and the world share an id and steal each other's
    // impulses in the one arrangement that happens on every map.
    if (index < 0) return 0;
    return uint64_t(uint32_t(index) + 1u) | (uint64_t(salt_[size_t(index)]) << 32);
}

int PhysicsWorld::add(const RigidBody& body) {
    int index;
    if (!freeBodies_.empty()) {
        index = freeBodies_.back();
        freeBodies_.pop_back();
        bodies_[size_t(index)] = body;
    } else {
        index = int(bodies_.size());
        bodies_.push_back(body);
        alive_.push_back(0);
        salt_.push_back(0);
        activeAt_.push_back(-1);
    }

    alive_[size_t(index)] = 1;
    activeAt_[size_t(index)] = int(active_.size());
    active_.push_back(index);
    return index;
}

void PhysicsWorld::remove(int index) {
    if (!alive(index)) return;

    // Whatever it was holding up has to be told, and there are two ways it
    // could have been: a joint, or simply resting on it.
    //
    // Sleeping is what makes this necessary. A crate hanging from a weld
    // settles and stops being simulated, and nothing in a step ever asks
    // whether the thing it is hanging from still exists -- so removing the
    // anchor leaves it asleep in mid-air for ever. The same rule as the
    // hydraulic that changed length and moved nothing: an optimisation that
    // swallows an instruction is a bug, and deleting something is an
    // instruction.
    Vec3 lo{}, hi{};
    bodyBounds(bodies_[size_t(index)], lo, hi);

    for (int i = 0; i < int(constraints_.size()); ++i) {
        const Constraint& joint = constraints_[size_t(i)];
        if (joint.bodyA != index && joint.bodyB != index) continue;

        int other = joint.bodyA == index ? joint.bodyB : joint.bodyA;
        if (other >= 0 && alive(other)) bodies_[size_t(other)].wake();

        // A joint to a body that is not there any more is not a *broken*
        // joint -- nothing gave way -- and leaving it in the list naming a
        // slot that will be handed out again is how a new crate ends up
        // welded to the ghost of an old one.
        removeConstraint(i);
    }

    const float margin = 0.05f;
    lo = lo - Vec3{margin};
    hi = hi + Vec3{margin};

    for (int other : active_) {
        if (other == index) continue;
        RigidBody& neighbour = bodies_[size_t(other)];
        if (neighbour.isStatic() || !neighbour.sleeping) continue;

        Vec3 otherLo{}, otherHi{};
        bodyBounds(neighbour, otherLo, otherHi);
        if (boundsOverlap(lo, hi, otherLo, otherHi)) neighbour.wake();
    }

    // Out of the live list, by swapping the last one into the hole.
    const int at = activeAt_[size_t(index)];
    const int last = active_.back();
    active_[size_t(at)] = last;
    activeAt_[size_t(last)] = at;
    active_.pop_back();

    alive_[size_t(index)] = 0;
    activeAt_[size_t(index)] = -1;
    ++salt_[size_t(index)];

    // Emptied, not merely flagged: a default body has infinite mass, and
    // every loop in the step already skips those. So the linear passes need
    // no new condition and cannot forget one.
    bodies_[size_t(index)] = RigidBody{};
    freeBodies_.push_back(index);
}

void PhysicsWorld::setFrozen(int index, bool frozen) {
    if (!alive(index)) return;

    RigidBody& body = bodies_[size_t(index)];
    if (frozen) {
        body.freeze();
        return;
    }
    if (!body.frozen) return;

    // Everything it was holding up has to be told, and bounds are the only
    // thing available before the next `collide` -- contacts against a frozen
    // body are generated, but the island it is in is deliberately not shared.
    Vec3 lo{}, hi{};
    bodyBounds(body, lo, hi);
    body.unfreeze();

    const float margin = 0.05f;
    lo = lo - Vec3{margin};
    hi = hi + Vec3{margin};

    for (int other : active_) {
        if (other == index) continue;
        RigidBody& neighbour = bodies_[size_t(other)];
        if (neighbour.isStatic() || !neighbour.sleeping) continue;

        Vec3 otherLo{}, otherHi{};
        bodyBounds(neighbour, otherLo, otherHi);
        if (boundsOverlap(lo, hi, otherLo, otherHi)) neighbour.wake();
    }
}

int PhysicsWorld::addConstraint(const Constraint& joint) {
    if (!freeConstraints_.empty()) {
        int index = freeConstraints_.back();
        freeConstraints_.pop_back();
        constraints_[size_t(index)] = joint;
        return index;
    }
    constraints_.push_back(joint);
    return int(constraints_.size()) - 1;
}

void PhysicsWorld::removeConstraint(int index) {
    if (index < 0 || index >= int(constraints_.size())) return;

    Constraint& joint = constraints_[size_t(index)];
    if (joint.bodyA < 0 && joint.broken) return;   // already retired

    joint = Constraint{};
    joint.bodyA = -1;
    joint.bodyB = -1;
    joint.broken = true;   // so anything walking the list unaware skips it
    freeConstraints_.push_back(index);
}

// ---------------------------------------------------------------- joint rows
//
// Every joint below is built from two kinds of row and nothing else: a linear
// row, which pushes a pair of anchor points along some direction, and an
// angular row, which turns the two bodies about some axis without moving
// them. A ball socket is three linear rows; a weld is those plus three
// angular; a hinge swaps the three angular for two.
//
// The point constraints are solved as three sequential scalar rows along the
// world axes rather than as one three-by-three block. The block solve is
// exact in a single pass and this is not, but it needs a matrix inverse per
// joint per iteration, and the iterations are already there. Where it shows
// is a joint under load at an odd angle taking a few more iterations to stop
// creeping -- worth knowing if a contraption ever sags along one axis only.
void PhysicsWorld::solveConstraints(float dt, bool positionPass) {
    for (Constraint& joint : constraints_) {
        if (joint.broken) continue;
        if (joint.bodyA < 0 || joint.bodyA >= int(bodies_.size())) continue;

        RigidBody& a = bodies_[size_t(joint.bodyA)];
        RigidBody* b = joint.bodyB >= 0 ? &bodies_[size_t(joint.bodyB)] : nullptr;
        if (a.sleeping && (!b || b->sleeping)) continue;

        Vec3 pA = a.position + rotate(a.orientation, joint.anchorA);
        Vec3 pB = b ? b->position + rotate(b->orientation, joint.anchorB) : joint.anchorB;

        Mat3 invIA = a.invInertiaWorld();
        Mat3 invIB = b ? b->invInertiaWorld() : Mat3{};
        Vec3 rA = pA - a.position;
        Vec3 rB = b ? pB - b->position : Vec3{};

        auto linearMass = [&](Vec3 axis) {
            float k = a.invMass + dot(axis, cross(invIA * cross(rA, axis), rA));
            if (b) k += b->invMass + dot(axis, cross(invIB * cross(rB, axis), rB));
            return k;
        };
        auto angularMass = [&](Vec3 axis) {
            float k = dot(axis, invIA * axis);
            if (b) k += dot(axis, invIB * axis);
            return k;
        };
        auto linearVelocity = [&](Vec3 axis) {
            Vec3 v = positionPass ? a.pseudoVelocityAt(pA) : a.velocityAt(pA);
            if (b) v = v - (positionPass ? b->pseudoVelocityAt(pB) : b->velocityAt(pB));
            return dot(v, axis);
        };
        auto angularVelocityAlong = [&](Vec3 axis) {
            Vec3 w = positionPass ? a.pseudoAngular : a.angularVelocity;
            if (b) w = w - (positionPass ? b->pseudoAngular : b->angularVelocity);
            return dot(w, axis);
        };
        auto applyLinear = [&](Vec3 impulse) {
            if (positionPass) {
                a.applyPseudoImpulse(impulse, pA);
                if (b) b->applyPseudoImpulse(-impulse, pB);
            } else {
                a.applyImpulse(impulse, pA);
                if (b) b->applyImpulse(-impulse, pB);
            }
        };
        auto applyAngular = [&](Vec3 impulse) {
            if (positionPass) {
                a.applyPseudoAngularImpulse(impulse);
                if (b) b->applyPseudoAngularImpulse(-impulse);
            } else {
                a.applyAngularImpulse(impulse);
                if (b) b->applyAngularImpulse(-impulse);
            }
        };
        auto bias = [&](float error) {
            float speed = settings.baumgarte * error / dt;
            return std::clamp(speed, -settings.maxCorrectionSpeed, settings.maxCorrectionSpeed);
        };

        if (joint.kind == ConstraintKind::Distance) {
            Vec3 d = pA - pB;
            float len = length(d);
            if (len < kEps) continue;
            Vec3 n = d / len;
            float error = len - joint.distance;

            // A slack rope is not a constraint at all, and must also drop its
            // stored impulse -- otherwise the next time it goes taut it warm
            // starts from a force that belonged to a different moment.
            if (joint.rope && error < 0.0f) {
                joint.distanceImpulse = 0.0f;
                joint.pseudoDistanceImpulse = 0.0f;
                continue;
            }

            float k = linearMass(n);
            if (k <= kEps) continue;

            float target = positionPass ? -bias(error) : 0.0f;
            float lambda = (target - linearVelocity(n)) / k;

            float& accumulated = positionPass ? joint.pseudoDistanceImpulse : joint.distanceImpulse;
            float previous = accumulated;
            accumulated += lambda;
            if (joint.rope) accumulated = std::min(0.0f, accumulated);  // may only pull
            applyLinear(n * (accumulated - previous));

            // Damping along the line, which is what makes a hydraulic feel
            // like one instead of a spring that rings.
            if (!positionPass && joint.friction > 0.0f) {
                float damped = -linearVelocity(n) / k;
                float before = joint.frictionImpulse.x;
                joint.frictionImpulse.x = std::clamp(before + damped, -joint.friction, joint.friction);
                applyLinear(n * (joint.frictionImpulse.x - before));
            }
            continue;
        }

        // Everything else holds the two anchor points together.
        Vec3 error = pA - pB;
        const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int i = 0; i < 3; ++i) {
            float k = linearMass(axes[i]);
            if (k <= kEps) continue;

            float target = positionPass ? -bias(error[i]) : 0.0f;
            float lambda = (target - linearVelocity(axes[i])) / k;

            Vec3& accumulated = positionPass ? joint.pseudoLinearImpulse : joint.linearImpulse;
            accumulated[i] += lambda;
            applyLinear(axes[i] * lambda);
        }

        if (joint.kind == ConstraintKind::Weld) {
            // The rotation that would take A from where it should be to where
            // it is. For a small angle the vector part of a quaternion is half
            // the rotation vector, hence the two.
            Quat relative = b ? normalize(conjugate(b->orientation) * a.orientation) : a.orientation;
            Quat drift = relative * conjugate(joint.rest);
            if (drift.w < 0.0f) drift = drift * -1.0f;   // the short way round

            Vec3 driftLocal = Vec3{drift.x, drift.y, drift.z} * 2.0f;
            Vec3 driftWorld = b ? rotate(b->orientation, driftLocal) : driftLocal;

            for (int i = 0; i < 3; ++i) {
                float k = angularMass(axes[i]);
                if (k <= kEps) continue;

                float target = positionPass ? -bias(dot(driftWorld, axes[i])) : 0.0f;
                float lambda = (target - angularVelocityAlong(axes[i])) / k;

                Vec3& accumulated = positionPass ? joint.pseudoAngularImpulse : joint.angularImpulse;
                accumulated[i] += lambda;
                applyAngular(axes[i] * lambda);
            }
        } else if (joint.kind == ConstraintKind::Hinge) {
            // Two rows, both perpendicular to the axle: they stop the axes
            // tilting away from each other and leave the spin about them
            // alone. `cross` of the two axes is the rotation that would bring
            // A's onto B's, to first order.
            Vec3 axisAworld = rotate(a.orientation, joint.axisA);
            Vec3 axisBworld = b ? rotate(b->orientation, joint.axisB) : joint.axisB;
            Vec3 misalignment = cross(axisAworld, axisBworld);

            Vec3 t1, t2;
            orthonormalBasis(normalize(axisBworld), t1, t2);
            const Vec3 rows[2] = {t1, t2};

            for (int i = 0; i < 2; ++i) {
                float k = angularMass(rows[i]);
                if (k <= kEps) continue;

                float target = positionPass ? bias(dot(misalignment, rows[i])) : 0.0f;
                float lambda = (target - angularVelocityAlong(rows[i])) / k;

                Vec3& accumulated = positionPass ? joint.pseudoAngularImpulse : joint.angularImpulse;
                accumulated = accumulated + rows[i] * lambda;
                applyAngular(rows[i] * lambda);
            }

            // The axle itself: driven, braked, or merely resisted. Velocity
            // pass only -- neither of these corrects a positional error, and
            // running them in the position pass would let a motor drive the
            // joint through the correction rather than through the world.
            if (!positionPass && (joint.motor || joint.friction > 0.0f)) {
                Vec3 axle = normalize(axisBworld);
                float k = angularMass(axle);
                if (k > kEps) {
                    if (joint.motor) {
                        // A motor asks for a relative speed and is allowed a
                        // bounded impulse to get it. At speed zero it is a
                        // brake, which is why friction stands aside for it.
                        float lambda = (joint.motorSpeed - angularVelocityAlong(axle)) / k;
                        float before = joint.motorImpulse;
                        joint.motorImpulse =
                            std::clamp(before + lambda, -joint.motorTorque, joint.motorTorque);
                        applyAngular(axle * (joint.motorImpulse - before));
                    } else {
                        float lambda = -angularVelocityAlong(axle) / k;
                        float before = joint.frictionImpulse.x;
                        joint.frictionImpulse.x =
                            std::clamp(before + lambda, -joint.friction, joint.friction);
                        applyAngular(axle * (joint.frictionImpulse.x - before));
                    }
                }
            }
        } else if (joint.kind == ConstraintKind::BallSocket) {
            // ------------------------------------------------------ limits
            // How far the joint may bend, and how far it may spin. Both are
            // one-sided rows: inside the limit there is no constraint at all,
            // and past it the row may push back but never pull.
            if (joint.coneLimitDegrees > 0.0f || joint.twistLimitDegrees > 0.0f) {
                // Where the joint is now, measured from where it was made.
                Quat relative =
                    b ? normalize(conjugate(b->orientation) * a.orientation) : a.orientation;
                Quat drift = normalize(relative * conjugate(joint.rest));
                if (drift.w < 0.0f) drift = drift * -1.0f;   // the short way round

                // Swing-twist decomposition about the joint's own axis. The
                // swing is the part that tilts the limb away from where it
                // points; the twist is the part that spins it about itself.
                // They want different limits -- a shoulder swings most of a
                // half circle and twists barely at all -- and one angle
                // cannot say both.
                Vec3 axis = normalize(joint.axisB);
                Vec3 v{drift.x, drift.y, drift.z};
                float along = dot(v, axis);
                Vec3 projected = axis * along;

                Quat twist = normalize(Quat{projected.x, projected.y, projected.z, drift.w});
                Quat swing = normalize(drift * conjugate(twist));
                if (swing.w < 0.0f) swing = swing * -1.0f;

                // `rowAxis` is the direction turning *further* past the
                // limit, so the row is allowed to apply a negative impulse
                // and nothing else.
                auto limitRow = [&](Vec3 rowAxis, float excess, float& store, float& pseudoStore) {
                    if (excess <= 0.0f) return;
                    Vec3 world = b ? rotate(b->orientation, rowAxis) : rowAxis;

                    float k = angularMass(world);
                    if (k <= kEps) return;

                    float target = positionPass ? -bias(excess) : 0.0f;
                    float lambda = (target - angularVelocityAlong(world)) / k;

                    float& accumulated = positionPass ? pseudoStore : store;
                    float previous = accumulated;
                    accumulated = std::min(0.0f, previous + lambda);
                    applyAngular(world * (accumulated - previous));
                };

                if (joint.coneLimitDegrees > 0.0f) {
                    Vec3 swingVector{swing.x, swing.y, swing.z};
                    float sine = length(swingVector);
                    if (sine > kEps) {
                        float angle = 2.0f * std::atan2(sine, swing.w);
                        limitRow(swingVector / sine, angle - radians(joint.coneLimitDegrees),
                                 joint.coneImpulse, joint.pseudoConeImpulse);
                    }
                }

                if (joint.twistLimitDegrees > 0.0f) {
                    // Signed, and the limit is symmetric, so the row goes
                    // along whichever way it has turned.
                    float angle = 2.0f * std::atan2(along, drift.w);
                    float excess = std::fabs(angle) - radians(joint.twistLimitDegrees);
                    limitRow(angle >= 0.0f ? axis : axis * -1.0f, excess, joint.twistImpulse,
                             joint.pseudoTwistImpulse);
                }
            }

            // ---------------------------------------------------- friction
            // Nothing about a ball socket's rotation is *held*, so its
            // friction is on all three axes at once. It runs inside the
            // limits rather than instead of them: past the limit the row
            // above stops the joint opening, and this still resists it
            // sliding round the cone.
            if (!positionPass && joint.friction > 0.0f) {
                for (int i = 0; i < 3; ++i) {
                    float k = angularMass(axes[i]);
                    if (k <= kEps) continue;

                    float lambda = -angularVelocityAlong(axes[i]) / k;
                    float before = joint.frictionImpulse[i];
                    joint.frictionImpulse[i] =
                        std::clamp(before + lambda, -joint.friction, joint.friction);
                    applyAngular(axes[i] * (joint.frictionImpulse[i] - before));
                }
            }
        }
    }
}

void PhysicsWorld::clear() {
    bodies_.clear();
    constraints_.clear();
    contacts_.clear();
    targetNormalVelocity_.clear();
    alive_.clear();
    salt_.clear();
    freeBodies_.clear();
    freeConstraints_.clear();
    active_.clear();
    activeAt_.clear();
    jointedPairs_.clear();
    boundsLo_.clear();
    boundsHi_.clear();
    island_.clear();
    previousImpulses_.clear();
    impulses_.clear();
    stats_ = StepStats{};
}

int PhysicsWorld::islandRoot(int index) {
    while (island_[size_t(index)] != index) {
        island_[size_t(index)] = island_[size_t(island_[size_t(index)])];  // halve the path
        index = island_[size_t(index)];
    }
    return index;
}

// ---------------------------------------------------------------- collision
bool PhysicsWorld::jointed(int a, int b) const {
    return jointedPairs_.find(pairKey(a, b)) != jointedPairs_.end();
}

void PhysicsWorld::collide(const World& world) {
    contacts_.clear();

    // The exclusion mask for this step. Rebuilt rather than maintained: a
    // joint that breaks during a step has to stop excluding on the next one,
    // and the list is a handful of entries next to a quadratic pair loop.
    jointedPairs_.clear();
    for (const Constraint& joint : constraints_) {
        if (joint.broken || joint.bodyA < 0 || joint.bodyB < 0) continue;
        jointedPairs_.insert(pairKey(joint.bodyA, joint.bodyB));
    }

    boundsLo_.assign(bodies_.size(), Vec3{});
    boundsHi_.assign(bodies_.size(), Vec3{});
    for (int index : active_)
        bodyBounds(bodies_[size_t(index)], boundsLo_[size_t(index)], boundsHi_[size_t(index)]);

    // Against the world, for anything awake. A sleeping body cannot be moved
    // by the ground, so there is nothing to compute.
    for (int index : active_) {
        const RigidBody& body = bodies_[size_t(index)];
        if (body.isStatic() || body.sleeping) continue;

        size_t before = contacts_.size();
        collideBodyWithWorld(body, world, contacts_);
        for (size_t k = before; k < contacts_.size(); ++k) {
            contacts_[k].bodyA = index;
            contacts_[k].bodyB = -1;
        }
    }

    // Against each other. A pair is worth testing when at least one of them
    // is awake -- which is also how a sleeping body finds out that something
    // landed on it, since the contact is what puts them in one island.
    //
    // The broad phase is every *live* pair, rejected on bounds. That is
    // quadratic and honest at this scale; a sandbox with hundreds of props
    // wants sweep and prune or a refitted tree, and this is the seam it goes
    // behind. Walking `active_` rather than the slots is what keeps deleting
    // things from costing anything afterwards.
    for (size_t ia = 0; ia < active_.size(); ++ia) {
        const int i = active_[ia];
        const RigidBody& a = bodies_[size_t(i)];

        // Immovable counts as still, and that is the whole condition: a pair
        // is worth testing when at least one of the two could move as a
        // result. Static bodies used to be skipped outright, which was
        // invisible while nothing was ever static and wrong the moment
        // something could be *frozen* -- freezing a plank is how a wall gets
        // built, and a plank nothing can rest on is not a building material.
        const bool aStill = a.isStatic() || a.sleeping;

        for (size_t ib = ia + 1; ib < active_.size(); ++ib) {
            const int j = active_[ib];
            const RigidBody& b = bodies_[size_t(j)];
            if (aStill && (b.isStatic() || b.sleeping)) continue;
            if (!boundsOverlap(boundsLo_[size_t(i)], boundsHi_[size_t(i)], boundsLo_[size_t(j)],
                               boundsHi_[size_t(j)]))
                continue;
            if (jointed(i, j)) continue;

            ++stats_.bodyPairs;
            size_t before = contacts_.size();
            collideBodies(a, b, contacts_);
            for (size_t k = before; k < contacts_.size(); ++k) {
                contacts_[k].bodyA = i;
                contacts_[k].bodyB = j;
            }
        }
    }
}

// ------------------------------------------------------------------ islands
void PhysicsWorld::wakeIslands() {
    size_t count = bodies_.size();

    // A joint that is *driving*, or that is no longer holding the length it
    // was told to hold, has to wake what it is attached to.
    //
    // Without this a hydraulic on a settled contraption does nothing at all:
    // the bodies went to sleep while it was at rest, the solver skips
    // sleeping pairs, and the script driving the piston watches a number
    // change with no effect in the world. Sleeping is an optimisation, and an
    // optimisation that swallows an instruction is a bug.
    for (Constraint& joint : constraints_) {
        if (joint.broken || joint.bodyA < 0 || size_t(joint.bodyA) >= count) continue;

        bool demands = joint.motor && joint.motorSpeed != 0.0f;

        if (!demands && joint.kind == ConstraintKind::Distance) {
            const RigidBody& a = bodies_[size_t(joint.bodyA)];
            const RigidBody* b = joint.bodyB >= 0 ? &bodies_[size_t(joint.bodyB)] : nullptr;

            Vec3 pA = a.position + rotate(a.orientation, joint.anchorA);
            Vec3 pB = b ? b->position + rotate(b->orientation, joint.anchorB) : joint.anchorB;
            float error = length(pA - pB) - joint.distance;

            // A slack rope is doing its job and demands nothing; a rope only
            // has something to say when it has gone taut, a rod either way.
            demands = joint.rope ? error > settings.penetrationSlop
                                 : std::fabs(error) > settings.penetrationSlop;
        }

        if (!demands) continue;
        bodies_[size_t(joint.bodyA)].wake();
        if (joint.bodyB >= 0 && size_t(joint.bodyB) < count) bodies_[size_t(joint.bodyB)].wake();
    }

    island_.resize(count);
    std::iota(island_.begin(), island_.end(), 0);

    // Only dynamic pairs join an island. Uniting through a static body would
    // put everything resting on the same floor into one group, and then a
    // single moving crate would keep the whole map awake.
    for (const Contact& contact : contacts_) {
        if (contact.bodyB < 0) continue;
        // Nor through a frozen one, for the same reason: everything stacked
        // on the same frozen platform would be one island, and one crate
        // being pushed at the far end would keep the lot awake.
        if (bodies_[size_t(contact.bodyA)].isStatic() ||
            bodies_[size_t(contact.bodyB)].isStatic())
            continue;

        int rootA = islandRoot(contact.bodyA);
        int rootB = islandRoot(contact.bodyB);
        if (rootA != rootB) island_[size_t(rootA)] = rootB;
    }

    // A joint binds its two bodies as firmly as a contact does, and more
    // permanently: welded props must sleep and wake as one thing, or half a
    // contraption stops being simulated while the other half swings.
    for (const Constraint& joint : constraints_) {
        if (joint.broken || joint.bodyB < 0) continue;
        if (bodies_[size_t(joint.bodyA)].isStatic() || bodies_[size_t(joint.bodyB)].isStatic())
            continue;
        int rootA = islandRoot(joint.bodyA);
        int rootB = islandRoot(joint.bodyB);
        if (rootA != rootB) island_[size_t(rootA)] = rootB;
    }

    std::vector<uint8_t> hasAwake(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (bodies_[i].isStatic() || bodies_[i].sleeping) continue;
        hasAwake[size_t(islandRoot(int(i)))] = 1;
    }

    for (size_t i = 0; i < count; ++i) {
        if (bodies_[i].isStatic() || !bodies_[i].sleeping) continue;
        if (hasAwake[size_t(islandRoot(int(i)))]) bodies_[i].wake();
    }

    for (size_t i = 0; i < count; ++i)
        if (hasAwake[i]) ++stats_.islands;
}

// -------------------------------------------------------------------- step
void PhysicsWorld::step(const World& world, float dt) {
    auto start = std::chrono::steady_clock::now();
    stats_ = StepStats{};
    if (dt <= 0.0f) return;

    // Collision runs first, on last step's positions, because waking depends
    // on who is touching whom: a body that is about to be woken has to have
    // its contacts already in hand, and it has to receive gravity in the same
    // step it wakes rather than a step later.
    collide(world);
    wakeIslands();

    // Joints start each step from zero rather than from last step's force.
    // Contacts are warm started because they come and go by the dozen and a
    // stack needs the history; a joint is a handful of rows that persist and
    // gets enough iterations without it. Measured on the test contraption the
    // difference was not visible -- worth revisiting for a large one.
    for (Constraint& joint : constraints_) {
        joint.linearImpulse = Vec3{};
        joint.angularImpulse = Vec3{};
        joint.distanceImpulse = 0.0f;
        joint.pseudoLinearImpulse = Vec3{};
        joint.pseudoAngularImpulse = Vec3{};
        joint.pseudoDistanceImpulse = 0.0f;
        joint.frictionImpulse = Vec3{};
        joint.motorImpulse = 0.0f;
        joint.coneImpulse = joint.twistImpulse = 0.0f;
        joint.pseudoConeImpulse = joint.pseudoTwistImpulse = 0.0f;
    }

    for (RigidBody& body : bodies_) {
        if (body.isStatic() || body.sleeping) continue;

        body.linearVelocity = body.linearVelocity + settings.gravity * dt;

        // Damping as a per-step decay rather than a force: it is a fudge for
        // air, and expressing it as a force would only make it a fudge with
        // a mass in it.
        body.linearVelocity = body.linearVelocity * (1.0f / (1.0f + settings.linearDamping * dt));
        body.angularVelocity = body.angularVelocity * (1.0f / (1.0f + settings.angularDamping * dt));
    }

    // Restitution is measured once, before any impulse has been applied. Read
    // during the iterations instead, it would feed on the velocity the solver
    // itself just produced, and the body would gain height on each bounce.
    targetNormalVelocity_.assign(contacts_.size(), 0.0f);
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& contact = contacts_[i];
        const RigidBody& a = bodies_[size_t(contact.bodyA)];

        Vec3 velocity = a.velocityAt(contact.position);
        float restitution = a.restitution;
        if (contact.bodyB >= 0) {
            const RigidBody& b = bodies_[size_t(contact.bodyB)];
            velocity = velocity - b.velocityAt(contact.position);
            restitution = std::max(restitution, b.restitution);
        }

        float vn = dot(velocity, contact.normal);
        if (vn < -settings.restitutionThreshold) targetNormalVelocity_[i] = -restitution * vn;
    }

    solveVelocities(dt);

    // A joint gives way on the force it had to carry, measured after the
    // velocity pass -- the position pass is correcting an error, not holding
    // a load, and counting it would make a joint break for being badly placed
    // rather than for being overloaded.
    for (Constraint& joint : constraints_) {
        if (joint.broken || joint.breakImpulse <= 0.0f) continue;

        float carried = joint.kind == ConstraintKind::Distance
                            ? std::fabs(joint.distanceImpulse)
                            : length(joint.linearImpulse);
        if (carried > joint.breakImpulse) {
            joint.broken = true;
            ++stats_.brokenConstraints;
        }
    }

    integrateAndSleep(dt);

    for (const RigidBody& body : bodies_) {
        if (body.isStatic()) continue;
        if (body.sleeping) ++stats_.sleepingBodies; else ++stats_.awakeBodies;
    }
    stats_.contacts = int(contacts_.size());
    stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ------------------------------------------------------------------- solver
void PhysicsWorld::solveVelocities(float dt) {
    // Joints are solved even with no contacts at all: a pendulum hanging in
    // clear air touches nothing and is still constrained.
    if (contacts_.empty() && constraints_.empty()) return;

    // Cached per contact: the inverse inertias and the arms do not change
    // during the iterations, since only velocities are being solved.
    struct Row {
        Mat3 invInertiaA, invInertiaB;
        Vec3 rA{}, rB{};
        Vec3 tangent[2];
        float normalMass = 0.0f;
        float tangentMass[2]{0.0f, 0.0f};
        float friction = 0.0f;
        float bias = 0.0f;
    };

    auto bodyAt = [&](int index) -> RigidBody* {
        return index >= 0 ? &bodies_[size_t(index)] : nullptr;
    };

    std::vector<Row> rows(contacts_.size());
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& contact = contacts_[i];
        RigidBody& a = bodies_[size_t(contact.bodyA)];
        RigidBody* b = bodyAt(contact.bodyB);

        Row& row = rows[i];
        row.invInertiaA = a.invInertiaWorld();
        row.rA = contact.position - a.position;
        if (b) {
            row.invInertiaB = b->invInertiaWorld();
            row.rB = contact.position - b->position;
        }
        orthonormalBasis(contact.normal, row.tangent[0], row.tangent[1]);

        auto pairMass = [&](Vec3 axis) {
            float k = effectiveMass(a, row.invInertiaA, row.rA, axis);
            if (b) k += effectiveMass(*b, row.invInertiaB, row.rB, axis);
            return k;
        };
        row.normalMass = pairMass(contact.normal);
        row.tangentMass[0] = pairMass(row.tangent[0]);
        row.tangentMass[1] = pairMass(row.tangent[1]);

        // Geometric mean, so two slippery things are slippery and one grippy
        // surface cannot rescue a slippery one.
        row.friction = b ? std::sqrt(a.friction * b->friction) : a.friction;

        // The separation speed the position pass aims for. It is deliberately
        // *not* used by the velocity pass: mixing it in there is the classic
        // Baumgarte stabilisation, and the velocity it adds stays in the body
        // after the overlap is gone. One box shrugs that off; a stack turns it
        // into a hum that never dies.
        float excess = std::max(0.0f, contact.depth - settings.penetrationSlop);
        row.bias = std::min(settings.baumgarte * excess / dt, settings.maxCorrectionSpeed);
    }

    auto relativeVelocity = [&](size_t i) {
        const Contact& contact = contacts_[i];
        Vec3 v = bodies_[size_t(contact.bodyA)].velocityAt(contact.position);
        if (contact.bodyB >= 0) v = v - bodies_[size_t(contact.bodyB)].velocityAt(contact.position);
        return v;
    };

    auto applyPair = [&](size_t i, Vec3 impulse) {
        const Contact& contact = contacts_[i];
        bodies_[size_t(contact.bodyA)].applyImpulse(impulse, contact.position);
        if (contact.bodyB >= 0)
            bodies_[size_t(contact.bodyB)].applyImpulse(-impulse, contact.position);
    };

    // Warm start: hand the solver last step's answer before it begins.
    //
    // The impulse has to be *applied*, not merely stored, or the first
    // iteration would immediately solve it away again as though the bodies
    // were still approaching. The tangent directions come from
    // `orthonormalBasis(normal, ...)`, a pure function of the normal, so as
    // long as a contact keeps its normal last step's tangent impulses still
    // mean what they meant.
    impulses_.clear();
    if (settings.warmStart) {
        for (size_t i = 0; i < contacts_.size(); ++i) {
            Contact& contact = contacts_[i];
            auto found = previousImpulses_.find(
                impulseKey(bodyKey(contact.bodyA), bodyKey(contact.bodyB), contact.feature));
            if (found == previousImpulses_.end()) continue;

            const Row& row = rows[i];
            contact.normalImpulse = found->second.normal;
            contact.tangentImpulse[0] = found->second.tangent[0];
            contact.tangentImpulse[1] = found->second.tangent[1];

            applyPair(i, contact.normal * contact.normalImpulse +
                             row.tangent[0] * contact.tangentImpulse[0] +
                             row.tangent[1] * contact.tangentImpulse[1]);
            ++stats_.warmStarted;
        }
    }

    for (int iteration = 0; iteration < settings.iterations; ++iteration) {
        for (size_t i = 0; i < contacts_.size(); ++i) {
            Contact& contact = contacts_[i];
            const Row& row = rows[i];

            // Normal first: friction is clamped against the normal impulse, so
            // solving it second means clamping against this iteration's value
            // rather than the previous one's.
            if (row.normalMass > kEps) {
                float vn = dot(relativeVelocity(i), contact.normal);
                float lambda = (targetNormalVelocity_[i] - vn) / row.normalMass;

                // Clamp the running total, not this increment. A contact may
                // only ever have pushed, but an individual iteration is
                // allowed to take back part of what an earlier one gave.
                float previous = contact.normalImpulse;
                contact.normalImpulse = std::max(0.0f, previous + lambda);
                applyPair(i, contact.normal * (contact.normalImpulse - previous));
            }

            float limit = row.friction * contact.normalImpulse;
            for (int t = 0; t < 2; ++t) {
                if (row.tangentMass[t] <= kEps) continue;

                float vt = dot(relativeVelocity(i), row.tangent[t]);
                float lambda = -vt / row.tangentMass[t];

                float previous = contact.tangentImpulse[t];
                contact.tangentImpulse[t] = std::clamp(previous + lambda, -limit, limit);
                applyPair(i, row.tangent[t] * (contact.tangentImpulse[t] - previous));
            }
        }

        // Joints share the iteration budget with contacts rather than getting
        // their own loop: a crate welded to a crate resting on the floor is
        // one problem, and solving the two halves alternately is what makes
        // them agree.
        solveConstraints(dt, false);
    }

    // Position pass, on the parallel velocity. Same rows, same clamp, but the
    // impulses land in `pseudo*` and are spent moving the bodies apart during
    // integration rather than staying in them as motion.
    for (int iteration = 0; iteration < settings.iterations; ++iteration) {
        for (size_t i = 0; i < contacts_.size(); ++i) {
            Contact& contact = contacts_[i];
            const Row& row = rows[i];
            if (row.normalMass <= kEps || row.bias <= 0.0f) continue;

            const Contact& c = contacts_[i];
            Vec3 pseudo = bodies_[size_t(c.bodyA)].pseudoVelocityAt(c.position);
            if (c.bodyB >= 0) pseudo = pseudo - bodies_[size_t(c.bodyB)].pseudoVelocityAt(c.position);

            float vn = dot(pseudo, contact.normal);
            float lambda = (row.bias - vn) / row.normalMass;

            float previous = contact.pseudoImpulse;
            contact.pseudoImpulse = std::max(0.0f, previous + lambda);

            Vec3 impulse = contact.normal * (contact.pseudoImpulse - previous);
            bodies_[size_t(c.bodyA)].applyPseudoImpulse(impulse, c.position);
            if (c.bodyB >= 0) bodies_[size_t(c.bodyB)].applyPseudoImpulse(-impulse, c.position);
        }

        solveConstraints(dt, true);
    }

    for (const Contact& contact : contacts_) {
        CachedImpulse& cached =
            impulses_[impulseKey(bodyKey(contact.bodyA), bodyKey(contact.bodyB), contact.feature)];
        cached.normal = contact.normalImpulse;
        cached.tangent[0] = contact.tangentImpulse[0];
        cached.tangent[1] = contact.tangentImpulse[1];
    }
    previousImpulses_.swap(impulses_);
}

// ------------------------------------------------------- integrate and sleep
void PhysicsWorld::integrateAndSleep(float dt) {
    for (RigidBody& body : bodies_) {
        if (body.isStatic() || body.sleeping) continue;

        // Real velocity plus the correction, which is then discarded -- the
        // body moves out of the overlap without keeping the motion that did
        // it. Sleep is judged on the real velocity alone, so a body still
        // being pushed apart is not thereby kept awake.
        Vec3 linear = body.linearVelocity + body.pseudoLinear;
        Vec3 angular = body.angularVelocity + body.pseudoAngular;
        body.pseudoLinear = Vec3{};
        body.pseudoAngular = Vec3{};

        body.position = body.position + linear * dt;

        // q' = q + 0.5 * omega * q * dt, renormalised. The renormalisation is
        // not tidiness: the first-order step leaves the quaternion slightly
        // long every time, and an un-normalised one is a rotation with a
        // scale baked into it, which would slowly grow the body.
        Quat spin{angular.x, angular.y, angular.z, 0.0f};
        body.orientation = normalize(body.orientation + (spin * body.orientation) * (0.5f * dt));

        bool slow = lengthSq(body.linearVelocity) < settings.sleepLinear * settings.sleepLinear &&
                    lengthSq(body.angularVelocity) < settings.sleepAngular * settings.sleepAngular;
        body.restSeconds = slow ? body.restSeconds + dt : 0.0f;
    }

    // Sleep by island, never one body at a time. A crate resting on a crate
    // that is still sliding is *itself* still moving, however small its own
    // velocity happens to be at the instant it is measured -- and a body put
    // to sleep on top of a moving one hangs in the air when the one below
    // leaves.
    //
    // The island's timer is the minimum over its members, so one restless
    // body keeps the group awake and they all fall asleep together.
    size_t count = bodies_.size();
    std::vector<float> islandRest(count, std::numeric_limits<float>::infinity());

    for (size_t i = 0; i < count; ++i) {
        if (bodies_[i].isStatic() || bodies_[i].sleeping) continue;
        size_t root = size_t(islandRoot(int(i)));
        islandRest[root] = std::min(islandRest[root], bodies_[i].restSeconds);
    }

    for (size_t i = 0; i < count; ++i) {
        if (bodies_[i].isStatic() || bodies_[i].sleeping) continue;
        if (islandRest[size_t(islandRoot(int(i)))] < settings.sleepSeconds) continue;

        bodies_[i].sleeping = true;
        bodies_[i].linearVelocity = Vec3{};
        bodies_[i].angularVelocity = Vec3{};
    }
}

} // namespace blocky
