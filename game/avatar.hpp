#pragma once
// What the player is made of, as opposed to where the player is.
//
// `Character` in the physics layer is a box that walks; it has no skin, no
// limbs and nothing to look at. This is the other half: the model those limbs
// hang off, the skin that clothes it, and the block held in the fist.
//
// The body is what a camera behind the player sees. First person gets the arm
// alone, as voxels, because the whole body cannot work there and the reason is
// arithmetic rather than taste: the eye sits at 1.62 blocks and the torso ends
// at 1.5, so its top face is twelve centimetres under the camera and fills the
// entire lower half of the view. Rendering your own chest needs proportions
// this model does not have.
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/model.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/world/block.hpp"

#include <string>
#include <unordered_map>

namespace game {

struct Avatar {
    blocky::Skin skin;
    blocky::EntityModel world;   // the character, for third person

    // The right arm as a voxel grid, for the first-person view model.
    //
    // Not the same arm as the one on `world`, and it cannot be. An entity
    // carries a position and a yaw; a view model has to follow the camera
    // through its pitch as well, and be drawn smaller than life because the
    // shoulder of a real arm sits a quarter of a block from the eye. A prop
    // takes a whole matrix and any scale, so this is a prop -- which is what
    // `prop/` exists for, and the reason it does not need a new subsystem.
    //
    // Voxel (x, y, z) is model pixel (x, y, z) of the arm box: y zero is the
    // fist and y eleven the shoulder, and -Z is the way it faces.
    blocky::VoxelModel arm;

    // Eyes on their own joints, so a glance is the pupil moving inside the
    // white rather than the whole face turning.
    //
    // Built rather than assumed: the engine scans the skin for a mirrored pair
    // of eye-like texels, paints the originals out and draws new ones. It can
    // fail, and when it does it says so instead of guessing -- a wrong guess
    // here does not look like a bug, it looks like a character whose face has
    // been vandalised. `rig.built` false simply means this character has
    // painted eyes like everyone else's.
    blocky::face::EyeRig eyes;

    // Every part as voxels, for a ragdoll. Indexed by `SkinPart`.
    blocky::VoxelModel parts[blocky::PartCount];

    bool ready() const { return skin.valid() && !world.boxes.empty(); }
};

// Loads `skinPath` if it is there and generates one if it is not.
//
// The game promises to need no Minecraft install, and a skin is the one asset
// it cannot generate from a block palette -- so it carries its own, and falls
// back to a drawn-in-code one when the file is missing (which mostly means the
// executable was run from somewhere other than the project root).
//
// `note` receives a line describing which of the two happened, because "why
// does the player look like that" is otherwise unanswerable from the outside.
bool buildAvatar(Avatar& avatar, const std::string& skinPath, std::string* note = nullptr);

// A held block, as a voxel cube.
//
// The block in a fist is a prop, not a block: it is sixteen times smaller than
// a cell and turns with the wrist, which is the whole reason `prop/` exists.
// Built by capturing a scratch world through `voxelize::fromWorld`, so the
// colour comes from the same place the world's does rather than from a second
// formula that would drift from it.
class HeldBlocks {
public:
    // Null for air or for a block that captures to nothing. The pointer stays
    // valid for the life of this object, which is what a `VoxelAttachment`
    // needs.
    const blocky::VoxelModel* modelFor(blocky::BlockId id,
                                       const blocky::BlockTextureLibrary* textures);

private:
    std::unordered_map<uint32_t, blocky::VoxelModel> models_;
};

} // namespace game
