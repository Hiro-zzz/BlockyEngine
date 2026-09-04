// The sandbox spike: voxel props falling onto a voxel world, and the cost of
// keeping a chunked mesh up to date while blocks change.
//
// It exists to answer two questions that the rest of this engine cannot,
// because the rest of this engine builds a scene once and then renders it:
//
//   1. Can a `VoxelModel` be a rigid body without a new object model?
//      `Prop` already stores a position, an orientation and a uniform voxel
//      size, and `PropSet::addTransformed` already accepts a matrix. So the
//      physics writes transforms and nothing downstream knows the difference
//      -- the props here go through exactly the path a hand-placed one does.
//
//   2. Does meshing become proportional to what changed rather than to how
//      much world there is? The benchmark at the end edits one block and
//      remeshes only what `World::chunkStamp` says went stale.
//
// What is standing in the picture, left to right: a loose piece that tumbled
// onto a step, a stack of three crates, a crate on a rope, a plank on a hinge
// still swinging, and a plank welded across a crate that arrived as one
// object. Four of those are joints, which is the part a sandbox is actually
// made of -- bodies that fall are a demo, bodies that can be fastened
// together are a toy.
//
//   scene_spike           four stills of the drop, 900x560
//   scene_spike draft     the same, smaller and faster
//   scene_spike bench     the meshing numbers only, no render
//
// Needs no game files: every model here is built in code and the world
// renders with the flat palette.
#include "engine/core/png.hpp"
#include "engine/physics/constraint.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/gl/gl_resources.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

VoxelMaterial colour(float r, float g, float b, float roughness = 0.85f) {
    VoxelMaterial material;
    material.albedo = srgbToLinear(Vec3{r, g, b});
    material.roughness = roughness;
    return material;
}

// A crate. The bands are not decoration: a uniformly coloured cube tumbles
// invisibly, and the whole point of the picture is to show that it turned.
VoxelModel buildCrate() {
    VoxelModel model;
    model.resize({8, 8, 8});
    uint16_t body = model.addMaterial(colour(0.72f, 0.52f, 0.30f));
    uint16_t band = model.addMaterial(colour(0.34f, 0.26f, 0.20f));
    uint16_t top = model.addMaterial(colour(0.86f, 0.68f, 0.42f));

    for (int y = 0; y < 8; ++y) {
        for (int z = 0; z < 8; ++z) {
            for (int x = 0; x < 8; ++x) {
                bool edge = (x == 0 || x == 7) && (z == 0 || z == 7);
                uint16_t material = body;
                if (edge || y == 0 || y == 7) material = band;
                if (y == 7 && !edge) material = top;
                model.set({x, y, z}, material);
            }
        }
    }
    return model;
}

VoxelModel buildPlank() {
    VoxelModel model;
    model.resize({18, 3, 6});
    uint16_t wood = model.addMaterial(colour(0.60f, 0.44f, 0.26f));
    uint16_t dark = model.addMaterial(colour(0.42f, 0.30f, 0.18f));
    for (int y = 0; y < 3; ++y)
        for (int z = 0; z < 6; ++z)
            for (int x = 0; x < 18; ++x)
                model.set({x, y, z}, (x % 6 == 0) ? dark : wood);
    return model;
}

// An L, so that a shape whose centre of mass is not its bounding-box centre
// gets exercised. It should settle onto its long face, not balance.
VoxelModel buildEll() {
    VoxelModel model;
    model.resize({10, 10, 5});
    uint16_t stone = model.addMaterial(colour(0.55f, 0.57f, 0.60f));
    uint16_t moss = model.addMaterial(colour(0.36f, 0.48f, 0.32f));
    for (int y = 0; y < 10; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 10; ++x)
                if (x < 4 || y < 4) model.set({x, y, z}, (y == 9 || x == 9) ? moss : stone);
    return model;
}

void buildGround(World& world) {
    world.fillBox({-20, -2, -20}, {20, -1, 20}, palette::Stone);
    world.fillBox({-20, 0, -20}, {20, 0, 20}, palette::GrassBlock);

    // A step, so that not every landing is the same event.
    world.fillBox({-9, 1, -3}, {-6, 1, 0}, palette::Cobblestone);

    // A gantry to hang things from. Joints need somewhere to be anchored, and
    // an anchor floating in mid-air reads as a bug rather than as a rope.
    world.fillBox({6, 1, 1}, {6, 6, 1}, palette::OakLog);
    world.fillBox({0, 6, 1}, {6, 6, 1}, palette::OakLog);
}

struct Piece {
    const VoxelModel* model = nullptr;
    Vec3 tint{1.0f, 1.0f, 1.0f};
    int body = -1;
};

}  // namespace

