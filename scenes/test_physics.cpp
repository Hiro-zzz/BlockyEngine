// Tests for the rigid-body spike: box decomposition, mass properties,
// contacts against the lattice, and a body that actually stops.
//
// A physics engine has no outside oracle. The path tracer can be checked
// against the CPU one, the codec against ffmpeg, the DDA against a fine march
// -- there is nothing equivalent here, and "it looked stable" is exactly the
// kind of claim this project does not accept elsewhere. So the tests are
// written against the things that *can* be stated exactly:
//
//   - the decomposition is a partition, checked cell by cell;
//   - a cube's inertia is m*a^2/6, which is arithmetic, not opinion;
//   - a resting body's contacts all point along +Y, which is a claim about
//     the veto rule and fails loudly without it;
//   - a dropped body ends asleep, above the floor, no higher than it started.
//
// That last one is the honest form of "it settles": position alone would be
// satisfied by a box vibrating on the spot for ever.
//
// Needs no game files.
#include "engine/physics/character.hpp"
#include "engine/physics/constraint.hpp"
#include "engine/physics/contact.hpp"
#include "engine/physics/pick.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

bool nearly(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

VoxelModel solidBox(IVec3 dims) {
    VoxelModel model;
    model.resize(dims);
    VoxelMaterial material;
    material.albedo = {0.7f, 0.7f, 0.75f};
    uint16_t id = model.addMaterial(material);
    for (int y = 0; y < dims.y; ++y)
        for (int z = 0; z < dims.z; ++z)
            for (int x = 0; x < dims.x; ++x) model.set({x, y, z}, id);
    return model;
}

// ------------------------------------------------------------ decomposition
void testDecomposition() {
    std::printf("box decomposition\n");

    VoxelModel cube = solidBox({4, 4, 4});
    Collider collider = buildCollider(cube, 1.0f);

    check(collider.boxes.size() == 1, "a solid cube merges to exactly one box");
    check(nearly(collider.volume, 64.0f, 1e-4f), "with the right volume");
    check(nearly(collider.centreOfMassVoxel.x, 2.0f, 1e-4f) &&
          nearly(collider.centreOfMassVoxel.y, 2.0f, 1e-4f) &&
          nearly(collider.centreOfMassVoxel.z, 2.0f, 1e-4f),
          "centred in voxel coordinates");
    if (!collider.boxes.empty()) {
        check(nearly(collider.boxes[0].halfExtents.x, 2.0f, 1e-4f), "half extents of two");
        check(nearly(length(collider.boxes[0].center), 0.0f, 1e-4f),
              "and the box sits on the centre of mass");
    }

    // A shape the greedy merge cannot do in one box, checked as a partition
    // rather than against a specific set of boxes: the merge order is an
    // implementation choice, but covering every solid voxel exactly once is
    // not.
    VoxelModel blob;
    blob.resize({12, 9, 7});
    VoxelMaterial material;
    uint16_t id = blob.addMaterial(material);

    size_t solid = 0;
    for (int y = 0; y < 9; ++y) {
        for (int z = 0; z < 7; ++z) {
            for (int x = 0; x < 12; ++x) {
                // An L in cross-section with a bite out of the middle.
                bool fill = (x < 5 || y < 3) && !(x > 6 && y > 5) && !(x == 3 && z == 2 && y == 4);
                if (fill) { blob.set({x, y, z}, id); ++solid; }
            }
        }
    }

    Collider part = buildCollider(blob, 1.0f);
    check(!part.empty(), "the blob decomposes into something");
    std::printf("  %zu solid voxels -> %zu boxes\n", solid, part.boxes.size());

    std::vector<int> cover(size_t(12 * 9 * 7), 0);
    for (const ColliderBox& box : part.boxes) {
        Vec3 lo = box.center - box.halfExtents + part.centreOfMassVoxel;
        Vec3 hi = box.center + box.halfExtents + part.centreOfMassVoxel;
        for (int y = int(std::lround(lo.y)); y < int(std::lround(hi.y)); ++y)
            for (int z = int(std::lround(lo.z)); z < int(std::lround(hi.z)); ++z)
                for (int x = int(std::lround(lo.x)); x < int(std::lround(hi.x)); ++x)
                    ++cover[size_t((y * 7 + z) * 12 + x)];
    }

    int uncovered = 0, doubled = 0, phantom = 0;
    for (int y = 0; y < 9; ++y) {
        for (int z = 0; z < 7; ++z) {
            for (int x = 0; x < 12; ++x) {
                int times = cover[size_t((y * 7 + z) * 12 + x)];
                bool isSolid = blob.at({x, y, z}) != VoxelModel::kEmpty;
                if (isSolid && times == 0) ++uncovered;
                if (times > 1) ++doubled;
                if (!isSolid && times > 0) ++phantom;
            }
        }
    }

    check(uncovered == 0, "every solid voxel is inside some box");
    check(doubled == 0, "no voxel is inside two boxes");
    check(phantom == 0, "no box covers empty space");
    check(nearly(part.volume, float(solid), 1e-3f), "so the volume is the solid count");
}

// ---------------------------------------------------------- mass properties
void testMassProperties() {
    std::printf("mass properties\n");

    // A one-metre cube at 1000 kg/m^3. Inertia of a cube about its centre is
    // m*a^2/6 on every diagonal, and zero off it.
    VoxelModel cube = solidBox({4, 4, 4});
    Collider collider = buildCollider(cube, 0.25f);

    float mass = 0.0f;
    Mat3 inertia = inertiaTensor(collider, 1000.0f, &mass);

    check(nearly(mass, 1000.0f, 0.5f), "mass is density times volume");

    float expected = 1000.0f * 1.0f / 6.0f;
    check(nearly(inertia.m[0][0], expected, 0.5f), "Ixx = m a^2 / 6");
    check(nearly(inertia.m[1][1], expected, 0.5f), "Iyy likewise");
    check(nearly(inertia.m[2][2], expected, 0.5f), "Izz likewise");
    check(nearly(inertia.m[0][1], 0.0f, 1e-2f) && nearly(inertia.m[0][2], 0.0f, 1e-2f) &&
          nearly(inertia.m[1][2], 0.0f, 1e-2f),
          "and a cube has no products of inertia");

    // A long plank is harder to turn about its short axes than its long one.
    VoxelModel plank = solidBox({16, 2, 2});
    Collider plankCollider = buildCollider(plank, 0.0625f);
    float plankMass = 0.0f;
    Mat3 plankInertia = inertiaTensor(plankCollider, 1000.0f, &plankMass);
    check(plankInertia.m[0][0] < plankInertia.m[1][1], "a plank turns most easily about its length");
    check(nearly(plankInertia.m[1][1], plankInertia.m[2][2], 1e-2f), "and equally about the other two");

    // The body wrapper inverts all of that, and an empty collider leaves the
    // body static rather than producing an infinity.
    RigidBody body;
    setMassFromCollider(body, collider, 1000.0f);
    check(nearly(body.invMass, 1.0f / 1000.0f, 1e-6f), "invMass is one over mass");

    Collider nothing;
    RigidBody empty;
    setMassFromCollider(empty, nothing, 1000.0f);
    check(empty.isStatic(), "an empty collider gives a static body");
}

// --------------------------------------------------------- the placement seam
void testBodyToWorld() {
    std::printf("body to world\n");

    VoxelModel model = solidBox({4, 2, 2});
    Collider collider = buildCollider(model, 1.0f);

    RigidBody body;
    setMassFromCollider(body, collider, 1000.0f);
    body.voxelSize = 1.0f;
    body.position = {10.0f, 5.0f, -3.0f};

    Mat4 toWorld = bodyToWorld(body);

    // The centre of mass of the model has to land exactly on the body.
    Vec3 com = transformPoint(toWorld, collider.centreOfMassVoxel);
    check(nearly(length(com - body.position), 0.0f, 1e-4f),
          "the model's centre of mass lands on the body position");

    // And an arbitrary voxel keeps its offset, scaled.
    Vec3 corner = transformPoint(toWorld, Vec3{0.0f, 0.0f, 0.0f});
    Vec3 expected = body.position - collider.centreOfMassVoxel * body.voxelSize;
    check(nearly(length(corner - expected), 0.0f, 1e-4f), "and a corner keeps its offset");

    // Turned a quarter about Y, the model's +X voxel direction becomes -Z.
    body.orientation = Quat::axisAngle({0.0f, 1.0f, 0.0f}, radians(90.0f));
    Mat4 turned = bodyToWorld(body);
    Vec3 along = transformPoint(turned, collider.centreOfMassVoxel + Vec3{1.0f, 0.0f, 0.0f}) - body.position;
    check(nearly(along.z, -1.0f, 1e-3f) && nearly(along.x, 0.0f, 1e-3f),
          "a quarter turn about Y sends model +X to world -Z");
}

// ------------------------------------------------------------------ contacts
void testContacts() {
    std::printf("contacts against the lattice\n");

    World world(palette::registry());
    world.fillBox({-6, -1, -6}, {6, -1, 6}, palette::Stone);   // floor, top at y = 0

    VoxelModel cube = solidBox({16, 16, 16});
    Collider collider = buildCollider(cube, 0.125f);          // a two-metre cube

    RigidBody body;
    setMassFromCollider(body, collider, 500.0f);
    body.voxelSize = 0.125f;
    body.position = {1.0f, 1.0f - 0.01f, 1.0f};               // 1 cm into the floor

    std::vector<Contact> contacts;
    collideBodyWithWorld(body, world, contacts);

    check(!contacts.empty(), "a box resting in the floor produces contacts");
    std::printf("  %zu contacts under a 2 m cube spanning four cells\n", contacts.size());

    // This is the veto's test. The cube straddles four floor cells, and each
    // of those has solid neighbours on all four sides. Every sideways face is
    // a seam between two blocks, not a surface, so nothing may push sideways.
    int sideways = 0;
    int wrongDepth = 0;
    for (const Contact& contact : contacts) {
        if (!nearly(contact.normal.y, 1.0f, 1e-3f)) ++sideways;
        if (!nearly(contact.depth, 0.01f, 2e-3f)) ++wrongDepth;
    }
    check(sideways == 0, "every contact pushes straight up, none through a seam");
    check(wrongDepth == 0, "and reports the true penetration depth");

    // Buried completely: every direction out is blocked by another block, so
    // there is no honest normal and the right answer is silence.
    World solid(palette::registry());
    solid.fillBox({-4, -4, -4}, {4, 4, 4}, palette::Stone);

    RigidBody buried;
    setMassFromCollider(buried, collider, 500.0f);
    buried.voxelSize = 0.125f;
    buried.position = {0.5f, 0.5f, 0.5f};

    std::vector<Contact> none;
    collideBodyWithWorld(buried, solid, none);
    check(none.empty(), "a body inside solid rock reports no contacts");

    // Clear of the floor entirely: nothing at all.
    RigidBody high = body;
    high.position = {1.0f, 4.0f, 1.0f};
    std::vector<Contact> air;
    collideBodyWithWorld(high, world, air);
    check(air.empty(), "a body in the air reports no contacts");

    // A body wider than the cell it rests on. This is the case the cell-corner
    // half of the manifold exists for: none of the body's own corners is
    // anywhere near the block, so a manifold built only from those is empty
    // and the box falls straight through a pillar.
    //
    // It did, until this test was written. The cell's corners lie exactly on
    // its own top face, so measuring their depth against that face gave zero,
    // and zero was being discarded as "not touching".
    World pillar(palette::registry());
    pillar.set({0, 0, 0}, palette::Stone);   // one block, top at y = 1

    RigidBody wide;
    setMassFromCollider(wide, collider, 500.0f);
    wide.voxelSize = 0.125f;               // the same two-metre cube
    wide.position = {0.5f, 2.0f - 0.01f, 0.5f};   // sitting on the pillar, 1 cm in

    std::vector<Contact> onPillar;
    collideBodyWithWorld(wide, pillar, onPillar);

    check(!onPillar.empty(), "a body wider than its support still makes contact");
    std::printf("  %zu contacts for a 2 m cube on a single block\n", onPillar.size());

    int notUp = 0;
    for (const Contact& contact : onPillar)
        if (!nearly(contact.normal.y, 1.0f, 1e-3f)) ++notUp;
    check(notUp == 0, "and they push up off the top of it");
}

// ------------------------------------------------------------------- the drop
struct DropResult {
    Vec3 position{};
    Quat orientation{};
    bool sleeping = false;
    float lowest = 0.0f;
    int steps = 0;
    float sleptAt = -1.0f;   // seconds, or negative if it never did
};

DropResult drop(const World& world, const Collider& collider, Vec3 from, Quat spin,
                Vec3 initialVelocity, float seconds, float dt, bool warmStart = true) {
    PhysicsWorld physics;
    physics.settings.warmStart = warmStart;

    RigidBody body;
    setMassFromCollider(body, collider, 500.0f);
    body.voxelSize = 0.125f;
    body.position = from;
    body.orientation = spin;
    body.linearVelocity = initialVelocity;
    body.friction = 0.7f;
    body.restitution = 0.0f;
    physics.add(body);

    DropResult result;
    result.lowest = from.y;

    int steps = int(seconds / dt);
    for (int i = 0; i < steps; ++i) {
        physics.step(world, dt);
        result.lowest = std::min(result.lowest, physics.body(0).position.y);
        if (result.sleptAt < 0.0f && physics.body(0).sleeping) result.sleptAt = float(i + 1) * dt;
    }

    result.position = physics.body(0).position;
    result.orientation = physics.body(0).orientation;
    result.sleeping = physics.body(0).sleeping;
    result.steps = steps;
    return result;
}

void testDropAndRest() {
    std::printf("a body falls and stops\n");

    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);   // floor, top at y = 0

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);          // a one-metre cube

    const float dt = 1.0f / 120.0f;
    const float restY = 0.5f;   // half the cube above a floor whose top is y = 0

    DropResult flat = drop(world, collider, {0.5f, 3.0f, 0.5f}, Quat::identity(), Vec3{}, 5.0f, dt);
    std::printf("  flat drop: y = %.4f, asleep = %s\n", flat.position.y, flat.sleeping ? "yes" : "no");

    check(nearly(flat.position.y, restY, 0.02f), "it comes to rest on the floor");
    check(flat.sleeping, "and goes to sleep rather than trembling");
    check(flat.lowest > restY - 0.05f, "without ever sinking into the floor");
    check(flat.position.y <= 3.0f, "and never ends higher than it started");
    check(nearly(flat.position.x, 0.5f, 0.02f) && nearly(flat.position.z, 0.5f, 0.02f),
          "and does not wander sideways on a flat floor");

    // Dropped tilted, it has to tip onto a face and settle there.
    DropResult tilted = drop(world, collider, {0.5f, 3.0f, 0.5f},
                             Quat::axisAngle({0.0f, 0.0f, 1.0f}, radians(20.0f)), Vec3{}, 8.0f, dt);
    std::printf("  tilted drop: y = %.4f, asleep = %s\n", tilted.position.y,
                tilted.sleeping ? "yes" : "no");
    check(tilted.sleeping, "a tilted drop also settles");
    check(tilted.position.y > 0.4f && tilted.position.y < 0.8f,
          "resting on a face rather than sunk or perched");

    // Thrown down hard. There is no continuous collision detection here, so
    // this is a statement about where the limit is: at 40 m/s and 1/120 s a
    // step is a third of a metre, and the floor plus the box is two metres.
    DropResult fast = drop(world, collider, {0.5f, 6.0f, 0.5f}, Quat::identity(),
                           Vec3{0.0f, -40.0f, 0.0f}, 5.0f, dt);
    std::printf("  fast drop: y = %.4f, lowest = %.4f\n", fast.position.y, fast.lowest);
    check(fast.position.y > 0.4f, "a body thrown down at 40 m/s still stops on the floor");
    check(fast.lowest > -0.5f, "and never passes through it");
}

