#include "engine/entity/model.hpp"

namespace blocky {
namespace {

// How far each outer layer stands off the base box, in pixels. The hat is
// the thick one; sleeves and trousers are subtle.
constexpr float kHatInflate   = 0.5f;
constexpr float kOuterInflate = 0.25f;

void fillFaces(ModelBox& box, const Skin& skin) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        box.faces[face] = skin.faceRect(box.part, box.layer, SkinFace(face));
    }
}

ModelBox makeBox(const Skin& skin, SkinPart part, SkinLayer layer, int joint,
                 Vec3 origin, Vec3 size, float inflate) {
    ModelBox box;
    box.part = part;
    box.layer = layer;
    box.joint = joint;
    box.origin = origin;
    box.size = size;
    box.inflate = inflate;
    box.cutout = layer == LayerOuter;
    fillFaces(box, skin);
    return box;
}

} // namespace

Vec3 skinFaceNormal(SkinFace face) {
    switch (face) {
        case SkinFaceLeft:   return {-1.0f, 0.0f, 0.0f};
        case SkinFaceRight:  return { 1.0f, 0.0f, 0.0f};
        case SkinFaceBottom: return { 0.0f,-1.0f, 0.0f};
        case SkinFaceTop:    return { 0.0f, 1.0f, 0.0f};
        case SkinFaceFront:  return { 0.0f, 0.0f,-1.0f};
        default:             return { 0.0f, 0.0f, 1.0f};
    }
}

Vec2 boxFaceUv(Vec3 local, Vec3 size, SkinFace face) {
    // Guard against a degenerate axis before dividing.
    Vec3 t{size.x > 0.0f ? local.x / size.x : 0.0f,
           size.y > 0.0f ? local.y / size.y : 0.0f,
           size.z > 0.0f ? local.z / size.z : 0.0f};

    // v always runs downwards in texture space, so it is 1 - height for the
    // four side faces. u is chosen so that each face reads correctly when
    // looked at from outside the box.
    switch (face) {
        case SkinFaceFront:  return {1.0f - t.x, 1.0f - t.y};
        case SkinFaceBack:   return {t.x,        1.0f - t.y};
        case SkinFaceRight:  return {1.0f - t.z, 1.0f - t.y};
        case SkinFaceLeft:   return {t.z,        1.0f - t.y};
        case SkinFaceTop:    return {1.0f - t.x, t.z};
        default:             return {1.0f - t.x, 1.0f - t.z};  // bottom
    }
}

FaceAxes faceAxes(SkinFace face) {
    // Read straight off boxFaceUv above. Keep the two in step: cutting a box
    // in half uses this to cut its skin rectangles the same way, and a
    // disagreement shows up as a limb wearing the wrong half of its sleeve.
    switch (face) {
        case SkinFaceFront:  return {0, 1, true,  true};   // u = 1-x, v = 1-y
        case SkinFaceBack:   return {0, 1, false, true};   // u =   x, v = 1-y
        case SkinFaceRight:  return {2, 1, true,  true};   // u = 1-z, v = 1-y
        case SkinFaceLeft:   return {2, 1, false, true};   // u =   z, v = 1-y
        case SkinFaceTop:    return {0, 2, true,  false};  // u = 1-x, v =   z
        default:             return {0, 2, true,  true};   // bottom: u = 1-x, v = 1-z
    }
}

std::vector<size_t> EntityModel::boxesOn(int joint) const {
    std::vector<size_t> found;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].joint == joint) found.push_back(i);
    }
    return found;
}

