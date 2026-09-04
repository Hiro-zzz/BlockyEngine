#include "game/avatar.hpp"
#include "game/blocks.hpp"

#include "engine/assets/entity/skin_gen.hpp"
#include "engine/core/file.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/world/world.hpp"

#include <algorithm>

namespace game {

using namespace blocky;

namespace {

using RGBA = ImageU8::RGBA;

// The player the game draws for itself when there is no skin file.
//
// A recipe rather than a drawing routine: the verbs are the engine's, in
// `engine/assets/entity/skin_gen.hpp`, and everything below is who this
// particular person is. Same split as the block textures next door -- `texgen`
// knows how to lay a grain and `game/blocks.cpp` knows what oak looks like --
// and it is what lets `scenes/common/skins.hpp` stage a whole cast without a
// second copy of the painting code living beside this one.
void paintDefaultPlayer(const Skin& layout, ImageU8& sheet) {
    using namespace skingen;

    const RGBA skinTone{224, 176, 138, 255};
    const RGBA hair{74, 52, 38, 255};
    const RGBA shirt{78, 116, 158, 255};
    const RGBA sleeve{62, 96, 134, 255};
    const RGBA trousers{62, 62, 78, 255};
    const RGBA shoe{44, 40, 40, 255};

    part(sheet, layout, PartHead, skinTone, 6);
    part(sheet, layout, PartBody, shirt, 7);
    part(sheet, layout, PartRightArm, sleeve, 7);
    part(sheet, layout, PartLeftArm, sleeve, 7);
    part(sheet, layout, PartRightLeg, trousers, 6);
    part(sheet, layout, PartLeftLeg, trousers, 6);

    // Hair over the top and sides, the whole of the back, and a fringe.
    cap(sheet, layout, PartHead, hair, 2);
    fill(sheet, layout.faceRect(PartHead, LayerBase, SkinFaceBack), hair);
    faceFeatures(sheet, layout.faceRect(PartHead, LayerBase, SkinFaceFront),
                 {236, 236, 236, 255}, {60, 82, 140, 255}, {120, 76, 66, 255});

    // Hands and shoes.
    limbEnd(sheet, layout, PartRightArm, skinTone, 3);
    limbEnd(sheet, layout, PartLeftArm, skinTone, 3);
    limbEnd(sheet, layout, PartRightLeg, shoe, 3);
    limbEnd(sheet, layout, PartLeftLeg, shoe, 3);

    // Every outer layer is left at alpha zero. `hasLayer` answers by format
    // rather than by content, so the model gains its outer boxes either way --
    // they are simply cut away entirely, which is what an alpha-tested shell
    // over nothing is meant to do.
}

// The right arm, as voxels wearing the skin.
//
// Every voxel takes the colour of the face nearest it, read through the same
// `boxFaceUv` the renderer clothes the box with. Going through that function
// rather than working the rectangle out here is the point: the mapping has a
// mirror in it -- the character's right side is stored in the left half of its
// rectangle -- and a second copy of that rule would be a second chance to get
// it backwards. On a symmetrical sleeve nobody would ever see the mistake.
VoxelModel buildPartModel(const Skin& skin, SkinPart part) {
    const IVec3 size = skin.partSize(part);
    VoxelModel arm;
    if (size.x <= 0 || size.y <= 0 || size.z <= 0) return arm;

    arm.resize(size);
    const ImageU8& sheet = skin.image();

    // Which face a voxel shows, in the order a limb is actually looked at:
    // the outside of the arm before its ends, and its ends before nothing.
    const SkinFace order[] = {SkinFaceFront, SkinFaceBack,   SkinFaceRight,
                              SkinFaceLeft,  SkinFaceBottom, SkinFaceTop};

    for (int y = 0; y < size.y; ++y) {
        for (int z = 0; z < size.z; ++z) {
            for (int x = 0; x < size.x; ++x) {
                for (SkinFace face : order) {
                    Vec3 normal = skinFaceNormal(face);
                    bool onFace = (normal.x > 0.5f && x == size.x - 1) ||
                                  (normal.x < -0.5f && x == 0) ||
                                  (normal.y > 0.5f && y == size.y - 1) ||
                                  (normal.y < -0.5f && y == 0) ||
                                  (normal.z > 0.5f && z == size.z - 1) ||
                                  (normal.z < -0.5f && z == 0);
                    if (!onFace) continue;

                    SkinRect rect = skin.faceRect(part, LayerBase, face);
                    if (!rect.valid()) continue;

                    Vec2 uv = boxFaceUv({float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f},
                                        {float(size.x), float(size.y), float(size.z)}, face);

                    int tx = std::clamp(rect.x + int(uv.x * float(rect.width)), rect.x,
                                        rect.x + rect.width - 1);
                    int ty = std::clamp(rect.y + int(uv.y * float(rect.height)), rect.y,
                                        rect.y + rect.height - 1);

                    ImageU8::RGBA texel = sheet.get(tx, ty);
                    if (texel.a < 128) break;   // a cut-away texel is not arm

                    VoxelMaterial material;
                    material.albedo = srgbToLinear(Vec3{float(texel.r) / 255.0f,
                                                        float(texel.g) / 255.0f,
                                                        float(texel.b) / 255.0f});
                    material.roughness = 1.0f;
                    arm.set({x, y, z}, arm.addMaterial(material));
                    break;
                }
            }
        }
    }
    return arm;
}

bool loadSkinFile(Skin& skin, const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return skin.loadFromPng(bytes.data(), bytes.size(), error);
}

} // namespace

bool buildAvatar(Avatar& avatar, const std::string& skinPath, std::string* note) {
    std::string error;
    bool loaded = !skinPath.empty() && loadSkinFile(avatar.skin, skinPath, &error);

    if (!loaded) {
        ImageU8 sheet;
        if (!skingen::begin(avatar.skin, sheet)) return false;
        paintDefaultPlayer(avatar.skin, sheet);
        if (!skingen::finish(avatar.skin, sheet)) return false;
    }

    if (note) {
        *note = loaded ? ("skin " + skinPath)
                       : ("skin drawn in code (" + (error.empty() ? std::string("no file at ") + skinPath
                                                                  : error) + ")");
    }

    avatar.world = buildPlayerModel(avatar.skin);

    // Eyes before the limbs are voxelised, because building the rig **repaints
    // the skin**: it fills the found eyes in with what surrounds them and
    // draws a new pair somewhere the model was not using. Voxelising first
    // would bake the old face into the head model and leave the character
    // staring out of two places at once.
    avatar.eyes = face::buildEyeRig(avatar.world, avatar.skin);

    for (int part = 0; part < PartCount; ++part)
        avatar.parts[part] = buildPartModel(avatar.skin, SkinPart(part));
    avatar.arm = avatar.parts[PartRightArm];

    if (note) {
        *note += avatar.eyes.built
                     ? ", eyes rigged"
                     : (std::string(", eyes not rigged (") + avatar.eyes.rejection + ")");
    }
    return avatar.ready();
}

const VoxelModel* HeldBlocks::modelFor(BlockId id, const BlockTextureLibrary* textures) {
    if (id == block::Air) return nullptr;

    auto found = models_.find(uint32_t(id));
    if (found != models_.end()) return found->second.empty() ? nullptr : &found->second;

    // Captured against the game's own palette, because the id is one of its
    // blocks and the capture reads the block's material out of the registry.
    World scratch(palette());
    scratch.set({0, 0, 0}, id);

    voxelize::CaptureOptions options;
    options.textures = textures;
    options.trim = false;

    // A held glass block should be a held glass block. Capture refuses
    // transmissive blocks by default because a prop cannot be a medium -- but
    // the alternative here is an empty hand, and opaque paint is the better of
    // the two wrong answers.
    options.skipTransmissive = false;

    VoxelModel model = voxelize::fromWorld(scratch, {0, 0, 0}, {0, 0, 0}, options);
    auto inserted = models_.emplace(uint32_t(id), std::move(model));
    return inserted.first->second.empty() ? nullptr : &inserted.first->second;
}

} // namespace game