// Warm starting is a claim about how fast the solver converges, so it is
// tested as one: the same drop, run twice, differing only in the flag.
//
// The shape matters. A cube landing square hardly notices, because four
// symmetric contacts on a flat floor are nearly solved in one iteration. The
// case that shows it is a body that has to rock into place -- an L, landing
// tilted, where the contact set keeps changing and the solver spends its
// budget rediscovering the same forces.
void testWarmStarting() {
    std::printf("warm starting\n");

    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);

    VoxelModel ell;
    ell.resize({10, 10, 5});
    VoxelMaterial material;
    uint16_t id = ell.addMaterial(material);
    for (int y = 0; y < 10; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 10; ++x)
                if (x < 4 || y < 4) ell.set({x, y, z}, id);

    Collider collider = buildCollider(ell, 0.125f);
    Quat tilt = Quat::axisAngle({1.0f, 0.2f, 0.0f}, radians(48.0f));

    const float dt = 1.0f / 120.0f;

    // First: does the mechanism engage at all? A contact has to be recognised
    // by name from one step to the next, and if the feature ids were unstable
    // the cache would miss every time and warm starting would be a silent
    // no-op -- which is indistinguishable from "no benefit" if only the
    // settling time is measured. So the count is checked before the effect.
    {
        // A plain cube, so the resting height is known exactly, and sleeping
        // switched off -- a sleeping body is skipped by the collide pass and
        // reports no contacts at all, which would read as a cache miss.
        VoxelModel cube = solidBox({8, 8, 8});
        Collider cubeCollider = buildCollider(cube, 0.125f);

        PhysicsWorld physics;
        physics.settings.sleepSeconds = 1e9f;

        RigidBody body;
        setMassFromCollider(body, cubeCollider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = {0.5f, 0.55f, 0.5f};   // just above its resting height
        physics.add(body);

        for (int i = 0; i < 120; ++i) physics.step(world, dt);

        int recognised = physics.stats().warmStarted;
        std::printf("  %d of %d contacts recognised from the previous step\n", recognised,
                    physics.stats().contacts);
        check(physics.stats().contacts > 0, "a resting body has contacts to recognise");
        check(recognised > 0, "contacts are recognised across steps");
        check(recognised == physics.stats().contacts,
              "and a settled body recognises all of them");
    }

    DropResult cold = drop(world, collider, {0.5f, 4.0f, 0.5f}, tilt, Vec3{}, 20.0f, dt, false);
    DropResult warm = drop(world, collider, {0.5f, 4.0f, 0.5f}, tilt, Vec3{}, 20.0f, dt, true);

    std::printf("  L-shape settles in %.2f s cold, %.2f s warm-started\n", cold.sleptAt, warm.sleptAt);

    check(warm.sleptAt > 0.0f, "the warm-started drop settles");
    check(cold.sleptAt > 0.0f, "and so does the cold one");

    // Deliberately **not** asserting that warm starting settles faster here.
    // Measured, it does not: a single body on flat terrain has four nearly
    // symmetric contacts that the solver resolves in a couple of iterations
    // either way, and the two runs land within one sleep-timer quantum of
    // each other. Warm starting pays where an impulse has to travel through
    // a chain of contacts -- a stack -- and that case cannot exist until
    // bodies can touch each other. The claim is asserted there, not here.
    check(nearly(warm.position.y, cold.position.y, 0.02f),
          "and both reach the same resting height");
}