EntityModel buildPlayerModel(const Skin& skin) {
    EntityModel model;
    model.name = skin.model() == SkinModel::Slim ? "player_slim" : "player_classic";
    model.heightBlocks = 2.0f;

    const float armWidth = float(skin.partSize(PartRightArm).x);  // 3 slim, 4 classic

    // ---- the skeleton. Order fixed: the indices in rig.hpp depend on it.
    //
    // The torso turns about the waist rather than the neck, so bending it
    // folds the whole upper body forward and leaves the legs standing. Before
    // there was a skeleton every part turned about its own pivot alone, and a
    // twisted torso left its own arms hanging in the air beside it.
    const int root     = model.skeleton.add("root", -1, {0.0f, 0.0f, 0.0f});
    const int body     = model.skeleton.add("body", root, {0.0f, 12.0f, 0.0f});
    const int head     = model.skeleton.add("head", body, {0.0f, 24.0f, 0.0f});
    const int rightArm = model.skeleton.add("rightArm", body, {5.0f, 22.0f, 0.0f});
    const int leftArm  = model.skeleton.add("leftArm", body, {-5.0f, 22.0f, 0.0f});
    const int rightLeg = model.skeleton.add("rightLeg", root, {2.0f, 12.0f, 0.0f});
    const int leftLeg  = model.skeleton.add("leftLeg", root, {-2.0f, 12.0f, 0.0f});

    // Torso spans x [-4,4], y [12,24], z [-2,2]. The head sits on top, the
    // arms hang either side of the torso and the legs below it.
    model.boxes.reserve(12);

    // ---- head, y 24..32
    model.boxes.push_back(makeBox(skin, PartHead, LayerBase, head,
                                  {-4.0f, 24.0f, -4.0f}, {8.0f, 8.0f, 8.0f}, 0.0f));
    model.boxes.push_back(makeBox(skin, PartHead, LayerOuter, head,
                                  {-4.0f, 24.0f, -4.0f}, {8.0f, 8.0f, 8.0f}, kHatInflate));

    // ---- torso, y 12..24
    model.boxes.push_back(makeBox(skin, PartBody, LayerBase, body,
                                  {-4.0f, 12.0f, -2.0f}, {8.0f, 12.0f, 4.0f}, 0.0f));
    model.boxes.push_back(makeBox(skin, PartBody, LayerOuter, body,
                                  {-4.0f, 12.0f, -2.0f}, {8.0f, 12.0f, 4.0f}, kOuterInflate));

    // ---- arms. The entity's right hand is at +X.
    Vec3 rightArmOrigin{4.0f, 12.0f, -2.0f};
    Vec3 leftArmOrigin{-4.0f - armWidth, 12.0f, -2.0f};
    Vec3 armSize{armWidth, 12.0f, 4.0f};

    model.boxes.push_back(makeBox(skin, PartRightArm, LayerBase, rightArm, rightArmOrigin, armSize, 0.0f));
    model.boxes.push_back(makeBox(skin, PartRightArm, LayerOuter, rightArm, rightArmOrigin, armSize, kOuterInflate));
    model.boxes.push_back(makeBox(skin, PartLeftArm, LayerBase, leftArm, leftArmOrigin, armSize, 0.0f));
    model.boxes.push_back(makeBox(skin, PartLeftArm, LayerOuter, leftArm, leftArmOrigin, armSize, kOuterInflate));

    // ---- legs, y 0..12
    Vec3 legSize{4.0f, 12.0f, 4.0f};

    model.boxes.push_back(makeBox(skin, PartRightLeg, LayerBase, rightLeg, {0.0f, 0.0f, -2.0f}, legSize, 0.0f));
    model.boxes.push_back(makeBox(skin, PartRightLeg, LayerOuter, rightLeg, {0.0f, 0.0f, -2.0f}, legSize, kOuterInflate));
    model.boxes.push_back(makeBox(skin, PartLeftLeg, LayerBase, leftLeg, {-4.0f, 0.0f, -2.0f}, legSize, 0.0f));
    model.boxes.push_back(makeBox(skin, PartLeftLeg, LayerOuter, leftLeg, {-4.0f, 0.0f, -2.0f}, legSize, kOuterInflate));

    // A legacy skin has no outer layer except the hat, so drop the boxes that
    // could only ever sample blank pixels.
    if (skin.wasLegacy()) {
        std::vector<ModelBox> kept;
        kept.reserve(model.boxes.size());
        for (const ModelBox& box : model.boxes) {
            if (box.layer == LayerOuter && box.part != PartHead) continue;
            kept.push_back(box);
        }
        model.boxes = std::move(kept);
    }
    return model;
}

} // namespace blocky
