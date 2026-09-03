// The sandbox, live in a window: physics running, and the script reloaded
// from disk the moment it changes.
//
// This is the loop the whole scripting layer was built for. Leave it running,
// edit `scenes/scripts/demo.bly` in another window, save -- and the world is
// rebuilt without the process restarting, the GL context being recreated, or
// the camera moving. A syntax error costs a line in the title bar and nothing
// else; the world that was already standing keeps standing.
//
// What each side owns is worth stating, because the split is the design:
//
//   - the **script** decides what exists and what happens to it;
//   - this file owns the models, the physics world and the camera;
//   - the **viewport** owns the frame, and calls back once per frame so the
//     two above can get on with it.
//
//   scene_live                      the window
//   scene_live script=<file>        a different script
//   scene_live settle=3             simulate N seconds before the first frame
//   scene_live snapshot <png>       a frame to a file, no window
//   scene_live frames=N             run N frames before that snapshot
#include "engine/entity/entity.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/gl/viewport.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/bindings.hpp"
#include "engine/script/script.hpp"
#include "scenes/common/palette.hpp"

#include "common/props.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace blocky;

int main(int argc, char** argv) {
    std::string path = "scenes/scripts/demo.bly";
    std::string snapshot;
    double settle = 0.0;
    int snapshotFrames = 1;

    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "script=", 7) == 0) path = argv[i] + 7;
        else if (std::strncmp(argv[i], "settle=", 7) == 0) settle = std::atof(argv[i] + 7);
        else if (std::strcmp(argv[i], "snapshot") == 0 && i + 1 < argc) snapshot = argv[++i];
        else if (std::strncmp(argv[i], "frames=", 7) == 0) snapshotFrames = std::atoi(argv[i] + 7);
    }

    // ------------------------------------------------------------- the host
    Scene scene(palette::registry());
    PhysicsWorld physics;

    VoxelModel crate = demo::buildCrate();
    VoxelModel plank = demo::buildPlank();
    VoxelModel ell = demo::buildEll();

    // The project ships one skin; without it a built-in figure is used, so
    // the sandbox never fails to start over a missing asset.
    Skin skin = demo::loadDemoSkin();

    script::Sandbox sandbox;
    sandbox.world = &scene.world;
    sandbox.physics = &physics;
    sandbox.addModel("crate", crate, 0.125f);
    sandbox.addModel("plank", plank, 0.125f);
    sandbox.addModel("ell", ell, 0.125f);
    sandbox.addSkin("player", skin);

    std::string status;

    script::Script program;
    program.onPrint = [](const std::string& text) { std::printf("[script] %s\n", text.c_str()); };
    program.onBind = [&sandbox](script::Interpreter& vm) {
        script::installSandboxLibrary(vm, sandbox);
    };

    // Everything the script built is thrown away and built again. That is a
    // policy of *this host*, not of the script layer: the language only
    // promises to free its own heap. Here the script owns the contents of the
    // world, so a reload has to start from an empty one -- otherwise editing
    // a number would add a second contraption beside the first.
    auto rebuild = [&]() {
        scene.world.clear();
        physics.clear();
        sandbox.spawned.clear();
        sandbox.entities.clear();

        if (!program.call("ready")) {
            status = program.lastError();
            std::printf("[live] %s\n", status.c_str());
            return false;
        }
        return true;
    };

    if (!program.loadFile(path)) {
        std::printf("[live] %s\n", program.lastError().c_str());
        return 1;
    }
    if (!rebuild()) return 1;

    std::printf("[live] %s: %d bodies, %d joints, %zu chunks\n", path.c_str(), physics.bodyCount(),
                physics.constraintCount(), scene.world.chunkCount());

    // The viewport adopts this as its starting position and the user flies
    // from there; a reload does not move it, which is half of what makes the
    // edit loop usable.
    scene.camera.lookAt({5.0f, 7.0f, 15.5f}, {0.0f, 4.2f, -0.5f});
    scene.camera.fovY = radians(56.0f);

    scene.sun.direction = normalize(Vec3{-0.42f, 0.72f, 0.55f});
    scene.sun.color = {1.0f, 0.94f, 0.82f};
    scene.sun.intensity = 6.5f;
    scene.ambientStrength = 0.9f;

    // ---------------------------------------------------------- the frame
    const float kStep = 1.0f / 120.0f;
    double accumulator = 0.0;
    int reloads = 0;

    // Both sets are rebuilt every frame and must outlive the loop, because
    // the scene holds pointers to them.
    //
    // Props come from the solver; entities do not. An entity is not simulated,
    // it is *posed*, so its state lives in the sandbox and this only flattens
    // it -- which `EntitySet::add` has to redo anyway, since it bakes the pose
    // into world-space boxes.
    PropSet props;
    EntitySet entities;

    auto refreshDrawables = [&]() {
        props.clear();
        for (const script::Sandbox::Spawned& piece : sandbox.spawned) {
            if (piece.body < 0 || piece.body >= physics.bodyCount()) continue;
            const script::Sandbox::Model& model = *sandbox.models[piece.model];
            props.addTransformed(model.model, bodyToWorld(physics.body(piece.body)), piece.tint);
        }
        props.build();
        scene.props = props.empty() ? nullptr : &props;

        entities.clear();
        for (const Entity& entity : sandbox.entities) entities.add(entity);
        scene.entities = entities.empty() ? nullptr : &entities;
    };

    auto advance = [&](float dt) {
        accumulator += double(dt);

        // A fixed step, and a ceiling on how many of them one frame may take.
        // Without the ceiling a long stall asks for the time it missed, which
        // takes longer than a frame, which makes the next stall worse.
        int steps = 0;
        while (accumulator >= double(kStep) && steps < 8) {
            physics.step(scene.world, kStep);
            accumulator -= double(kStep);
            ++steps;

            if (program.hasHandler("tick") &&
                !program.call("tick", {script::Value::num(double(kStep))})) {
                status = program.lastError();
                std::printf("[live] %s\n", status.c_str());
                std::printf("[live] the world keeps running without it\n");
                break;
            }
        }
        refreshDrawables();
    };

    for (double t = 0.0; t < settle; t += double(kStep)) advance(kStep);
    refreshDrawables();

    ViewportSettings viewport;
    viewport.title = "BlockyEngine live";
    viewport.snapshotPath = snapshot;
    viewport.snapshotFrames = snapshotFrames;
    viewport.statusText = &status;

    viewport.onFrame = [&](const ViewportFrame& frame) {
        // Polled rather than watched. A script is a page long, so reading it
        // costs less than the syscall that would tell us whether to bother --
        // and comparing contents rather than a modification time avoids every
        // way an editor can lie about when it saved.
        bool changed = false;
        if (!program.reloadIfChanged(&changed)) {
            status = program.lastError();
            std::printf("[live] %s\n", status.c_str());
            // Deliberately not fatal, and deliberately no rebuild: the script
            // that was already running is still loaded and still correct.
        } else if (changed) {
            ++reloads;
            if (rebuild()) {
                status = "reloaded (" + std::to_string(reloads) + ")";
                std::printf("[live] reloaded: %d bodies, %d joints, %zu chunks\n",
                            physics.bodyCount(), physics.constraintCount(),
                            scene.world.chunkCount());
            }
            accumulator = 0.0;
        }

        // A snapshot run advances by a fixed amount per frame rather than by
        // the wall clock. Without a swap to pace it the loop spins at
        // microseconds a frame, so wall time would never accumulate a single
        // step -- and a picture that depended on how fast the machine spun
        // would not be a test of anything.
        advance(snapshot.empty() ? frame.dt : kStep * 2.0f);
        return true;
    };

    if (snapshot.empty()) {
        std::printf("\n[live] editing %s and saving rebuilds the world in place.\n\n", path.c_str());
    }
    return runViewport(scene, viewport);
}