// ------------------------------------------------------------------ stacks
// Five one-metre cubes, each starting a centimetre above the last.
//
// A stack is the honest test of a contact solver, because every one of its
// failures is visible as a number rather than as a feeling. It sags if the
// solver runs out of iterations; it slides apart if friction is wrong; it
// jitters for ever if the slop is missing; and half of it falls asleep in
// mid-air if sleeping is decided per body instead of per island.
struct StackResult {
    std::vector<float> heights;
    float sag = 0.0f;         // how far the top fell short of where it belongs
    float spread = 0.0f;      // furthest any cube wandered sideways
    bool  allAsleep = false;
    float sleptAt = -1.0f;
};

StackResult stack(int height, bool warmStart, float seconds) {
    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);

    static VoxelModel cube = solidBox({8, 8, 8});
    static Collider collider = buildCollider(cube, 0.125f);

    PhysicsWorld physics;
    physics.settings.warmStart = warmStart;

    for (int i = 0; i < height; ++i) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        // A centimetre of air between them, so they arrive as a stack rather
        // than starting already interpenetrating.
        body.position = {0.5f, 0.5f + float(i) * 1.01f, 0.5f};
        body.friction = 0.7f;
        body.restitution = 0.0f;
        physics.add(body);
    }

    const float dt = 1.0f / 120.0f;
    int steps = int(seconds / dt);
    StackResult result;

    for (int i = 0; i < steps; ++i) {
        physics.step(world, dt);
        if (result.sleptAt < 0.0f && physics.stats().awakeBodies == 0)
            result.sleptAt = float(i + 1) * dt;
    }

    result.allAsleep = true;
    for (int i = 0; i < height; ++i) {
        const RigidBody& body = physics.body(i);
        result.heights.push_back(body.position.y);
        result.allAsleep = result.allAsleep && body.sleeping;
        result.spread = std::max(result.spread,
                                 length(Vec3{body.position.x - 0.5f, 0.0f, body.position.z - 0.5f}));
    }

    // Where the top cube belongs: half a cube up, then one cube per layer.
    float expectedTop = 0.5f + float(height - 1);
    result.sag = expectedTop - result.heights.back();
    return result;
}

void testStack() {
    std::printf("stacking\n");

    StackResult warm = stack(5, true, 8.0f);
    StackResult cold = stack(5, false, 8.0f);

    std::printf("  warm: sag %.4f m, spread %.4f m, asleep at %.2f s\n", warm.sag, warm.spread,
                warm.sleptAt);
    std::printf("  cold: sag %.4f m, spread %.4f m, asleep at %.2f s\n", cold.sag, cold.spread,
                cold.sleptAt);

    check(warm.allAsleep, "a five-high stack settles and the whole island sleeps");
    check(warm.sag < 0.05f, "without sinking into itself");
    check(warm.spread < 0.05f, "and without walking sideways");

    // Heights must come out one metre apart, which is the claim that the
    // cubes are resting *on* each other rather than merged.
    for (size_t i = 1; i < warm.heights.size(); ++i) {
        float gap = warm.heights[i] - warm.heights[i - 1];
        check(nearly(gap, 1.0f, 0.02f), "each cube sits one metre above the one below");
    }

    // This is where warm starting earns its place, and the claim deliberately
    // left unasserted in testWarmStarting: an impulse has to travel from the
    // floor to the top of the stack, and a fixed iteration budget starting
    // from zero every step does not get it there.
    //
    // **What it buys changed, and the number is worth keeping.** While the
    // manifold was built from corners, a face-to-face pair got one moving
    // contact point instead of four, and starting cold made a five-high stack
    // collapse outright -- four metres of sag against eleven millimetres.
    // With the faces clipped, cold sags barely more than warm; what it still
    // cannot do is *stop*. So the assertion moved from the height to the
    // stillness, which is the honest place for it: a stack that sags a
    // centimetre is a stack, and one that never sleeps is a bug you can hear.
    check(warm.sag <= cold.sag + 1e-4f, "warm starting does not sag more than starting cold");
    check(warm.spread < cold.spread, "and keeps the stack from walking, which is what it is for");
    check(warm.sleptAt > 0.0f, "the warm-started stack goes to sleep");
    std::printf("  warm starting removes %.1f%% of the sag and %.1f%% of the spread\n",
                cold.sag > 1e-5f ? 100.0 * double(cold.sag - warm.sag) / double(cold.sag) : 0.0,
                cold.spread > 1e-5f ? 100.0 * double(cold.spread - warm.spread) / double(cold.spread)
                                    : 0.0);
}

