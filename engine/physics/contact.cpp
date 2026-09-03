#include "engine/physics/contact.hpp"

#include <algorithm>
#include <cmath>

namespace blocky {
namespace {

constexpr float kCellHalf = 0.5f;
constexpr float kTouchEpsilon = 1e-4f;
constexpr int   kMaxPointsPerPair = 4;

struct Obb {
    Vec3 center{};
    Vec3 axis[3]{};
    Vec3 half{};
};

Obb cellObb(IVec3 cell) {
    Obb box;
    box.center = toVec3(cell) + Vec3{kCellHalf};
    box.axis[0] = {1.0f, 0.0f, 0.0f};
    box.axis[1] = {0.0f, 1.0f, 0.0f};
    box.axis[2] = {0.0f, 0.0f, 1.0f};
    box.half = Vec3{kCellHalf};
    return box;
}

Obb boxOf(const RigidBody& body, const Mat3& rotation, size_t index) {
    const ColliderBox& local = body.collider->boxes[index];
    Obb box;
    box.center = body.position + rotation * local.center;
    box.axis[0] = {rotation.m[0][0], rotation.m[0][1], rotation.m[0][2]};
    box.axis[1] = {rotation.m[1][0], rotation.m[1][1], rotation.m[1][2]};
    box.axis[2] = {rotation.m[2][0], rotation.m[2][1], rotation.m[2][2]};
    box.half = local.halfExtents;
    return box;
}

// How far the box reaches along `axis`, measured from its centre.
float projectedRadius(const Obb& box, Vec3 axis) {
    return std::fabs(dot(box.axis[0], axis)) * box.half.x +
           std::fabs(dot(box.axis[1], axis)) * box.half.y +
           std::fabs(dot(box.axis[2], axis)) * box.half.z;
}

bool insideObb(const Obb& box, Vec3 point) {
    Vec3 d = point - box.center;
    return std::fabs(dot(d, box.axis[0])) <= box.half.x + kTouchEpsilon &&
           std::fabs(dot(d, box.axis[1])) <= box.half.y + kTouchEpsilon &&
           std::fabs(dot(d, box.axis[2])) <= box.half.z + kTouchEpsilon;
}

Vec3 cornerOf(const Obb& box, int index) {
    return box.center +
           box.axis[0] * ((index & 1) ? box.half.x : -box.half.x) +
           box.axis[1] * ((index & 2) ? box.half.y : -box.half.y) +
           box.axis[2] * ((index & 4) ? box.half.z : -box.half.z);
}

uint64_t mixHash(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

// What the separating-axis test found: the direction, how deep, and -- which
// matters as much as either -- *whose face* it belongs to.
//
// The last field is what lets the manifold be built by clipping one face
// against another. An axis taken from a box's own list names a real face of
// that box; one taken from a cross product names no face at all, and there is
// nothing to clip.
struct SatHit {
    Vec3  normal{};        // from b towards a: the way A must move to leave
    float depth = 0.0f;
    int   ownerAxis = -1;  // which of the owner's three axes won
    bool  ownerIsA = false;
    bool  edge = false;    // a cross-product axis: no face, so no clipping
};

// An edge axis has to beat a face axis by this much before it is preferred.
//
// Two boxes resting face to face have their face axis and several edge axes
// separating them by very nearly the same amount, so without a bias the
// winner alternates on floating-point noise -- and with it alternates the
// whole shape of the manifold, which is the one thing a resting stack cannot
// survive. Every engine that clips faces carries a constant like this.
constexpr float kFaceBias = 1e-3f;

// Separating-axis test between two oriented boxes.
//
// Returns false as soon as any of the fifteen axes separates them. On overlap
// it reports the axis of least penetration, oriented to point from `b`
// towards `a` -- the direction A has to move to stop overlapping.
//
// `veto` is asked about each candidate direction after it has been oriented,
// and may refuse it as a *choice* without refusing it as a separation test.
// The lattice needs this and nothing else does; see satBoxAgainstCell.
template <class Veto>
bool satObbVsObb(const Obb& a, const Obb& b, const Veto& veto, SatHit& hit) {
    Vec3 toA = a.center - b.center;

    SatHit best;
    bool haveAxis = false;

    auto consider = [&](Vec3 axis, int ownerAxis, bool ownerIsA, bool edge) {
        float axisLengthSq = dot(axis, axis);
        if (axisLengthSq < 1e-12f) return true;   // degenerate: parallel edges

        Vec3 l = axis / std::sqrt(axisLengthSq);
        float overlap = projectedRadius(a, l) + projectedRadius(b, l) - std::fabs(dot(toA, l));
        if (overlap <= 0.0f) return false;        // separated: no contact at all

        // Orient towards A before the veto, since the veto asks which face we
        // would be pushing out through.
        if (dot(toA, l) < 0.0f) l = -l;
        if (veto(l)) return true;

        // Shallowest wins, except that a face is preferred to an edge unless
        // the edge is clearly shallower -- see kFaceBias.
        bool better;
        if (!haveAxis) {
            better = true;
        } else if (edge && !best.edge) {
            better = overlap < best.depth - kFaceBias;
        } else if (!edge && best.edge) {
            better = overlap < best.depth + kFaceBias;
        } else {
            better = overlap < best.depth;
        }

        if (better) {
            best.normal = l;
            best.depth = overlap;
            best.ownerAxis = ownerAxis;
            best.ownerIsA = ownerIsA;
            best.edge = edge;
            haveAxis = true;
        }
        return true;
    };

    for (int i = 0; i < 3; ++i)
        if (!consider(b.axis[i], i, false, false)) return false;
    for (int i = 0; i < 3; ++i)
        if (!consider(a.axis[i], i, true, false)) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!consider(cross(a.axis[i], b.axis[j]), -1, false, true)) return false;

    if (!haveAxis) return false;

    hit = best;
    return true;
}

// The lattice version. The extra rule: a world axis may not win if the
// neighbouring cell in that direction is also solid, because that face is not
// a surface at all -- it is the seam between two blocks of a floor.
//
// Without this a box sliding along flat ground catches on every seam it
// crosses, since the solver is periodically told to push it sideways out of a
// cell it is genuinely inside, through a face with another block behind it.
// The symptom reads as the floor being sticky, not as a contact bug.
//
// The veto keys off the *direction*, not off which list the axis came from. A
// box sitting square with the world has its own axes equal to the world's, so
// a veto that trusted the list would be bypassed by the duplicate -- and
// square with the world is the common case, so the rule would have been
// missing exactly where it is needed.
bool satBoxAgainstCell(const Obb& box, const World& world, IVec3 cell, SatHit& hit) {
    auto veto = [&](Vec3 l) {
        IVec3 step{int(std::lround(l.x)), int(std::lround(l.y)), int(std::lround(l.z))};
        bool alongLattice = std::fabs(l.x - float(step.x)) < 1e-4f &&
                            std::fabs(l.y - float(step.y)) < 1e-4f &&
                            std::fabs(l.z - float(step.z)) < 1e-4f;
        return alongLattice && world.collides(cell + step);
    };

    // Every axis overlapped but every one was vetoed: the box is inside solid
    // rock with no uncovered face to leave through. Reporting nothing is
    // right -- any direction we picked would push it into another block.
    return satObbVsObb(box, cellObb(cell), veto, hit);
}

// ------------------------------------------------------------ face clipping
//
// How the manifold is actually built, and the second attempt at it.
//
// The first took every corner of each box that was inside the other. That is
// simple and it is enough for a corner poking into something, and it fails at
// the one arrangement everything in a sandbox does: a flat face resting on a
// flat face. There the corners lie *on* each other's boundary rather than
// inside it, so whether each one counts is decided in the last bits of a
// float -- and the answer changes every step. A face-to-face pair that should
// have four fixed points got nought, one or two moving ones, which is not
// enough to stop a box turning. So it turned, and once it was turned the
// corners really were outside, and it sank through whatever it was resting
// on. Crates could not be stacked; five could, but only because they fell
// asleep before the rot set in, which is what made it look like it worked.
//
// The standard answer, and the one here: take the face that won the SAT as a
// *reference*, take the most opposed face of the other box as the *incident*
// one, clip the incident face against the reference face's four sides, and
// keep what is behind the reference plane. Face to face that yields four
// points every step in the same places with the same names, whatever the
// penetration is doing in the last bits.
//
// It also measures depth per point against a real plane, which the old code
// could only do against the lattice and had a flag for. A tilted box has
// genuinely different depths at its corners, and one SAT number over-corrects
// the shallow ones.

// The face of `box` whose outward normal is closest to `direction`.
void faceAlong(const Obb& box, Vec3 direction, int& axisOut, float& signOut) {
    axisOut = 0;
    signOut = 1.0f;
    float best = -1.0f;
    for (int i = 0; i < 3; ++i) {
        float d = dot(box.axis[i], direction);
        if (std::fabs(d) > best) {
            best = std::fabs(d);
            axisOut = i;
            signOut = d >= 0.0f ? 1.0f : -1.0f;
        }
    }
}

// The four corners of one face, walked round it.
void faceCorners(const Obb& box, int axis, float sign, Vec3 out[4]) {
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    Vec3 centre = box.center + box.axis[axis] * (sign * box.half[axis]);
    Vec3 du = box.axis[u] * box.half[u];
    Vec3 dv = box.axis[v] * box.half[v];
    out[0] = centre - du - dv;
    out[1] = centre + du - dv;
    out[2] = centre + du + dv;
    out[3] = centre - du + dv;
}

// Sutherland-Hodgman against one plane, keeping the half-space
// dot(normal, p) <= offset.
int clipToPlane(const Vec3* in, int count, Vec3 planeNormal, float planeOffset, Vec3* out) {
    int produced = 0;
    for (int i = 0; i < count && produced < 14; ++i) {
        Vec3 current = in[i];
        Vec3 next = in[(i + 1) % count];
        float dCurrent = dot(planeNormal, current) - planeOffset;
        float dNext = dot(planeNormal, next) - planeOffset;

        if (dCurrent <= 0.0f) out[produced++] = current;
        if ((dCurrent > 0.0f) != (dNext > 0.0f) && produced < 15) {
            float span = dCurrent - dNext;
            if (std::fabs(span) > 1e-9f) out[produced++] = current + (next - current) * (dCurrent / span);
        }
    }
    return produced;
}

// Builds the manifold for one overlapping pair.
//
// Points come from both directions: corners of A inside B, and corners of B
// inside A. Neither alone is enough -- the first misses a large body resting
// on a small one, the second misses a small body resting inside a large one.
//
// `exactAgainstFace` asks for the per-point depth to be measured against a
// known plane rather than taken from the SAT. Only the lattice can offer that,
// and only when the normal is one of its axes.
void appendManifold(const Obb& a, const Obb& b, const SatHit& sat, uint64_t featureBase,
                    std::vector<Contact>& out) {
    const Vec3 normal = sat.normal;
    const float satDepth = sat.depth;

    Contact candidates[16];
    int count = 0;

    // `fromA` separates the two kinds of point, and the difference is not
    // cosmetic. A corner of A has an exact depth -- how far past B's face it
    // reached -- and that exactness matters for a tilted box, whose corners
    // are genuinely at different depths and for which one SAT number
    // over-corrects the shallow ones.
    //
    // A corner of B is a different situation: it lies on B's own surface, so
    // measuring it against that surface gives zero by construction. Rejecting
    // zero as "not really touching" is wrong here and was a real bug -- it
    // deleted every contact in the one case the second half exists for, a
    // body wider than the cell it rests on, which then fell straight through
    // a single-block pillar.
    auto push = [&](Vec3 point, float depth, uint64_t featureId) {
        if (count >= 16) return;

        // Two points at the same place *on the contact plane* are one contact,
        // however far apart they are along the normal. This is not a tidying
        // step, it is what makes a stack stand still.
        //
        // Face to face, the two halves of the manifold land on top of each
        // other: A's four bottom corners and B's four top corners share their
        // tangential position and differ only by the penetration. Measured in
        // three dimensions they are eight distinct points, all at nearly equal
        // depth, of which the cap keeps an arbitrary four -- and *which* four
        // flips from step to step as the depths jitter in the last bits. Every
        // flip renames the contacts, so warm starting misses, the friction
        // impulses restart from zero, and a stack that looks stable creeps
        // sideways and never sleeps.
        //
        // A's corners are pushed first, so keeping the earlier point makes the
        // choice stable rather than merely fewer -- and against the lattice
        // A's corner is the one with the exactly measured depth.
        for (int i = 0; i < count; ++i) {
            Vec3 delta = candidates[i].position - point;
            Vec3 tangential = delta - normal * dot(delta, normal);
            if (lengthSq(tangential) < 1e-6f) return;
        }

        Contact contact;
        contact.position = point;
        contact.normal = normal;
        contact.depth = depth;
        contact.feature = mixHash(featureBase, featureId);
        candidates[count++] = contact;
    };

    if (!sat.edge) {
        // ------------------------------------------------- face against face
        // The reference face is the one that won, turned to look at the other
        // box: B's faces look along the normal, A's against it.
        const Obb& reference = sat.ownerIsA ? a : b;
        const Obb& incident = sat.ownerIsA ? b : a;
        const Vec3 towards = sat.ownerIsA ? -normal : normal;

        const int refAxis = sat.ownerAxis;
        const float refSign = dot(reference.axis[refAxis], towards) >= 0.0f ? 1.0f : -1.0f;
        const Vec3 refNormal = reference.axis[refAxis] * refSign;
        const float refOffset = dot(refNormal, reference.center) + reference.half[refAxis];

        int incAxis = 0;
        float incSign = 1.0f;
        faceAlong(incident, -refNormal, incAxis, incSign);

        Vec3 polygon[16], scratch[16];
        faceCorners(incident, incAxis, incSign, polygon);
        int points = 4;

        const int sides[2] = {(refAxis + 1) % 3, (refAxis + 2) % 3};
        for (int s = 0; s < 2 && points > 0; ++s) {
            for (int sign = -1; sign <= 1; sign += 2) {
                Vec3 planeNormal = reference.axis[sides[s]] * float(sign);
                float planeOffset = dot(planeNormal, reference.center) + reference.half[sides[s]];
                points = clipToPlane(polygon, points, planeNormal, planeOffset, scratch);
                for (int i = 0; i < points; ++i) polygon[i] = scratch[i];
                if (points == 0) break;
            }
        }

        // The name has to survive from step to step or warm starting misses,
        // so it is built from which faces met and which corner of the clipped
        // polygon this is -- all of which stay put while the arrangement does.
        uint64_t faces = uint64_t(uint32_t(refAxis)) | (refSign > 0.0f ? 0x10u : 0u) |
                         (uint64_t(uint32_t(incAxis)) << 8) | (incSign > 0.0f ? 0x1000u : 0u) |
                         (sat.ownerIsA ? 0x10000u : 0u);

        for (int i = 0; i < points; ++i) {
            float depth = refOffset - dot(refNormal, polygon[i]);
            if (depth < 0.0f) continue;   // in front of the face: not touching
            push(polygon[i], depth, mixHash(faces, uint64_t(uint32_t(i))));
        }
    } else {
        // ------------------------------------------------- edge against edge
        // No face to clip against, and no need: an edge contact really is one
        // corner in one place, and the corner test finds it. This is also the
        // only case where the single SAT depth is the honest number.
        for (int i = 0; i < 8; ++i) {
            Vec3 corner = cornerOf(a, i);
            if (insideObb(b, corner)) push(corner, satDepth, uint64_t(uint32_t(0x100 | i)));
        }
        for (int i = 0; i < 8; ++i) {
            Vec3 corner = cornerOf(b, i);
            if (insideObb(a, corner)) push(corner, satDepth, uint64_t(uint32_t(i)));
        }
    }

    if (count == 0) return;

    // Keep the deepest few. More points than this on one pair are redundant --
    // four already pin all six degrees of freedom -- and redundant rows make
    // the solver fight itself.
    // Deepest first, with the feature id breaking ties. Depth alone is not a
    // total order when four corners share a plane, and an unstable order is
    // the same disease the tangential merge above cures.
    std::sort(candidates, candidates + count, [](const Contact& x, const Contact& y) {
        if (x.depth != y.depth) return x.depth > y.depth;
        return x.feature < y.feature;
    });
    int keep = std::min(count, kMaxPointsPerPair);
    for (int i = 0; i < keep; ++i) out.push_back(candidates[i]);
}

}  // namespace

void bodyBounds(const RigidBody& body, Vec3& lo, Vec3& hi) {
    lo = body.position;
    hi = body.position;
    if (!body.collider || body.collider->empty()) return;

    Mat3 rotation = toMat3(body.orientation);
    bool first = true;

    for (size_t i = 0; i < body.collider->boxes.size(); ++i) {
        Obb box = boxOf(body, rotation, i);
        Vec3 extent{projectedRadius(box, {1, 0, 0}), projectedRadius(box, {0, 1, 0}),
                    projectedRadius(box, {0, 0, 1})};
        Vec3 boxLo = box.center - extent;
        Vec3 boxHi = box.center + extent;
        lo = first ? boxLo : minv(lo, boxLo);
        hi = first ? boxHi : maxv(hi, boxHi);
        first = false;
    }
}

void collideBodyWithWorld(const RigidBody& body, const World& world,
                          std::vector<Contact>& out) {
    if (!body.collider || body.collider->empty()) return;

    Mat3 rotation = toMat3(body.orientation);

    for (size_t boxIndex = 0; boxIndex < body.collider->boxes.size(); ++boxIndex) {
        Obb box = boxOf(body, rotation, boxIndex);

        // World AABB of the oriented box: the projected radius along each
        // world axis, which is the same sum the SAT uses.
        Vec3 extent{projectedRadius(box, {1, 0, 0}), projectedRadius(box, {0, 1, 0}),
                    projectedRadius(box, {0, 0, 1})};

        IVec3 lo = floorToInt(box.center - extent - Vec3{kTouchEpsilon});
        IVec3 hi = floorToInt(box.center + extent + Vec3{kTouchEpsilon});

        for (int y = lo.y; y <= hi.y; ++y) {
            for (int z = lo.z; z <= hi.z; ++z) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    IVec3 cell{x, y, z};
                    if (!world.collides(cell)) continue;

                    SatHit sat;
                    if (!satBoxAgainstCell(box, world, cell, sat)) continue;

                    uint64_t base = mixHash(0x9e3779b9ull, uint64_t(uint32_t(boxIndex)));
                    base = mixHash(base, uint64_t(uint32_t(cell.x)));
                    base = mixHash(base, uint64_t(uint32_t(cell.y)));
                    base = mixHash(base, uint64_t(uint32_t(cell.z)));

                    appendManifold(box, cellObb(cell), sat, base, out);
                }
            }
        }
    }
}

