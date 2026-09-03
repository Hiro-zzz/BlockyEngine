#pragma once
// Entity geometry: a model is a list of boxes, each carrying the six skin
// rectangles that clothe it, plus the skeleton those boxes hang from.
//
// Everything here is measured in *model pixels*, sixteen to a block, because
// that is the unit Minecraft models are authored in -- a player is 32 pixels
// tall. The conversion to world units happens once, when an entity instance
// is flattened for rendering.
//
// Axis convention, fixed here and relied on everywhere downstream:
//   +Y up, the entity looks towards -Z, and the entity's own right hand is
//   at +X. A viewer standing at -Z therefore sees the right hand on their
//   left, which is what looking at a person actually does.
#include "engine/assets/entity/skin.hpp"
#include "engine/core/math.hpp"
#include "engine/rig/rig.hpp"

#include <string>
#include <vector>

namespace blocky {

struct ModelBox {
    SkinPart  part  = PartHead;
    SkinLayer layer = LayerBase;

    // Which joint carries this box. The joint owns the pivot and the
    // parenting; the box only says where it sits and what it is wearing.
    int joint = -1;

    Vec3 origin{};   // min corner, model pixels, in entity space
    Vec3 size{};     // extent in model pixels

    // Outer layers are drawn as a slightly larger shell around the base box.
    float inflate = 0.0f;

    // Where each face lives inside the skin.
    SkinRect faces[SkinFaceCount];

    // Outer layers are mostly transparent and must be alpha-tested; base
    // layers are always treated as solid, exactly as the game does.
    bool cutout = false;
};

struct EntityModel {
    std::string name;
    std::vector<ModelBox> boxes;

    // The joints the boxes hang from. A model with an empty skeleton still
    // renders -- every box simply sits where it was authored.
    Skeleton skeleton;

    // Height of the model in blocks, for placing it on the ground.
    float heightBlocks = 2.0f;

    // Every box bound to `joint`, by index into `boxes`.
    std::vector<size_t> boxesOn(int joint) const;
};

// The humanoid player model, sized from the skin (classic or slim arms) and
// with every face rectangle resolved. This is the template that mob models
// will sit alongside -- nothing above this function knows it is a player.
//
// The skeleton it builds is the standard one, with the joint indices in
// rig.hpp: the torso hangs off a root, the head and both arms hang off the
// torso, and the legs hang off the root. So bending the torso carries the
// upper body with it while the legs stay planted, which is what bending at
// the waist does.
EntityModel buildPlayerModel(const Skin& skin);

// Face normal in box-local space.
Vec3 skinFaceNormal(SkinFace face);

// Map a point on a box face to texture coordinates within that face's
// rectangle. `local` is in pixels from the box min corner, `size` its extent.
Vec2 boxFaceUv(Vec3 local, Vec3 size, SkinFace face);

// Which model axis drives u and v across a face, and whether each runs
// backwards along it. This is the inverse of boxFaceUv, and it is what lets
// a box be cut in half with its skin rectangles cut to match.
struct FaceAxes {
    int  uAxis = 0;
    int  vAxis = 1;
    bool uFlip = false;
    bool vFlip = false;
};
FaceAxes faceAxes(SkinFace face);

} // namespace blocky