// -------------------------------------------------------------- constraints
// Joints are where a sign error is cheapest to make and most obvious to see:
// get one backwards and the correction drives the error the way it was
// already going, so the joint does not sag -- it explodes. Every test here
// therefore checks the invariant the joint claims to hold, at every step
// rather than only at the end.
void testConstraints() {
    std::printf("constraints\n");

    World world(palette::registry());
    world.fillBox({-10, -1, -10}, {10, -1, 10}, palette::Stone);

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);   // a one-metre cube
    const float dt = 1.0f / 120.0f;

    auto makeBody = [&](Vec3 at) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = at;
        body.friction = 0.6f;
        return body;
    };

    // ------------------------------------------------------------- the rope
    {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 9.0f, 0.0f};
        int id = physics.add(makeBody({0.0f, 8.0f, 0.0f}));   // 1 m below: slack

        Constraint rope = ropeBetween(physics.body(id), id, physics.body(id), -1,
                                      physics.body(id).position, anchor, 2.0f);
        physics.addConstraint(rope);

        float longest = 0.0f;
        for (int i = 0; i < 900; ++i) {
            physics.step(world, dt);
            longest = std::max(longest, length(physics.body(id).position - anchor));
        }

        float hanging = length(physics.body(id).position - anchor);
        std::printf("  rope: hangs at %.4f m, never exceeded %.4f m\n", hanging, longest);
        check(nearly(hanging, 2.0f, 0.02f), "a rope holds its length");
        check(longest < 2.05f, "and never stretches meaningfully past it");
        check(physics.body(id).position.y < 7.5f, "having gone slack first and let the body fall");
    }

    // ------------------------------------------------------ the ball socket
    {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 8.0f, 0.0f};
        int id = physics.add(makeBody({2.0f, 8.0f, 0.0f}));

        Constraint joint = ballSocketAt(physics.body(id), id, physics.body(id), -1, anchor);
        physics.addConstraint(joint);

        float worstSlip = 0.0f;
        float lowest = 8.0f;
        for (int i = 0; i < 600; ++i) {
            physics.step(world, dt);
            const RigidBody& body = physics.body(id);
            Vec3 held = body.position + rotate(body.orientation, joint.anchorA);
            worstSlip = std::max(worstSlip, length(held - anchor));
            lowest = std::min(lowest, body.position.y);
        }

        std::printf("  ball socket: anchor slipped at most %.4f m, swung down to y = %.2f\n",
                    worstSlip, lowest);
        check(worstSlip < 0.02f, "a ball socket keeps its anchor point");
        check(lowest < 7.0f, "and lets the body swing");
    }

    // ------------------------------------------------------------ the hinge
    {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 8.0f, 0.0f};
        Vec3 axis{0.0f, 0.0f, 1.0f};
        int id = physics.add(makeBody({2.0f, 8.0f, 0.0f}));

        // Spun hard about two axes the hinge is supposed to forbid. Without
        // this the test passes on a hinge that does nothing at all: gravity
        // alone never tries to tilt the axle, so "it stayed aligned" would be
        // a statement about the setup rather than about the joint.
        physics.body(id).angularVelocity = {4.0f, 4.0f, 0.0f};

        Constraint joint = hingeAt(physics.body(id), id, physics.body(id), -1, anchor, axis);
        physics.addConstraint(joint);

        float worstTilt = 0.0f;
        float swept = 0.0f;
        float spinAboutAxle = 0.0f;
        for (int i = 0; i < 600; ++i) {
            physics.step(world, dt);
            const RigidBody& body = physics.body(id);
            Vec3 live = rotate(body.orientation, joint.axisA);
            worstTilt = std::max(worstTilt, length(cross(live, axis)));
            swept = std::max(swept, 8.0f - body.position.y);
            spinAboutAxle = std::max(spinAboutAxle, std::fabs(dot(body.angularVelocity, axis)));
        }

        std::printf("  hinge: axle tilted at most %.5f, swung %.2f m, spun %.2f rad/s about it\n",
                    worstTilt, swept, spinAboutAxle);
        check(worstTilt < 0.02f, "a hinge keeps its axle pointing where it was put");
        check(swept > 1.0f, "and lets the body turn about it");
        // Without this the previous two are satisfied by a joint that welds
        // everything solid: no tilt and a swing driven by the anchor alone.
        check(spinAboutAxle > 0.5f, "leaving rotation about the axle genuinely free");
    }

    // ------------------------------------------------------------- the weld
    {
        // A step under the left half only, so the welded bar lands on one end
        // and has to tip. Dropped onto flat ground the two cubes fall
        // identically and nothing ever pulls on the joint -- the test would
        // be measuring gravity, not the weld.
        World stepped = world;
        stepped.fillBox({-3, 0, 0}, {-1, 0, 1}, palette::Cobblestone);

        PhysicsWorld physics;
        int left = physics.add(makeBody({0.0f, 4.0f, 0.5f}));
        int right = physics.add(makeBody({1.05f, 4.0f, 0.5f}));

        Constraint weld = weldAt(physics.body(left), left, physics.body(right), right,
                                 Vec3{0.525f, 4.0f, 0.5f});
        physics.addConstraint(weld);

        // Measured in the left body's own frame: the bar is allowed to turn
        // as one piece, and a world-space offset would call that a failure.
        Vec3 startOffset = physics.body(right).position - physics.body(left).position;

        float worstStretch = 0.0f;
        float worstTwist = 0.0f;
        float turned = 0.0f;
        for (int i = 0; i < 900; ++i) {
            physics.step(stepped, dt);

            const RigidBody& l = physics.body(left);
            const RigidBody& r = physics.body(right);

            Vec3 offset = rotate(conjugate(l.orientation), r.position - l.position);
            worstStretch = std::max(worstStretch, length(offset - startOffset));

            Quat relative = conjugate(r.orientation) * l.orientation;
            if (relative.w < 0.0f) relative = relative * -1.0f;
            worstTwist = std::max(worstTwist, length(Vec3{relative.x, relative.y, relative.z}) * 2.0f);

            turned = std::max(turned, length(Vec3{l.orientation.x, l.orientation.y, l.orientation.z}) * 2.0f);
        }

        std::printf("  weld: offset drifted %.4f m, relative angle %.4f rad, bar turned %.3f rad, "
                    "y = %.3f/%.3f\n",
                    worstStretch, worstTwist, turned, physics.body(left).position.y,
                    physics.body(right).position.y);

        check(turned > 0.05f, "the stepped floor actually tipped the bar");
        check(worstStretch < 0.03f, "a weld holds the offset between two bodies");
        check(worstTwist < 0.05f, "and holds their relative angle while the pair turns");
        check(physics.body(left).position.y > physics.body(right).position.y,
              "the end on the step ends up higher");
        check(physics.body(left).sleeping && physics.body(right).sleeping,
              "and the welded island sleeps as one");
    }

    // ---------------------------------------------------------- and it breaks
    {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 9.0f, 0.0f};
        int id = physics.add(makeBody({0.0f, 8.5f, 0.0f}));

        Constraint rope = ropeBetween(physics.body(id), id, physics.body(id), -1,
                                      physics.body(id).position, anchor, 1.0f);
        // A 500 kg cube hanging still needs about m*g*dt of impulse a step,
        // which is roughly 40 N s. Half that cannot hold it.
        rope.breakImpulse = 20.0f;
        int jointId = physics.addConstraint(rope);

        for (int i = 0; i < 240; ++i) physics.step(world, dt);

        std::printf("  breaking: joint broken = %s, body fell to y = %.2f\n",
                    physics.constraint(jointId).broken ? "yes" : "no",
                    physics.body(id).position.y);
        check(physics.constraint(jointId).broken, "an overloaded joint gives way");
        check(physics.body(id).position.y < 7.0f, "and the body falls past where it hung");
    }
}

