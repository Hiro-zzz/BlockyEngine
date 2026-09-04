// The spike's contraption again -- but the world, the props and the joints
// are all built by `scenes/scripts/demo.bly` rather than by this file.
//
// What this file still does is everything a script has no business doing:
// owning the models, owning the physics world, deciding the camera, and
// rendering. The script decides what exists and what happens to it. That is
// the split the whole scripting layer is for: the half that changes every
// five minutes stops needing a compiler.
//
//   scene_sandbox              four stills, 900x560
//   scene_sandbox draft        smaller and faster
//   scene_sandbox script=<f>   run a different script
//   scene_sandbox check        run the script and report, without rendering
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/bindings.hpp"
#include "engine/script/script.hpp"
#include "scenes/common/palette.hpp"

#include "common/props.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

int main(int argc, char** argv) {
    bool draft = false, checkOnly = false;
    std::string path = "scenes/scripts/demo.bly";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else if (std::strcmp(argv[i], "check") == 0) checkOnly = true;
        else if (std::strncmp(argv[i], "script=", 7) == 0) path = argv[i] + 7;
    }

    // ------------------------------------------------------------ the host
    Scene scene(palette::registry());
    PhysicsWorld physics;

    VoxelModel crate = demo::buildCrate();
    VoxelModel plank = demo::buildPlank();
    VoxelModel ell = demo::buildEll();

    Skin skin = demo::loadDemoSkin();

    script::Sandbox sandbox;
    sandbox.world = &scene.world;
    sandbox.physics = &physics;
    sandbox.addModel("crate", crate, 0.125f);
    sandbox.addModel("plank", plank, 0.125f);
    sandbox.addModel("ell", ell, 0.125f);
    sandbox.addSkin("player", skin);

    script::Script program;
    program.onPrint = [](const std::string& text) { std::printf("[script] %s\n", text.c_str()); };

    // Reinstalled on every load, so a reload cannot end up with a different
    // set of verbs than the load before it.
    program.onBind = [&sandbox](script::Interpreter& vm) {
        script::installSandboxLibrary(vm, sandbox);
    };

    if (!program.loadFile(path)) {
        std::printf("[sandbox] %s\n", program.lastError().c_str());
        return 1;
    }
    std::printf("[sandbox] loaded %s\n", path.c_str());

    if (!program.call("ready")) {
        std::printf("[sandbox] %s\n", program.lastError().c_str());
        return 1;
    }

    // ----------------------------------------------------------- the frames
    const float dt = 1.0f / 120.0f;
    const float shots[4] = {0.0f, 0.5f, 1.5f, 5.0f};

    scene.camera.lookAt({5.0f, 7.0f, 15.5f}, {0.0f, 4.2f, -0.5f});
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

    double simulated = 0.0;
    int steps = 0;

    for (int shot = 0; shot < 4; ++shot) {
        while (simulated < double(shots[shot]) - 1e-6) {
            physics.step(scene.world, dt);
            simulated += double(dt);
            ++steps;

            // A script error inside `tick` stops the script, not the world.
            // The physics keeps running and the frames still come out, which
            // is the behaviour a sandbox needs on the afternoon someone is
            // writing a tool.
            if (!program.call("tick", {script::Value::num(double(dt))})) {
                std::printf("[sandbox] %s\n", program.lastError().c_str());
                std::printf("[sandbox] the world keeps running without it\n");
                break;
            }
        }

        // Whatever the script spawned, drawn the ordinary way: the renderer
        // never learns that a script was involved.
        PropSet props;
        for (const script::Sandbox::Spawned& piece : sandbox.spawned) {
            const script::Sandbox::Model& model = *sandbox.models[piece.model];
            props.addTransformed(model.model, bodyToWorld(physics.body(piece.body)), piece.tint);
        }
        props.build();
        scene.props = &props;

        EntitySet entities;
        for (const Entity& entity : sandbox.entities) entities.add(entity);
        scene.entities = entities.empty() ? nullptr : &entities;

        if (!checkOnly) {
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

            char out[128];
            std::snprintf(out, sizeof(out), "out/sandbox_%s%d.png", draft ? "draft_" : "", shot);
            pngSave(out, frame, tone, nullptr);
            std::printf("[sandbox] t = %.2f s -> %s\n", shots[shot], out);
        }
        scene.props = nullptr;
        scene.entities = nullptr;
    }

    int asleep = 0;
    for (const script::Sandbox::Spawned& piece : sandbox.spawned)
        if (physics.body(piece.body).sleeping) ++asleep;

    std::printf("[sandbox] %zu chunks, %d bodies (%d asleep), %d constraints, %d steps\n",
                scene.world.chunkCount(), physics.bodyCount(), asleep, physics.constraintCount(),
                steps);
    std::printf("[sandbox] script heap: %zu objects\n", program.heapObjects());
    return 0;
}
