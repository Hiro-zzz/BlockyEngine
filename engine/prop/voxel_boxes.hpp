#pragma once
// Greedy merge of a voxel model's cells into boxes.
//
// Convex decomposition is a genuinely hard problem in general. Voxels hand it
// over easy: a set of solid cells already *is* a union of axis-aligned unit
// cubes, so merging them into larger boxes is **exact** rather than
// approximate. A sixteen-voxel item usually falls to a few dozen boxes, and a
// slab or a plank to one.
//
// ------------------------------------------------------------- one walk, two
//
// There are two callers and they want different equivalences, which is why
// this is a parameter rather than two functions in two places:
//
//   - **physics** wants the fewest boxes covering the solid set, and does not
//     care what colour anything is. Every solid cell is one class.
//   - **an exported model** carries a texture per face, so a box that spans
//     two materials would have to pick one and lose the other. The class is
//     the material.
//
// The walk itself is the same, and it has to stay the same. Two greedy
// mergers would agree until the day they did not, and the difference would
// show as a model whose collision is subtly not its shape.
//
// ------------------------------------------------------------- what it gives
//
// A partition: the boxes **cover every cell of their class** and **never
// overlap**. `test_physics` asserts both, which is what lets mass properties
// sum over boxes instead of over cells, and what lets an exporter emit one
// quad per box face without wondering whether it double-counted.
#include "engine/core/math.hpp"
#include "engine/prop/voxel_model.hpp"

#include <cstdint>
#include <vector>

namespace blocky {

// Voxel-space box, half-open: [min, max).
struct VoxelBox {
    IVec3 min{};
    IVec3 max{};

    // The material of the cells inside. Under `Solid` this is the seed cell's
    // material and the rest of the box may differ, so only `ByMaterial`
    // callers may trust it.
    uint16_t material = 0;

    int cells() const { return (max.x - min.x) * (max.y - min.y) * (max.z - min.z); }
};

enum class BoxMerge {
    Solid,        // any non-empty cell joins any box: the fewest boxes
    ByMaterial,   // a cell joins only a box of its own material
};

std::vector<VoxelBox> mergeVoxelBoxes(const VoxelModel& model, BoxMerge rule);

} // namespace blocky