// --------------------------------------------------- friction and driving
// Friction is the difference between a sandbox and a perpetual motion museum,
// and it is stated here as a before-and-after rather than as "it settled":
// the same pendulum, twice, differing only in the friction figure.
void testJointFrictionAndMotors() {
    std::printf("joint friction and motors\n");

    World world(palette::registry());
    world.fillBox({-10, -1, -10}, {10, -1, 10}, palette::Stone);

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);
    const float dt = 1.0f / 120.0f;

    auto pendulum = [&](float friction, bool motor, float speed, float torque, int steps,
                        float* finalSpin, bool* asleep) {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 8.0f, 0.0f};

        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = {2.0f, 8.0f, 0.0f};
        physics.add(body);

        Constraint joint = hingeAt(physics.body(0), 0, physics.body(0), -1, anchor,
                                   Vec3{0.0f, 0.0f, 1.0f});
        joint.friction = friction;
        joint.motor = motor;
        joint.motorSpeed = speed;
        joint.motorTorque = torque;
        physics.addConstraint(joint);

        int sleptAt = -1;
        for (int i = 0; i < steps; ++i) {
            physics.step(world, dt);
            if (sleptAt < 0 && physics.body(0).sleeping) sleptAt = i;
        }

        if (finalSpin) *finalSpin = physics.body(0).angularVelocity.z;
        if (asleep) *asleep = physics.body(0).sleeping;
        return sleptAt;
    };

    // ------------------------------------------------------------ friction
    bool freeAsleep = false, dampedAsleep = false;
    // The frictionless one never sleeps, so it has no time to report.
    pendulum(0.0f, false, 0.0f, 0.0f, 2400, nullptr, &freeAsleep);
    int dampedSlept = pendulum(60.0f, false, 0.0f, 0.0f, 2400, nullptr, &dampedAsleep);

    std::printf("  frictionless: asleep = %s; with friction: asleep = %s after %.2f s\n",
                freeAsleep ? "yes" : "no", dampedAsleep ? "yes" : "no",
                dampedSlept >= 0 ? float(dampedSlept) * dt : -1.0f);

    check(!freeAsleep, "a frictionless hinge swings for ever, as it should");
    check(dampedAsleep, "friction brings the same hinge to rest");
    check(dampedSlept > 0 && float(dampedSlept) * dt < 12.0f, "and does it in a usable time");

    // ------------------------------------------------------------- a motor
    // Enough torque to carry the weight round and hold a speed.
    float spin = 0.0f;
    pendulum(0.0f, true, 4.0f, 3000.0f, 1200, &spin, nullptr);
    std::printf("  motor asked for 4.00 rad/s, got %.2f\n", spin);
    check(nearly(spin, 4.0f, 0.4f), "a motor reaches the speed it was asked for");

    // Reversed, to prove the sign is the axle's and not an accident.
    pendulum(0.0f, true, -4.0f, 3000.0f, 1200, &spin, nullptr);
    check(nearly(spin, -4.0f, 0.4f), "and turns the other way when asked");

    // A brake is a motor set to zero, and it must hold the arm still.
    bool braked = false;
    pendulum(0.0f, true, 0.0f, 3000.0f, 1200, &spin, &braked);
    check(std::fabs(spin) < 0.1f, "a motor at zero speed is a brake");

    // The torque limit is not decoration. With almost none, the motor cannot
    // hold the weight up and the arm falls regardless of what it was asked
    // for -- which is the property that stops a motor being a free lever.
    pendulum(0.0f, true, 4.0f, 8.0f, 600, &spin, nullptr);
    std::printf("  the same motor limited to 8 N s reached %.2f rad/s\n", spin);
    check(std::fabs(spin - 4.0f) > 1.0f, "a torque-limited motor cannot reach any speed it likes");

    // ---------------------------------------------------------- a hydraulic
    // A rod whose length is driven. There is no piston joint: a piston *is*
    // a rod somebody is changing.
    {
        PhysicsWorld physics;
        Vec3 anchor{0.0f, 9.0f, 0.0f};

        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = {0.0f, 7.0f, 0.0f};
        physics.add(body);

        Constraint rod = rodBetween(physics.body(0), 0, physics.body(0), -1,
                                    physics.body(0).position, anchor, 2.0f);
        rod.friction = 400.0f;
        int id = physics.addConstraint(rod);

        for (int i = 0; i < 240; ++i) physics.step(world, dt);
        float extended = length(physics.body(0).position - anchor);

        // Retract it, as a script driving the piston would.
        for (int i = 0; i < 480; ++i) {
            physics.constraint(id).distance =
                std::max(1.0f, physics.constraint(id).distance - 0.004f);
            physics.step(world, dt);
        }
        float retracted = length(physics.body(0).position - anchor);

        std::printf("  hydraulic: %.2f m extended, %.2f m retracted\n", extended, retracted);
        check(nearly(extended, 2.0f, 0.03f), "a rod holds its length hanging");
        check(nearly(retracted, 1.0f, 0.05f), "and follows the length it is driven to");
        check(physics.body(0).position.y > 7.5f, "pulling the body up with it");
    }
}

// ------------------------------------------------------ removing and reusing
//
// A sandbox spawns and deletes for as long as it is open, so the interesting
// claims are not "the body went away" but the two that follow from the slot
// being handed out again: nothing that named the old body still acts on the
// new one, and nothing the old body left behind is applied to it.
void testRemoval() {
    std::printf("removal and slot reuse\n");

    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);   // floor, top at y = 0

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);

    auto makeBody = [&](Vec3 at) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = at;
        body.friction = 0.7f;
        return body;
    };

    // ------------------------------------------------------- the bookkeeping
    {
        PhysicsWorld physics;
        int a = physics.add(makeBody({0.5f, 4.0f, 0.5f}));
        int b = physics.add(makeBody({4.5f, 4.0f, 0.5f}));
        int c = physics.add(makeBody({8.5f, 4.0f, 0.5f}));

        check(physics.bodyCount() == 3 && physics.liveBodyCount() == 3, "three bodies, three live");

        physics.remove(b);
        check(!physics.alive(b), "a removed body is not alive");
        check(physics.alive(a) && physics.alive(c), "and its neighbours are untouched");
        check(physics.liveBodyCount() == 2, "two live bodies remain");
        check(physics.bodyCount() == 3, "and the slot count does not shrink");

        int d = physics.add(makeBody({0.5f, 9.0f, 0.5f}));
        check(d == b, "the next body reuses the freed slot");
        check(physics.bodyCount() == 3, "so the slot count still does not grow");
        check(physics.liveBodyCount() == 3, "and it counts as live again");
    }

    // ------------------------------------------------------- the joint goes
    // A crate welded to a crate, then the one holding it is removed. The
    // survivor has to fall: a joint left behind naming a dead slot is a joint
    // that will name whatever is put there next.
    {
        PhysicsWorld physics;
        RigidBody anchorBody = makeBody({0.5f, 6.0f, 0.5f});
        setMassFromCollider(anchorBody, collider, 0.0f);   // immovable
        int anchor = physics.add(anchorBody);
        int hanging = physics.add(makeBody({0.5f, 4.9f, 0.5f}));

        physics.addConstraint(
            weldAt(physics.body(hanging), hanging, physics.body(anchor), anchor, {0.5f, 5.4f, 0.5f}));

        const float dt = 1.0f / 120.0f;
        for (int i = 0; i < 240; ++i) physics.step(world, dt);
        check(physics.body(hanging).position.y > 4.0f, "a welded crate hangs");

        physics.remove(anchor);
        for (int i = 0; i < 480; ++i) physics.step(world, dt);

        std::printf("  the crate its weld hung from was removed; it fell to y = %.2f\n",
                    physics.body(hanging).position.y);
        check(physics.body(hanging).position.y < 1.0f, "and falls once its anchor is removed");
        check(physics.body(hanging).sleeping, "landing and settling as any other body would");
    }

    // ------------------------------------------------- no inherited impulses
    // The one that needed the salt on the impulse key. A body that settled in
    // a slot leaves warm-start impulses behind under its name; without the
    // salt the next body in that slot is handed them before the first
    // iteration, and is fired off the floor by a force that was holding
    // somebody else up.
    {
        PhysicsWorld physics;
        int first = physics.add(makeBody({0.5f, 3.0f, 0.5f}));

        const float dt = 1.0f / 120.0f;
        for (int i = 0; i < 600; ++i) physics.step(world, dt);
        check(physics.body(first).sleeping, "the first body settles");

        physics.remove(first);
        int second = physics.add(makeBody({0.5f, 0.6f, 0.5f}));
        check(second == first, "the replacement takes the same slot");

        float highest = physics.body(second).position.y;
        for (int i = 0; i < 600; ++i) {
            physics.step(world, dt);
            highest = std::max(highest, physics.body(second).position.y);
        }

        std::printf("  a body spawned in a dead body's slot rose to at most y = %.3f\n", highest);
        check(highest < 0.75f, "and is not launched by the dead body's stored impulses");
        check(nearly(physics.body(second).position.y, 0.5f, 0.05f), "it just settles");
    }
}