void collideBodies(const RigidBody& a, const RigidBody& b, std::vector<Contact>& out) {
    if (!a.collider || a.collider->empty()) return;
    if (!b.collider || b.collider->empty()) return;

    Mat3 rotationA = toMat3(a.orientation);
    Mat3 rotationB = toMat3(b.orientation);

    auto never = [](Vec3) { return false; };

    for (size_t i = 0; i < a.collider->boxes.size(); ++i) {
        Obb boxA = boxOf(a, rotationA, i);
        Vec3 extentA{projectedRadius(boxA, {1, 0, 0}), projectedRadius(boxA, {0, 1, 0}),
                     projectedRadius(boxA, {0, 0, 1})};

        for (size_t j = 0; j < b.collider->boxes.size(); ++j) {
            Obb boxB = boxOf(b, rotationB, j);
            Vec3 extentB{projectedRadius(boxB, {1, 0, 0}), projectedRadius(boxB, {0, 1, 0}),
                         projectedRadius(boxB, {0, 0, 1})};

            // Cheap rejection before the fifteen axes: a model of many boxes
            // against another of many boxes is quadratic, and most pairs are
            // nowhere near each other.
            Vec3 gap = absv(boxA.center - boxB.center) - (extentA + extentB);
            if (gap.x > 0.0f || gap.y > 0.0f || gap.z > 0.0f) continue;

            SatHit sat;
            if (!satObbVsObb(boxA, boxB, never, sat)) continue;

            uint64_t base = mixHash(0x517cc1b7ull, uint64_t(uint32_t(i)));
            base = mixHash(base, uint64_t(uint32_t(j)));

            appendManifold(boxA, boxB, sat, base, out);
        }
    }
}

} // namespace blocky