int main(int argc, char** argv) {
    bool draft = false, benchOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        if (std::strcmp(argv[i], "bench") == 0) benchOnly = true;
    }

    Scene scene(palette::registry());
    buildGround(scene.world);

    // ------------------------------------------------------- meshing bench
    // On a world the size of a map rather than the size of this demo -- the
    // ratio is the whole point, and eight chunks would flatter it into
    // meaninglessness.
    {
        World map(palette::registry());
        map.fillBox({-48, -3, -48}, {47, 8, 47}, palette::Stone);
        map.fillBox({-48, 9, -48}, {47, 9, 47}, palette::GrassBlock);
        for (int x = -40; x < 40; x += 7)
            for (int z = -40; z < 40; z += 9)
                map.fillBox({x, 10, z}, {x + 3, 13, z + 2}, palette::OakLog);

        // What the viewport does today: the whole world, once, before the loop.
        MeshData opaque, translucent;
        double start = now();
        buildWorldMesh(map, opaque, translucent);
        double fullSeconds = now() - start;

        std::printf("[spike] bench map: %zu chunks, %zu triangles\n", map.chunkCount(),
                    (opaque.indices.size() + translucent.indices.size()) / 3);
        std::printf("[spike] full mesh: %.1f ms\n", fullSeconds * 1000.0);

        // What a sandbox needs: one block changes, and only what went stale
        // is rebuilt. The stamps are read exactly as ChunkMeshCache reads them.
        std::vector<uint64_t> before;
        map.forEachChunk([&](IVec3, const World::Chunk& chunk) { before.push_back(chunk.stamp); });

        // On a chunk boundary, so the measurement pays for the neighbours the
        // rule drags in rather than reporting the best case.
        map.set({15, 10, 15}, palette::Bricks);

        int stale = 0;
        double meshSeconds = 0.0;
        MeshData chunkOpaque, chunkTranslucent;
        size_t index = 0;

        map.forEachChunk([&](IVec3, const World::Chunk& chunk) {
            bool changed = index >= before.size() || before[index] != chunk.stamp;
            ++index;
            if (!changed) return;

            chunkOpaque.clear();
            chunkTranslucent.clear();
            double t0 = now();
            appendChunkMesh(map, chunk, chunkOpaque, chunkTranslucent);
            meshSeconds += now() - t0;
            ++stale;
        });

        std::printf("[spike] boundary edit: %d chunk(s) stale, %.3f ms to remesh -- %.0fx cheaper\n",
                    stale, meshSeconds * 1000.0,
                    meshSeconds > 0.0 ? fullSeconds / meshSeconds : 0.0);

        // The same measurement for a block that is not near a boundary. The
        // difference between the two is the entire cost of the neighbour
        // rule, and it is worth quoting both: the speedup here is just
        // (chunks in the map) / (chunks the rule drags in), and nothing
        // cleverer, so it grows with the map and shrinks with the rule.
        before.clear();
        map.forEachChunk([&](IVec3, const World::Chunk& chunk) { before.push_back(chunk.stamp); });
        map.set({24, 5, 24}, palette::Bricks);

        stale = 0;
        meshSeconds = 0.0;
        index = 0;
        map.forEachChunk([&](IVec3, const World::Chunk& chunk) {
            bool changed = index >= before.size() || before[index] != chunk.stamp;
            ++index;
            if (!changed) return;

            chunkOpaque.clear();
            chunkTranslucent.clear();
            double t0 = now();
            appendChunkMesh(map, chunk, chunkOpaque, chunkTranslucent);
            meshSeconds += now() - t0;
            ++stale;
        });

        std::printf("[spike] interior edit: %d chunk(s) stale, %.3f ms to remesh -- %.0fx cheaper\n",
                    stale, meshSeconds * 1000.0,
                    meshSeconds > 0.0 ? fullSeconds / meshSeconds : 0.0);
    }

    // ------------------------------------------------------------- physics
    VoxelModel crate = buildCrate();
    VoxelModel plank = buildPlank();
    VoxelModel ell = buildEll();

    Collider crateCollider = buildCollider(crate, 0.125f);
    Collider plankCollider = buildCollider(plank, 0.125f);
    Collider ellCollider = buildCollider(ell, 0.125f);

    std::printf("[spike] colliders: crate %zu boxes, plank %zu, ell %zu\n",
                crateCollider.boxes.size(), plankCollider.boxes.size(), ellCollider.boxes.size());

    PhysicsWorld physics;
    std::vector<Piece> pieces;

    auto place = [&](const VoxelModel& model, const Collider& collider, Vec3 at, Quat spin,
                     Vec3 velocity, Vec3 angular, Vec3 tint) {
        RigidBody body;
        setMassFromCollider(body, collider, 500.0f);
        body.voxelSize = 0.125f;
        body.position = at;
        body.orientation = spin;
        body.linearVelocity = velocity;
        body.angularVelocity = angular;
        body.friction = 0.65f;
        body.restitution = 0.05f;

        Piece piece;
        piece.model = &model;
        piece.tint = tint;
        piece.body = physics.add(body);
        pieces.push_back(piece);
        return piece.body;
    };

    // A stack, which is what body-against-body and warm starting exist for.
    for (int i = 0; i < 3; ++i) {
        place(crate, crateCollider, {-3.0f, 1.55f + float(i) * 1.02f, -1.0f}, Quat::identity(),
              Vec3{}, Vec3{}, {1.0f, 1.0f, 1.0f});
    }

    // A crate on a rope, tied to the gantry. The rope is one-sided, so it
    // falls freely until the line goes taut.
    {
        Vec3 anchor{1.4f, 5.9f, 1.5f};
        int id = place(crate, crateCollider, {1.4f, 4.6f, 1.5f}, Quat::identity(),
                       {1.4f, 0.0f, 0.0f}, Vec3{}, {0.72f, 0.85f, 1.0f});
        Constraint rope = ropeBetween(physics.body(id), id, physics.body(id), -1,
                                      physics.body(id).position, anchor, 2.2f);
        physics.addConstraint(rope);
    }

    // A plank hinged to the gantry like a sign: free to swing about Z, held
    // against everything else.
    {
        Vec3 anchor{4.4f, 5.9f, 1.5f};
        int id = place(plank, plankCollider, {4.4f, 4.8f, 1.5f},
                       Quat::axisAngle({0.0f, 0.0f, 1.0f}, radians(90.0f)), {2.2f, 0.0f, 0.0f},
                       Vec3{}, {1.0f, 1.0f, 1.0f});
        physics.addConstraint(hingeAt(physics.body(id), id, physics.body(id), -1, anchor,
                                      Vec3{0.0f, 0.0f, 1.0f}));
    }

    // A plank welded across a crate, dropped as one piece. Nothing holds the
    // two together except the joint, so if it were wrong they would arrive
    // separately.
    {
        int base = place(crate, crateCollider, {8.2f, 5.0f, -1.4f}, Quat::identity(), Vec3{},
                         {0.0f, 0.6f, 0.0f}, {1.0f, 1.0f, 1.0f});
        int arm = place(plank, plankCollider, {8.2f, 5.7f, -1.4f}, Quat::identity(), Vec3{},
                        {0.0f, 0.6f, 0.0f}, {0.85f, 0.9f, 1.0f});
        physics.addConstraint(weldAt(physics.body(arm), arm, physics.body(base), base,
                                     Vec3{8.2f, 5.4f, -1.4f}));
    }

    // And one loose piece, tumbling onto the step.
    place(ell, ellCollider, {-7.4f, 6.4f, -1.6f}, Quat::axisAngle({1.0f, 0.2f, 0.0f}, radians(48.0f)),
          {0.4f, 0.0f, 0.5f}, {1.8f, 0.0f, 0.5f}, {1.0f, 1.0f, 1.0f});

    const float dt = 1.0f / 120.0f;
    const float shots[4] = {0.0f, 0.45f, 0.95f, 4.0f};

    // ------------------------------------------------------------- assemble
    scene.camera.lookAt({4.0f, 5.2f, 12.5f}, {0.4f, 2.5f, -0.5f});
    scene.camera.fovY = radians(56.0f);
    scene.camera.aspect = draft ? (720.0f / 450.0f) : (900.0f / 560.0f);

    scene.sun.direction = normalize(Vec3{-0.42f, 0.72f, 0.55f});
    scene.sun.color = {1.0f, 0.94f, 0.82f};
    scene.sun.intensity = 6.5f;
    scene.sun.angularRadiusDegrees = 1.2f;
    scene.sky.intensity = 1.0f;
    scene.ambientStrength = 0.9f;

    PathSettings settings;
    settings.width = draft ? 720 : 900;
    settings.height = draft ? 450 : 560;
    settings.samplesPerPixel = draft ? 24 : 64;
    settings.maxBounces = draft ? 4 : 6;

    double simulated = 0.0f;
    double physicsSeconds = 0.0;
    int steps = 0;
    int maxContacts = 0;

    // When each piece first fell asleep. "Comes to rest" is the claim the
    // whole solver is built around, and a body that is merely slow at the
    // moment the picture is taken has not made it -- so the moment is
    // recorded rather than the end state.
    std::vector<double> sleptAt(pieces.size(), -1.0);
    auto noteSleepers = [&]() {
        for (size_t i = 0; i < pieces.size(); ++i)
            if (sleptAt[i] < 0.0 && physics.body(pieces[i].body).sleeping) sleptAt[i] = simulated;
    };

    // Steps where something was actually moving, kept apart from the total.
    // Averaging over a run that ends with everything asleep measures the
    // sleeping, not the solving, and would report a flattering number for
    // work that was never done.
    int activeSteps = 0;
    double activeSeconds = 0.0;

    for (int shot = 0; shot < 4; ++shot) {
        while (simulated < double(shots[shot]) - 1e-6) {
            double t0 = now();
            physics.step(scene.world, dt);
            double spent = now() - t0;
            physicsSeconds += spent;
            if (physics.stats().awakeBodies > 0) { activeSeconds += spent; ++activeSteps; }
            simulated += double(dt);
            ++steps;
            maxContacts = std::max(maxContacts, physics.stats().contacts);
            noteSleepers();
        }

        // Every piece goes into the set the ordinary way. The only thing the
        // physics contributes is the matrix, which is the whole claim.
        PropSet props;
        for (const Piece& piece : pieces)
            props.addTransformed(piece.model, bodyToWorld(physics.body(piece.body)), piece.tint);
        props.build();
        scene.props = &props;

        RenderStats stats;
        RenderTargets targets;
        Image frame = renderPath(scene, settings, &stats, &targets);
        frame = denoise(targets, {});

        BloomSettings bloom;
        bloom.threshold = 1.3f;
        bloom.intensity = 0.05f;
        applyBloom(frame, bloom);

        GradeSettings grade;
        grade.contrast = 1.04f;
        grade.saturation = 1.05f;
        applyGrade(frame, grade);
        applyVignette(frame, {});

        ToneParams tone;
        tone.curve = Tonemap::ACES;

        char path[128];
        std::snprintf(path, sizeof(path), "out/spike_%s%d.png", draft ? "draft_" : "", shot);

        if (!benchOnly) {
            pngSave(path, frame, tone, nullptr);
            std::printf("[spike] t = %.2f s -> %s\n", shots[shot], path);
        }
        scene.props = nullptr;
    }

    // Keep stepping past the last picture, so "did not settle" can be told
    // apart from "had not settled yet when the shutter closed".
    while (simulated < 20.0) {
        double t0 = now();
        physics.step(scene.world, dt);
        double spent = now() - t0;
        physicsSeconds += spent;
        if (physics.stats().awakeBodies > 0) { activeSeconds += spent; ++activeSteps; }
        simulated += double(dt);
        ++steps;
        noteSleepers();
    }

    int asleep = 0;
    for (const Piece& piece : pieces)
        if (physics.body(piece.body).sleeping) ++asleep;

    std::printf("[spike] %d steps of %.4f s: %.1f ms total; %d with a body awake, %.4f ms each\n", steps, dt,
                physicsSeconds * 1000.0, activeSteps,
                activeSteps ? activeSeconds * 1000.0 / activeSteps : 0.0);
    std::printf("[spike] peak %d contacts, %d of %zu pieces asleep at the end\n", maxContacts, asleep,
                pieces.size());

    // The lowest corner of the collider in world space, which is the number
    // that says whether anything ended up inside the ground. Computed from
    // the rotated boxes rather than from the local bounds, because a turned
    // body's lowest point is not the lowest point of its rest pose.
    for (size_t i = 0; i < pieces.size(); ++i) {
        const RigidBody& body = physics.body(pieces[i].body);
        Mat3 rotation = toMat3(body.orientation);

        float lowest = body.position.y;
        for (const ColliderBox& box : body.collider->boxes) {
            for (int corner = 0; corner < 8; ++corner) {
                Vec3 local{box.center.x + ((corner & 1) ? box.halfExtents.x : -box.halfExtents.x),
                           box.center.y + ((corner & 2) ? box.halfExtents.y : -box.halfExtents.y),
                           box.center.z + ((corner & 4) ? box.halfExtents.z : -box.halfExtents.z)};
                lowest = std::min(lowest, (body.position + rotation * local).y);
            }
        }

        std::printf("[spike]   piece %zu at (%6.2f, %5.2f, %6.2f), lowest corner y = %5.3f, "
                    "|v| = %.4f, |w| = %.4f, %s\n",
                    i, body.position.x, body.position.y, body.position.z, lowest,
                    length(body.linearVelocity), length(body.angularVelocity),
                    body.sleeping ? "asleep" : "AWAKE");
        if (sleptAt[i] >= 0.0)
            std::printf("[spike]            fell asleep at t = %.2f s\n", sleptAt[i]);
        else
            std::printf("[spike]            never settled in 20 s\n");
    }
    return 0;
}