// -------------------------------------------------------------------- frozen
//
// Freezing is a third state, and each of these fails differently if it is
// confused with one of the other two. Treated as sleeping, anything touching
// it wakes it; treated as static in the old sense, nothing can be stacked on
// it, because the broad phase used to skip every pair with an immovable body
// in it.
void testFreezing() {
    std::printf("freezing\n");

    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);

    auto makeBody = [&](Vec3 at) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = at;
        body.friction = 0.7f;
        return body;
    };

    PhysicsWorld physics;
    int platform = physics.add(makeBody({0.5f, 3.0f, 0.5f}));
    physics.setFrozen(platform, true);

    const float dt = 1.0f / 120.0f;
    for (int i = 0; i < 240; ++i) physics.step(world, dt);

    check(nearly(physics.body(platform).position.y, 3.0f, 1e-4f),
          "a frozen body hangs in the air");
    check(physics.body(platform).frozen, "and says so");

    // Something dropped on it. This is the one the old broad phase failed:
    // with static pairs skipped outright the crate passes through and lands
    // on the floor, which looks exactly like the freeze having worked.
    int crate = physics.add(makeBody({0.5f, 5.0f, 0.5f}));
    for (int i = 0; i < 600; ++i) physics.step(world, dt);

    std::printf("  a crate dropped on a frozen one rests at y = %.2f (floor would be 0.50)\n",
                physics.body(crate).position.y);
    check(physics.body(crate).position.y > 3.4f, "a crate lands on a frozen body");
    check(physics.body(crate).sleeping, "and settles on it");
    check(nearly(physics.body(platform).position.y, 3.0f, 1e-3f),
          "which does not push the frozen one down");

    // The control, and the test that found the manifold bug: the same two
    // cubes with no freezing anywhere near them.
    //
    // A crate resting on a crate is the plainest thing a sandbox does and it
    // did not work. Built from corners, a face-to-face manifold had nought to
    // two points instead of four, the upper cube tipped a little further every
    // step, and after four seconds it was inside the lower one. Both offsets
    // are here because the first suspicion -- corners exactly on a boundary
    // being a coin flip -- was only half of it: moving one cube sideways so
    // its corners are unambiguously inside did not help either.
    {
        for (float offset : {0.0f, 0.13f}) {
            PhysicsWorld plain;
            int lower = plain.add(makeBody({0.5f, 3.0f, 0.5f}));
            int upper = plain.add(makeBody({0.5f + offset, 4.0f, 0.5f}));
            for (int i = 0; i < 900; ++i) plain.step(world, dt);

            float gap = plain.body(upper).position.y - plain.body(lower).position.y;
            std::printf("  two cubes dropped together, offset %.2f: lower %.3f, upper %.3f\n",
                        offset, plain.body(lower).position.y, plain.body(upper).position.y);
            check(nearly(gap, 1.0f, 0.05f), "one cube comes to rest on top of the other");
            check(plain.body(upper).sleeping, "and both stop moving");
        }
    }

    // Letting go. The crate above is asleep and islands are deliberately not
    // shared through an immovable body, so nothing would tell it to fall --
    // which is why unfreezing wakes its neighbours by hand.
    physics.setFrozen(platform, false);
    check(!physics.body(crate).sleeping, "unfreezing wakes what was resting on it");

    for (int i = 0; i < 900; ++i) physics.step(world, dt);
    std::printf("  after letting go: platform y = %.2f, crate y = %.2f\n",
                physics.body(platform).position.y, physics.body(crate).position.y);
    check(physics.body(platform).position.y < 1.0f, "the unfrozen body falls");
    check(nearly(physics.body(crate).position.y - physics.body(platform).position.y, 1.0f, 0.05f),
          "and arrives with what was on it still on it");
    check(physics.body(platform).sleeping && physics.body(crate).sleeping,
          "both settle into a stack on the floor");
}

// ------------------------------------------------------------- joint limits
//
// A cone limit is the difference between a ragdoll and a bag of parts, and it
// is exactly the kind of joint test that passes vacuously: a limb that never
// swings far enough to reach its limit will agree with any limit at all. So
// each of these is run twice, once limited and once not, and the claim is the
// difference between the two.
void testJointLimits() {
    std::printf("joint limits\n");

    World world(palette::registry());   // empty: nothing here should ever touch the ground

    VoxelModel limbModel = solidBox({4, 12, 4});
    Collider limb = buildCollider(limbModel, 1.0f / 16.0f);
    VoxelModel anchorModel = solidBox({4, 4, 4});
    Collider anchorShape = buildCollider(anchorModel, 1.0f / 16.0f);

    // A limb hanging from an immovable block, kicked sideways. `cone` of zero
    // means no limit.
    auto swing = [&](float cone, float twist, Vec3 kick, Vec3 spin, float* maxBend,
                     float* maxTwist) {
        PhysicsWorld physics;

        RigidBody anchorBody;
        setMassFromCollider(anchorBody, anchorShape, 0.0f);
        anchorBody.voxelSize = 1.0f / 16.0f;
        anchorBody.position = {0.0f, 4.0f, 0.0f};
        int anchor = physics.add(anchorBody);

        RigidBody limbBody;
        setMassFromCollider(limbBody, limb, 900.0f);
        limbBody.voxelSize = 1.0f / 16.0f;
        limbBody.position = {0.0f, 4.0f - 12.0f / 32.0f, 0.0f};   // hanging, half its length down
        limbBody.linearVelocity = kick;
        limbBody.angularVelocity = spin;
        int hanging = physics.add(limbBody);

        Constraint joint = ballSocketAt(physics.body(hanging), hanging, physics.body(anchor),
                                        anchor, {0.0f, 4.0f, 0.0f});
        joint.axisB = {0.0f, -1.0f, 0.0f};
        joint.coneLimitDegrees = cone;
        joint.twistLimitDegrees = twist;
        physics.addConstraint(joint);

        const float dt = 1.0f / 120.0f;
        *maxBend = 0.0f;
        *maxTwist = 0.0f;

        for (int i = 0; i < 480; ++i) {
            physics.step(world, dt);
            const RigidBody& body = physics.body(hanging);

            // How far the limb hangs from straight down.
            Vec3 down = normalize(body.position - Vec3{0.0f, 4.0f, 0.0f});
            *maxBend = std::max(*maxBend, degrees(std::acos(std::clamp(-down.y, -1.0f, 1.0f))));

            // And how far it has turned about its own length, which for a
            // limb that is still hanging is the spin of its sideways axis.
            Vec3 side = rotate(body.orientation, Vec3{1.0f, 0.0f, 0.0f});
            *maxTwist = std::max(*maxTwist, degrees(std::acos(std::clamp(side.x, -1.0f, 1.0f))));
        }
    };

    // --------------------------------------------------------------- the cone
    {
        float freeBend = 0.0f, freeTwist = 0.0f, heldBend = 0.0f, heldTwist = 0.0f;
        swing(0.0f, 0.0f, {2.6f, 0.0f, 0.0f}, Vec3{}, &freeBend, &freeTwist);
        swing(25.0f, 0.0f, {2.6f, 0.0f, 0.0f}, Vec3{}, &heldBend, &heldTwist);

        std::printf("  kicked sideways: swung %.1f degrees free, %.1f with a 25 degree cone\n",
                    freeBend, heldBend);
        check(freeBend > 45.0f, "an unlimited ball socket lets the limb swing right up");
        check(heldBend < 33.0f, "a cone limit holds it near the angle it was given");
        check(heldBend > 15.0f, "without pinning it -- inside the cone there is no constraint");
    }

    // -------------------------------------------------------------- the twist
    {
        float freeBend = 0.0f, freeTwist = 0.0f, heldBend = 0.0f, heldTwist = 0.0f;
        swing(0.0f, 0.0f, Vec3{}, {0.0f, -7.0f, 0.0f}, &freeBend, &freeTwist);
        swing(0.0f, 20.0f, Vec3{}, {0.0f, -7.0f, 0.0f}, &heldBend, &heldTwist);

        std::printf("  spun about itself: turned %.1f degrees free, %.1f with a 20 degree limit\n",
                    freeTwist, heldTwist);
        check(freeTwist > 90.0f, "an unlimited ball socket lets it spin right round");
        check(heldTwist < 30.0f, "a twist limit stops it");
    }
}

// ----------------------------------------------------------------- picking
// Pointing at a body. What a tool needs, and separate from `intersectScene`
// because that one answers about the drawn set and hands back a material.
void testPicking() {
    std::printf("picking");
    std::printf("\n");

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);

    PhysicsWorld physics;
    auto place = [&](Vec3 at) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = at;
        return physics.add(body);
    };

    int near = place({0.0f, 0.0f, 0.0f});     // a one-metre cube at the origin
    int far = place({0.0f, 0.0f, -6.0f});

    BodyPick pick;
    check(pickBody(physics, Ray{{0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, -1.0f}}, 32.0f, pick),
          "a ray down the line of two cubes hits");
    check(pick.body == near, "and hits the near one");
    check(nearly(pick.distance, 3.5f, 1e-3f), "at its face rather than at its centre");
    check(nearly(pick.normal.z, 1.0f, 1e-3f), "with the normal pointing back along the ray");

    std::printf("  hit body %d at %.3f m, normal (%.1f, %.1f, %.1f)\n", pick.body, pick.distance,
                pick.normal.x, pick.normal.y, pick.normal.z);

    check(!pickBody(physics, Ray{{0.0f, 3.0f, 4.0f}, {0.0f, 0.0f, -1.0f}}, 32.0f, pick),
          "a ray over the top of both misses");
    check(!pickBody(physics, Ray{{0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, -1.0f}}, 2.0f, pick),
          "and one that stops short does not reach");

    // A turned body is hit on its turned face, which is the whole reason the
    // ray is pushed into the body's frame rather than tested against bounds.
    physics.body(near).orientation = Quat::axisAngle({0.0f, 1.0f, 0.0f}, radians(45.0f));
    check(pickBody(physics, Ray{{0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, -1.0f}}, 32.0f, pick),
          "a turned cube is still hit");
    check(pick.distance < 3.5f, "and its corner is nearer than its face was");

    physics.remove(near);
    check(pickBody(physics, Ray{{0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, -1.0f}}, 32.0f, pick) &&
              pick.body == far,
          "a removed body is not picked, and the one behind it is");
}

