#pragma once
// A voxel model being edited: the 3D half of the editor.
//
// The document is a `VoxelModel`, which already is what a prop is made of.
// Nothing is converted on the way out, so what the editor shows and what the
// tracer shades are the same grid with the same palette.
//
// ------------------------------------------------------- placing and erasing
//
// The verb is the game's. `VoxelModel::trace` answers the same question
// `World::raycast` answers -- which cell, which face -- so pointing at a
// model gives the struck voxel and the empty one in front of it, and
// place/erase are those two cells. That is not a resemblance: it is
// `game/player.cpp` one layer down, against a grid instead of a lattice.
//
// ------------------------------------------------------ the grid does not grow
//
// Placing against the outer face of a boundary voxel is clamped, not grown.
// Growing would move every voxel's index, and an undo stack is a list of
// indices -- the step before the growth would then put cells back in the
// wrong places. So the size is changed deliberately, by `resizeKeeping`, and
// that clears the history for the same reason.
#include "engine/core/math.hpp"
#include "engine/edit/history.hpp"
#include "engine/prop/voxel_model.hpp"

#include <string>

namespace blocky {
namespace edit {

// What the cursor is over.
struct Pick {
    bool  hit = false;
    IVec3 voxel{};      // the solid cell the ray struck
    IVec3 adjacent{};   // the cell on the near side of that face -- where a place goes
    Vec3  normal{};     // model-local, axis aligned
    float t = 0.0f;
};

class Sculpt {
public:
    // An empty grid. Sixteen a side is the item scale the rest of the engine
    // works in -- a block is sixteen voxels across, and `item::buildModel`
    // extrudes exactly that.
    void create(IVec3 dims);
    void create(int side = 16) { create({side, side, side}); }

    // Takes over an existing model: a capture from the world, an extruded
    // item, or a catalogue entry being edited.
    void adopt(VoxelModel model);

    const VoxelModel& model() const { return model_; }
    IVec3 dims() const { return model_.dims(); }
    bool  empty() const { return model_.empty(); }

    bool inside(IVec3 v) const { return model_.inside(v); }
    uint16_t at(IVec3 v) const { return model_.at(v); }

    // ------------------------------------------------------------ materials
    //
    // Folded by `VoxelModel::addMaterial`, so asking twice for the same
    // colour gives the same slot rather than a second one.
    uint16_t addMaterial(const VoxelMaterial& material) { return model_.addMaterial(material); }
    void     setMaterial(uint16_t index) { material_ = index; }
    uint16_t material() const { return material_; }
    const VoxelMaterial& materialAt(uint16_t index) const { return model_.material(index); }

    // --------------------------------------------------------------- tools
    void place(IVec3 v, uint16_t material);
    void place(IVec3 v) { place(v, material_); }
    void erase(IVec3 v);

    // Inclusive box, like `World::fillBox`. `kEmpty` erases the region,
    // which is why there is no separate eraser box.
    void box(IVec3 a, IVec3 b, uint16_t material);

    void clearAll();

    // Mirrors every edit about the middle of the X extent. On an even width
    // the two halves are the reflection of each other; on an odd one the
    // centre column maps to itself and is written once.
    bool mirrorX = false;

    // --------------------------------------------------------------- picking
    //
    // The ray is in voxel units, model-local. Scaling it is the caller's
    // business, because the caller is the one holding the transform -- and
    // scaling direction by the same factor as the space is what keeps `t`
    // meaning the same thing on both sides of it.
    Pick pick(Vec3 origin, Vec3 direction, float tMax = 1024.0f) const;

    // ---------------------------------------------------------------- size
    //
    // Both clear the history: they move the cells its indices name.
    void resizeKeeping(IVec3 dims, IVec3 offset);
    void trimToContents();

    // ------------------------------------------------------------- strokes
    void beginStroke(std::string name);
    void endStroke();

    History& history() { return history_; }
    const History& history() const { return history_; }

    bool undo();
    bool redo();

private:
    // This project's rule about not computing the same thing twice cuts the
    // other way here: the history's index is **ours**, not the model's. It
    // has to be a bijection this class can invert and nothing more, so it
    // deliberately does not reach for `VoxelModel`'s private layout -- which
    // is then free to change without dragging an undo stack with it.
    uint32_t index(IVec3 v) const;
    IVec3    fromIndex(uint32_t at) const;

    void put(IVec3 v, uint16_t material);
    void putRaw(IVec3 v, uint16_t material);
    bool openStroke(const char* name);

    VoxelModel model_;
    History    history_;
    uint16_t   material_ = VoxelModel::kEmpty;
};

} // namespace edit
} // namespace blocky
