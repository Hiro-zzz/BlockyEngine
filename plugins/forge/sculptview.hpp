#pragma once
// The 3D half of the window: an orbit camera, a cursor that points at
// voxels, and the panel that says what it is pointing at.
//
// -------------------------------------------------------------- one transform
//
// The matrix that puts the model in the world is built **here, once**, and
// handed to `PropSet::addTransformed`. Picking inverts that same matrix. The
// alternative -- letting `Prop`'s anchor place it for the renderer and
// working out where it must have gone for the cursor -- is two derivations of
// one number, and `docs/architecture.md` records what that costs: they agree
// until the day they do not, and the symptom is a cursor that is slightly off
// rather than an error.
//
// It also has to exist before the model does. An empty grid is placed by
// nothing, and an editor you cannot put the first voxel into is not an
// editor, so the transform cannot come from the geometry.
#include "engine/core/math.hpp"
#include "engine/edit/sculpt.hpp"
#include "engine/render/gl/overlay.hpp"
#include "engine/scene/camera.hpp"

#include "plugins/forge/studio.hpp"

namespace blocky {
class Window;
}

namespace forge {

using namespace blocky;

// World units per voxel. A sixteen-voxel model is one block tall, which is
// what `item::buildModel` and the rest of the engine mean by item scale.
inline constexpr float kVoxelSize = 1.0f / 16.0f;

// Voxel coordinates to world. Centred in x and z, standing on y = 0.
Mat4 modelToWorld(IVec3 dims, float voxelSize = kVoxelSize);

// A one-voxel slab the size of the model's footprint, and where to put it.
//
// A prop rather than blocks of the world, because the footprint is the
// model's and the world's cells are a metre across: a 16-voxel model is one
// block wide and sits astride four of them, so a plate made of blocks is
// twice the size of the thing it is marking. The prop is exact at every grid
// size, and costs the same machinery the model already uses.
void buildPlate(VoxelModel& plate, IVec3 dims);
Mat4 plateToWorld(IVec3 dims, float voxelSize = kVoxelSize);

struct Orbit {
    float yawDegrees = 35.0f;
    float pitchDegrees = 24.0f;
    float distance = 2.0f;
    float height = 0.5f;    // what the camera looks at, above the floor

    // Frames what is actually there: the occupied box when the model has
    // anything in it, and the plate when it does not. Framing on the middle
    // of an empty 16-cube points the camera at a spot half a block above an
    // empty plate, which is a correct answer to the wrong question.
    //
    // Called when the document changes size, not every frame: it is a
    // starting point, and a camera that re-frames itself while somebody is
    // orbiting it fights them.
    void frame(const blocky::edit::Sculpt& sculpt, float voxelSize = kVoxelSize);

    void apply(Camera& camera, float aspect) const;
};

// What the cursor is over this frame.
struct Hover {
    bool  valid = false;
    IVec3 place{};    // where a new voxel goes
    IVec3 erase{};    // which one comes out; meaningless on the floor
    bool  onFloor = false;   // the empty grid's base plane rather than a voxel
};

struct SculptView {
    Hover hover;

    // Held across frames so a drag paints a streak rather than one voxel per
    // click. The same reason `Canvas` has strokes: the hand moves faster than
    // the eye checks.
    bool painting = false;
    bool erasing = false;

    // The orbit is the host's, not this view's: the sprite mode flies the same
    // camera around the same model, and two copies of it would drift apart the
    // first time somebody switched modes mid-turn.
    void update(Studio& studio, const Window& window, Orbit& orbit, const Camera& camera,
                int width, int height, bool pointerOverUi);

    void draw(const Studio& studio, Overlay& overlay) const;
};

} // namespace forge
