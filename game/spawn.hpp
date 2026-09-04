#pragma once
// Everything the sandbox has spawned, and the two ways things get into it.
//
// The physics world owns bodies; this owns what they *are* -- which model
// draws each one, and the storage that model and its collider live in. The
// split is the same one the ragdolls make: `main.cpp` decides when something
// is spawned, `PhysicsWorld` integrates it, and this remembers the pairing so
// the frame can be drawn.
//
// ---------------------------------------------------- why storage is stable
//
// A `RigidBody` holds a bare pointer to its `Collider`. So every collider a
// body might point at has to sit somewhere that never moves: the catalogue is
// built once and never resized, and the block models below live in a `deque`,
// which does not invalidate references when it grows. A `vector` here would
// work until the day it reallocated, and the symptom would arrive long after
// the cause -- the same trap the script layer's model list fell into.
//
// ------------------------------------------------------- a block as a prop
//
// The second way in, and the interesting one. The world is a lattice: a block
// is a cell, it cannot turn, and it cannot be anywhere but on the grid. The
// moment the physgun takes hold of one, none of that is true any more -- so
// the block is *removed from the world* and a rigid body of the same size and
// colour is put where it was. It is the same trade `prop/` was invented for:
// off the lattice, free rotation, any angle.
//
// Which means picking a block up is destructive to the world by design, and
// putting it back is not automatic -- a lifted block is furniture now. That is
// the sandbox answer rather than the survival one, and it is the one asked
// for.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/world/world.hpp"

#include "game/props.hpp"

#include <deque>
#include <unordered_map>
#include <vector>

namespace game {

// One thing standing in the world.
struct Spawned {
    int body = -1;
    const blocky::VoxelModel* model = nullptr;
    blocky::Vec3 tint{1.0f, 1.0f, 1.0f};
};

class PropYard {
public:
    // The catalogue is borrowed, not copied, and must outlive the yard: the
    // bodies point into its colliders.
    void open(const std::vector<PropKind>* catalogue) { catalogue_ = catalogue; }

    const std::vector<PropKind>* catalogue() const { return catalogue_; }

    // Spawn one of the catalogue's kinds. Returns the body index, or -1.
    int spawn(blocky::PhysicsWorld& physics, size_t kind, blocky::Vec3 at,
              blocky::Vec3 velocity = {}, blocky::Vec3 spin = {});

    // Take a block out of the world and stand it up as a body in the same
    // place. Returns the body index, or -1 when the cell held nothing that
    // could be lifted.
    //
    // `textures` may be null, in which case the prop takes the block's flat
    // palette colour -- the same fallback everything else in the game has.
    int liftBlock(blocky::PhysicsWorld& physics, blocky::World& world, blocky::IVec3 cell,
                  const blocky::BlockTextureLibrary* textures);

    // Drop everything. Called when a world is rebuilt: the bodies belong to a
    // physics world that is about to be cleared, so nothing is removed here.
    void clear();

    // Forget entries whose bodies are gone. Slots are reused, so an entry
    // that outlived its body would draw a model over somebody else.
    void forgetDead(const blocky::PhysicsWorld& physics);

    // Keep the count under `budget`, oldest first, skipping anything the
    // caller is still holding. Returns how many were removed.
    //
    // The same argument as the ragdoll budget: the broad phase is quadratic
    // in bodies, so a sandbox that only ever gains things is a game that gets
    // slower the longer it is played.
    int trim(blocky::PhysicsWorld& physics, size_t budget, int keepBody);

    void draw(blocky::PropSet& props, const blocky::PhysicsWorld& physics) const;

    size_t count() const { return spawned_.size(); }

private:
    struct BlockProp {
        blocky::VoxelModel model;
        blocky::Collider   collider;
    };

    const BlockProp& blockPropFor(blocky::BlockId id, const blocky::BlockRegistry& registry,
                                  const blocky::BlockTextureLibrary* textures);

    const std::vector<PropKind>* catalogue_ = nullptr;
    std::vector<Spawned> spawned_;

    // Deque: references stay valid as it grows, which is what the bodies need.
    std::deque<BlockProp> blockProps_;
    std::unordered_map<uint32_t, size_t> blockPropByBlock_;
};

} // namespace game
