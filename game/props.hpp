#pragma once
// What the sandbox is made of: the things you can spawn.
//
// A world you can only add blocks to is a builder. What makes it a toy is
// loose furniture -- things with a mass and a shape that fall over, stack,
// roll, and can be picked up and pinned in mid-air. The physgun existed
// before any of it, which is why pressing its trigger in a fresh world used
// to grab nothing at all: the tool was finished and there was nothing to use
// it on.
//
// ------------------------------------------------------------ built in code
//
// Like the block textures, the fallback skin and the physgun itself. The game
// promises to need no asset files, and these are small enough to write out
// shape by shape -- a crate is a loop, a barrel is a loop with a radius test.
// The moment one of them is worth more than that, `voxelize::fromLayers`
// takes a drawing in text and `voxelize::fromWorld` captures anything the
// `shape::` primitives can build.
//
// -------------------------------------------------------- what a kind holds
//
// The model, the collider built from it once, and the two numbers that decide
// how it behaves: `voxelSize` (how big a voxel is in blocks, so the same
// model can be a crate or a shipping container) and `density`. Mass comes out
// of the collider and the density, so a barrel is heavier than a plank
// because it is bigger and denser, not because a number said so.
//
// The collider lives here and nowhere else, and that is load-bearing: a
// `RigidBody` holds a bare pointer into it, so the catalogue must outlive
// every body spawned from it and must never be resized after the first spawn.
#include "engine/physics/collider.hpp"
#include "engine/prop/voxel_model.hpp"

#include <string>
#include <vector>

namespace game {

struct PropKind {
    // Shown in the spawn menu, in capitals because that is how the interface
    // draws everything else.
    std::string name;

    blocky::VoxelModel model;
    blocky::Collider   collider;

    // How big one voxel is, in blocks. 1/8 makes an eight-voxel crate one
    // block across.
    float voxelSize = 0.125f;

    // Mass per cubic block. Wood is light, stone is not, and a ball that
    // weighs what a girder does is a ball nobody believes.
    float density = 500.0f;

    float friction = 0.65f;
    float restitution = 0.05f;
};

// Konstruct's catalogue, built once. The order is the order the spawn menu
// shows, so it reads as a shelf: boxes, then long things, then round things.
std::vector<PropKind> buildPropCatalogue();

} // namespace game
