#pragma once
// What a sandbox script can reach: the world, the bodies, the joints.
//
// This is the only file in `script/` that knows a voxel exists. The language
// below it can be built and tested without a world, the same way `core` knows
// nothing about blocks -- and it means a second host (an editor, a test rig)
// binds its own verbs without touching the interpreter.
//
// Handles rather than objects. A script holds an opaque id for a body, not a
// pointer into `PhysicsWorld`, so a script keeping a reference across a step
// cannot be looking at freed memory or at a body that was replaced -- the
// worst it can do is name something that no longer exists, which every
// binding checks for and reports.
#include "engine/entity/entity.hpp"
#include "engine/entity/model.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/script/interp.hpp"
#include "engine/world/world.hpp"

#include <memory>
#include <string>
#include <vector>

namespace blocky::script {

// Handle kinds, so a joint id cannot be passed where a body is wanted.
inline constexpr uint32_t kHandleBody = 1;
inline constexpr uint32_t kHandleJoint = 2;
inline constexpr uint32_t kHandleEntity = 3;

// Everything the bindings act on. The host fills this in and keeps it alive
// for as long as the script can run.
struct Sandbox {
    World* world = nullptr;
    PhysicsWorld* physics = nullptr;

    // Models and skins a script may spawn, by name. The host registers them;
    // a script cannot invent geometry, which is deliberate for a first pass --
    // voxel authoring from script is a separate question from driving a world.
    //
    // Held behind pointers rather than by value, and that is not a style
    // choice. A `RigidBody` points at a `Collider` that lives in here, and an
    // `Entity` at an `EntityModel`; a vector that reallocated when a later
    // model was registered would leave every one of them pointing at freed
    // memory, and the symptom would arrive much later than the cause.
    struct Model {
        std::string name;
        const VoxelModel* model = nullptr;   // owned by the host
        Collider collider;                   // built once, here
        float voxelSize = 1.0f / 8.0f;
        float density = 500.0f;
    };

    struct SkinEntry {
        std::string name;
        const Skin* skin = nullptr;   // owned by the host
        EntityModel model;            // built from the skin: slim and classic differ
    };

    std::vector<std::unique_ptr<Model>> models;
    std::vector<std::unique_ptr<SkinEntry>> skins;

    // Props the script spawned, in the order it spawned them, parallel to the
    // physics bodies. The host reads this to draw them.
    struct Spawned {
        int body = -1;
        size_t model = 0;
        Vec3 tint{1.0f, 1.0f, 1.0f};
    };

    std::vector<Spawned> spawned;

    // Characters. Unlike a prop these carry their own state rather than
    // living in a solver: an entity is not simulated, it is *posed*, and the
    // script owns where it stands and how it is bent. The host flattens the
    // list into an `EntitySet` each frame, exactly as it does for props.
    std::vector<Entity> entities;

    void addModel(const std::string& name, const VoxelModel& model, float voxelSize,
                  float density = 500.0f);
    int findModel(const std::string& name) const;

    void addSkin(const std::string& name, const Skin& skin);
    int findSkin(const std::string& name) const;
};

// Installs the world and physics verbs, plus the block-name constants.
// `sandbox` must outlive the interpreter.
void installSandboxLibrary(Interpreter& interpreter, Sandbox& sandbox);

} // namespace blocky::script