// ----------------------------------------------------------- on foot
// A character is steered, not simulated, so what it promises is different
// from what a body promises -- and every one of these is a thing a player
// would notice within a minute of walking around.
void testCharacter() {
    std::printf("character controller\n");

    World world(palette::registry());
    world.fillBox({-20, -1, -20}, {20, 0, 20}, palette::Stone);   // floor, top at y = 1

    CharacterSettings settings;
    const float dt = 1.0f / 120.0f;

    auto walk = [&](Character& who, Vec3 wish, bool jump, bool run, float seconds) {
        int steps = int(seconds / dt);
        for (int i = 0; i < steps; ++i) stepCharacter(who, world, settings, wish, jump, run, dt);
    };

    // --------------------------------------------------------- standing
    {
        Character who;
        who.position = {0.5f, 5.0f, 0.5f};
        walk(who, Vec3{}, false, false, 3.0f);

        std::printf("  fell to y = %.4f, on ground = %s\n", who.position.y,
                    who.onGround ? "yes" : "no");
        check(nearly(who.position.y, 1.0f, 0.01f), "a dropped character lands on the floor");
        check(who.onGround, "and knows it is standing");
        check(std::fabs(who.velocity.y) < 0.01f, "with no leftover fall");

        // The commonest way a controller is wrong: it settles a millimetre
        // deeper every step and after a minute you are in the floor.
        float settled = who.position.y;
        walk(who, Vec3{}, false, false, 10.0f);
        check(nearly(who.position.y, settled, 1e-4f), "and does not sink over ten more seconds");
    }

    // ------------------------------------------------------------ walls
    {
        world.fillBox({4, 1, -6}, {4, 4, 6}, palette::Stone);   // a wall at x = 4

        Character who;
        who.position = {0.5f, 1.0f, 0.5f};
        walk(who, Vec3{1.0f, 0.0f, 0.0f}, false, true, 4.0f);

        std::printf("  walked into a wall, stopped at x = %.3f\n", who.position.x);
        check(who.position.x < 4.0f, "walking into a wall does not go through it");
        check(who.position.x > 3.0f, "but does reach it");
        check(who.hitWall, "and reports the wall");
    }

    // ------------------------------------------------------ a single step
    {
        World stairs(palette::registry());
        stairs.fillBox({-20, -1, -20}, {20, 0, 20}, palette::Stone);
        stairs.fillBox({3, 1, -6}, {8, 1, 6}, palette::Stone);   // one block high

        Character who;
        who.position = {0.5f, 1.0f, 0.5f};

        // One second, not three. At 4.3 m/s three seconds crosses the whole
        // five-block ledge and walks off the far side -- which the first
        // version of this test did, then read the landing as a failure to
        // climb. *When* to look is part of the claim.
        float highest = who.position.y;
        int steps = int(1.0f / dt);
        for (int i = 0; i < steps; ++i) {
            stepCharacter(who, stairs, settings, Vec3{1.0f, 0.0f, 0.0f}, false, false, dt);
            highest = std::max(highest, who.position.y);
        }

        std::printf("  walked at a one-block ledge, now at (%.2f, %.2f), highest %.2f\n",
                    who.position.x, who.position.y, highest);
        check(who.position.y > 1.5f, "a one-block ledge is stepped onto without jumping");
        check(who.position.x > 3.5f, "and walked along afterwards");
        check(who.onGround, "standing on it");
    }

    // A two-block wall is not a step, and must not become one.
    {
        World tall(palette::registry());
        tall.fillBox({-20, -1, -20}, {20, 0, 20}, palette::Stone);
        tall.fillBox({3, 1, -6}, {8, 2, 6}, palette::Stone);

        Character who;
        who.position = {0.5f, 1.0f, 0.5f};
        int steps = int(3.0f / dt);
        for (int i = 0; i < steps; ++i)
            stepCharacter(who, tall, settings, Vec3{1.0f, 0.0f, 0.0f}, false, false, dt);

        check(who.position.y < 1.5f, "a two-block wall is not walked up");
        check(who.position.x < 3.0f, "and stops the character");
    }

    // ------------------------------------------------------------- jump
    {
        Character who;
        who.position = {-8.5f, 1.0f, 0.5f};

        float highest = who.position.y;
        int steps = int(2.0f / dt);
        for (int i = 0; i < steps; ++i) {
            stepCharacter(who, world, settings, Vec3{}, i < 2, false, dt);
            highest = std::max(highest, who.position.y);
        }

        std::printf("  jumped %.2f blocks and landed at y = %.3f\n", highest - 1.0f, who.position.y);
        check(highest - 1.0f > 1.05f, "a jump clears a block");
        check(highest - 1.0f < 2.0f, "but not two");
        check(nearly(who.position.y, 1.0f, 0.01f), "and lands back where it started");
    }

    // ----------------------------------------------------- terminal speed
    // The one place a swept box can be wrong and look fine for weeks: a fall
    // fast enough to cross a floor in a single step. The substepping exists
    // for this, so the test drops from high enough to reach terminal speed.
    {
        Character who;
        who.position = {0.5f, 400.0f, 0.5f};
        float lowest = who.position.y;

        int steps = int(20.0f / dt);
        for (int i = 0; i < steps; ++i) {
            stepCharacter(who, world, settings, Vec3{}, false, false, dt);
            lowest = std::min(lowest, who.position.y);
        }

        std::printf("  fell 400 blocks, lowest y = %.3f\n", lowest);
        check(nearly(who.position.y, 1.0f, 0.01f), "a 400-block fall still lands on the floor");
        check(lowest > 0.9f, "without ever passing through it");
    }

    // ------------------------------------------------------- placement
    {
        World hill(palette::registry());
        hill.fillBox({-8, -1, -8}, {8, 0, 8}, palette::Stone);
        hill.fillBox({-2, 1, -2}, {2, 4, 2}, palette::Stone);   // a lump, top at y = 5

        check(!characterFits(hill, settings, {0.5f, 2.0f, 0.5f}), "inside the lump does not fit");
        check(characterFits(hill, settings, {0.5f, 5.0f, 0.5f}), "on top of it does");

        Vec3 placed = dropToGround(hill, settings, {0.5f, 3.0f, 0.5f});
        std::printf("  dropped into the lump, came out at y = %.2f\n", placed.y);
        check(nearly(placed.y, 5.0f, 0.15f), "a spawn inside geometry comes out on top of it");
        check(characterFits(hill, settings, placed), "and fits where it was put");
    }
}

// ----------------------------------------------------------------- swimming
//
// Water is the case where "it looked fine" is least trustworthy, because
// every wrong version of it still moves the character up and down. So none of
// these check a position on its own:
//
//   - the plunge is bounded, which is what a drag with no entry clamp fails;
//   - holding nothing *sinks*, holding up *floats*, and floating settles at a
//     depth rather than climbing out of the water;
//   - the head comes out, which is a claim about the eye and not about the box;
//   - a bank can be climbed, which is the one the whole feature is for.
//
// A lake with a bank, built once and used by all of them.
void testSwimming() {
    std::printf("swimming\n");

    World world(palette::registry());
    world.fillBox({-24, -1, -24}, {24, 0, 24}, palette::Stone);   // ground, top at y = 1
    world.fillBox({-14, 1, -14}, {14, 16, 14}, palette::Stone);   // a plateau, top at y = 17
    world.fillBox({-5, 2, -5}, {5, 16, 5}, palette::Water);       // a basin cut into it

    // So: a bed at y = 2, fifteen blocks of water, a surface at y = 17, and
    // nine blocks of bank all the way round. Deep and wide on purpose -- a
    // shallow pool would let the bed be the reason a dive stops, and a narrow
    // bank would let walking off the far side be the reason a climb ends.
    CharacterSettings settings;
    const float dt = 1.0f / 120.0f;

    auto swim = [&](Character& who, Vec3 wish, bool jump, bool run, float seconds) {
        int steps = int(seconds / dt);
        for (int i = 0; i < steps; ++i) stepCharacter(who, world, settings, wish, jump, run, dt);
    };

    // -------------------------------------------------------- the splash
    // Falling in from height. Without the entry clamp the drag still stops
    // the fall, only twelve blocks lower -- at the bottom, which is exactly
    // the bug this is here for.
    {
        Character who;
        who.position = {0.5f, 60.0f, 0.5f};

        float lowest = who.position.y;
        int steps = int(4.0f / dt);
        for (int i = 0; i < steps; ++i) {
            stepCharacter(who, world, settings, Vec3{}, false, false, dt);
            lowest = std::min(lowest, who.position.y);
        }

        std::printf("  fell 43 blocks into water, plunged to y = %.2f (surface 17, bed 2)\n",
                    lowest);
        check(lowest > 9.0f, "a long fall into water stops in the water, not at the bed");
        check(!who.onGround, "so the character is not standing on anything");
        check(who.submersion > 0.9f, "and is under it");
    }

    // ---------------------------------------------------------- the sink
    // Let go of everything and you go down, slowly. Both halves matter: no
    // sink at all is a cork, and a fast one is a stone.
    {
        Character who;
        who.position = {0.5f, 10.0f, 0.5f};
        float startY = who.position.y;
        swim(who, Vec3{}, false, false, 2.0f);

        float rate = (startY - who.position.y) / 2.0f;
        std::printf("  hands off, sank at %.2f blocks/s\n", rate);
        check(rate > 0.05f, "a swimmer who does nothing sinks");
        check(rate < 1.0f, "but slowly -- this is water, not air");
        check(nearly(who.submersion, 1.0f, 1e-3f), "and is fully under while doing it");
    }

    // --------------------------------------------------------- the float
    // Holding up from deep water rises to the surface and *stays* there. The
    // second half is the one that fails when buoyancy is set above one: the
    // character keeps climbing and steps out onto the water.
    {
        Character who;
        who.position = {0.5f, 3.0f, 0.5f};
        swim(who, Vec3{}, true, false, 8.0f);

        float settled = who.position.y;
        swim(who, Vec3{}, true, false, 3.0f);

        std::printf("  held up: floated at y = %.2f, submersion %.2f, eyes %s\n", settled,
                    double(who.submersion), who.eyesUnderwater ? "under" : "out");
        check(settled > 15.0f, "holding up from the bed reaches the surface");
        check(nearly(who.position.y, settled, 0.05f), "and stays at a level rather than climbing");
        check(who.position.y < 17.0f, "without standing on top of the water");
        check(!who.eyesUnderwater, "with the head out of it");
        check(who.swimming, "and reports itself swimming");
    }

    // ------------------------------------------------------------ the dive
    // The run key means down, and has to beat the buoyancy that is holding
    // the swimmer up.
    {
        Character who;
        who.position = {0.5f, 15.5f, 0.5f};
        swim(who, Vec3{}, false, true, 3.0f);

        // Against the sink above: three seconds of doing nothing is about a
        // block. The claim is the ordering, not the number.
        std::printf("  dived %.2f blocks in 3 s, to y = %.2f\n", 15.5f - who.position.y,
                    who.position.y);
        check(15.5f - who.position.y > 4.0f, "the run key dives, far faster than sinking");
        check(who.eyesUnderwater, "and puts the head under");
        check(who.position.y > 2.0f, "and does not push through the bed");
    }

    // ------------------------------------------------------- slower in water
    // Not a number anybody can check against the real world; the claim is
    // only the ordering, which is the part a player feels.
    {
        Character wet;
        wet.position = {-4.5f, 10.0f, 0.5f};
        swim(wet, Vec3{1.0f, 0.0f, 0.0f}, false, false, 2.0f);
        float wetSpeed = length(Vec3{wet.velocity.x, 0.0f, wet.velocity.z});

        std::printf("  swam forward at %.2f blocks/s against a walk of %.2f\n", wetSpeed,
                    settings.walkSpeed);
        check(wetSpeed > 0.5f, "a swimmer moves");
        check(wetSpeed < settings.walkSpeed * 0.8f, "and is slower than a walker");
    }

    // -------------------------------------------------------- out of the pool
    // The one the whole feature exists for. Swim at the bank holding forward
    // and up, and end up standing on it -- the step-up has to be tried from
    // water as well as from ground, or the bank is a wall.
    //
    // Two seconds, not four: the bank is nine blocks wide and a walk crosses
    // it in two. Reading a fall off the far side as a failure to climb is the
    // same mistake the ledge test above documents, and it was made again here
    // before this line was written.
    {
        // Up is held *while in the water* and let go on land, which is what a
        // person does and what the previous version of this got wrong: held
        // throughout, the character climbs out and then jumps up and down on
        // the bank, and is airborne at the instant the test looks.
        Character who;
        who.position = {3.5f, 16.0f, 0.5f};
        for (int i = 0; i < int(2.0f / dt); ++i)
            stepCharacter(who, world, settings, Vec3{1.0f, 0.0f, 0.0f}, who.inWater, false, dt);

        std::printf("  swam at the bank, ended at (%.2f, %.2f), on ground = %s\n", who.position.x,
                    who.position.y, who.onGround ? "yes" : "no");
        check(who.position.y > 16.9f, "a swimmer climbs out onto the bank");
        check(who.position.x > 6.0f, "and walks off it");
        check(who.onGround, "standing on solid ground");
        check(!who.inWater, "and out of the water");
    }

    // --------------------------------------------------------- shallow water
    // Wading is not swimming, and the difference must not need a threshold to
    // get right: one block of water, feet on the bottom, jump is still a jump.
    {
        World shallow(palette::registry());
        shallow.fillBox({-20, -1, -20}, {20, 0, 20}, palette::Stone);
        shallow.fillBox({-6, 1, -6}, {6, 1, 6}, palette::Water);   // one block deep

        Character who;
        who.position = {0.5f, 1.0f, 0.5f};

        float highest = who.position.y;
        int steps = int(1.5f / dt);
        for (int i = 0; i < steps; ++i) {
            stepCharacter(who, shallow, settings, Vec3{}, i < 2, false, dt);
            highest = std::max(highest, who.position.y);
        }

        std::printf("  standing in one block of water, jumped %.2f blocks\n", highest - 1.0f);
        check(who.submersion > 0.4f, "one block of water is over half the box");
        check(highest - 1.0f > 1.0f, "and a jump out of it still clears a block");
        check(!who.swimming, "wading is not swimming");
    }

    // ------------------------------------------------------------- lava
    // The same code path, because a fluid is a fluid to the controller. What
    // this pins is that lava is one at all: it used to be listed only in the
    // collision check, so a body fell through it exactly as through air.
    {
        World pit(palette::registry());
        pit.fillBox({-8, -1, -8}, {8, 0, 8}, palette::Stone);
        pit.fillBox({-4, 1, -4}, {4, 4, 4}, palette::Lava);

        Character who;
        who.position = {0.5f, 20.0f, 0.5f};
        int steps = int(6.0f / dt);
        for (int i = 0; i < steps; ++i) stepCharacter(who, pit, settings, Vec3{}, false, false, dt);

        std::printf("  dropped into lava, floating at y = %.2f\n", who.position.y);
        check(who.inWater, "lava is a fluid to the controller too");
        check(who.position.y > 1.0f, "and holds a body up the same way");
    }
}

void testDeterminism() {
    std::printf("determinism\n");

    World world(palette::registry());
    world.fillBox({-8, -1, -8}, {8, -1, 8}, palette::Stone);
    world.set({2, 0, 2}, palette::Cobblestone);   // something to land unevenly on

    VoxelModel cube = solidBox({8, 8, 8});
    Collider collider = buildCollider(cube, 0.125f);

    Quat spin = Quat::axisAngle({0.3f, 1.0f, 0.2f}, radians(35.0f));
    DropResult first = drop(world, collider, {2.4f, 4.0f, 2.2f}, spin, Vec3{1.5f, 0.0f, -0.7f}, 6.0f, 1.0f / 120.0f);
    DropResult second = drop(world, collider, {2.4f, 4.0f, 2.2f}, spin, Vec3{1.5f, 0.0f, -0.7f}, 6.0f, 1.0f / 120.0f);

    check(std::memcmp(&first.position, &second.position, sizeof(Vec3)) == 0,
          "the same drop twice ends in bitwise the same place");
    check(std::memcmp(&first.orientation, &second.orientation, sizeof(Quat)) == 0,
          "and the same orientation");
    std::printf("  tumbling drop ended at (%.4f, %.4f, %.4f), asleep = %s\n",
                first.position.x, first.position.y, first.position.z,
                first.sleeping ? "yes" : "no");
}

} // namespace

int main() {
    testDecomposition();
    testMassProperties();
    testBodyToWorld();
    testContacts();
    testDropAndRest();
    testWarmStarting();
    testStack();
    testConstraints();
    testJointFrictionAndMotors();
    testJointLimits();
    testRemoval();
    testFreezing();
    testPicking();
    testCharacter();
    testSwimming();
    testDeterminism();

    if (gFailures == 0) {
        std::printf("\nall physics tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
