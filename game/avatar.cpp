#include "game/avatar.hpp"
#include "game/blocks.hpp"

#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/world/world.hpp"

#include <algorithm>

namespace game {

using namespace blocky;

namespace {

using RGBA = ImageU8::RGBA;

void fillRect(ImageU8& image, const SkinRect& rect, RGBA colour) {
    if (!rect.valid()) return;
    for (int y = 0; y < rect.height; ++y)
        for (int x = 0; x < rect.width; ++x) image.set(rect.x + x, rect.y + y, colour);
}

// A little per-texel variation, so a flat fill does not read as plastic. The
// pattern is a hash of the absolute position rather than of the position
// inside the rect: two rects that meet at a seam then carry on the same
// noise instead of showing where one ended.
void scatter(ImageU8& image, const SkinRect& rect, int amount) {
    if (!rect.valid() || amount <= 0) return;
    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x) {
            int px = rect.x + x, py = rect.y + y;
            uint32_t h = uint32_t(px) * 73856093u ^ uint32_t(py) * 19349663u;
            h = h * 2654435761u;
            int delta = int((h >> 16) % uint32_t(2 * amount + 1)) - amount;

            RGBA c = image.get(px, py);
            if (c.a == 0) continue;
            auto bump = [delta](uint8_t v) {
                return uint8_t(std::clamp(int(v) + delta, 0, 255));
            };
            image.set(px, py, {bump(c.r), bump(c.g), bump(c.b), c.a});
        }
    }
}

// Bottom `rows` of a rect, which on a limb's side face is the hand or the
// shoe: a limb hangs downwards, so its far end is the bottom of its box.
SkinRect bottomRows(const SkinRect& rect, int rows) {
    if (!rect.valid() || rows <= 0) return {};
    rows = std::min(rows, rect.height);
    return {rect.x, rect.y + rect.height - rows, rect.width, rows};
}

// Fill every face of one part, then hand back the front rect so the caller
// can draw on it.
void paintPart(ImageU8& image, const Skin& layout, SkinPart part, RGBA colour, int noise) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinRect rect = layout.faceRect(part, LayerBase, SkinFace(face));
        fillRect(image, rect, colour);
        scatter(image, rect, noise);
    }
}

void paintLimbEnd(ImageU8& image, const Skin& layout, SkinPart part, RGBA colour, int rows) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinFace f = SkinFace(face);
        SkinRect rect = layout.faceRect(part, LayerBase, f);
        if (!rect.valid()) continue;

        // The bottom face of a limb is the whole end of it; the four sides
        // get their lowest rows; the top face is the shoulder or hip and is
        // never part of a hand or a shoe.
        if (f == SkinFaceTop) continue;
        fillRect(image, f == SkinFaceBottom ? rect : bottomRows(rect, rows), colour);
    }
}

// A face on the front of the head: two eyes and a mouth, in head-front
// coordinates. The rect is 8x8 and `v` runs downwards, as it does in any
// image, so row 3 is eye level.
void paintFace(ImageU8& image, const SkinRect& front) {
    if (front.width < 8 || front.height < 8) return;

    const RGBA white{236, 236, 236, 255};
    const RGBA iris{60, 82, 140, 255};
    const RGBA mouth{120, 76, 66, 255};

    auto put = [&](int x, int y, RGBA c) { image.set(front.x + x, front.y + y, c); };

    for (int side = 0; side < 2; ++side) {
        int x = side == 0 ? 1 : 5;
        put(x, 3, white);
        put(x + 1, 3, iris);
    }
    for (int x = 2; x < 6; ++x) put(x, 6, mouth);
}

// The skin the game draws for itself when the file is not there.
//
// `layout` must already be a valid 64x64 skin, because every rectangle below
// is asked of it rather than written down here: a table of hardcoded pixel
// coordinates would be a second copy of the layout in conventions.md, and the
// two would agree until one of them changed.
ImageU8 drawSkin(const Skin& layout) {
    ImageU8 image(64, 64);   // starts fully transparent

    const RGBA skinTone{224, 176, 138, 255};
    const RGBA hair{74, 52, 38, 255};
    const RGBA shirt{78, 116, 158, 255};
    const RGBA sleeve{62, 96, 134, 255};
    const RGBA trousers{62, 62, 78, 255};
    const RGBA shoe{44, 40, 40, 255};

    paintPart(image, layout, PartHead, skinTone, 6);
    paintPart(image, layout, PartBody, shirt, 7);
    paintPart(image, layout, PartRightArm, sleeve, 7);
    paintPart(image, layout, PartLeftArm, sleeve, 7);
    paintPart(image, layout, PartRightLeg, trousers, 6);
    paintPart(image, layout, PartLeftLeg, trousers, 6);

    // Hair over the top and back of the head, and a fringe on the front.
    fillRect(image, layout.faceRect(PartHead, LayerBase, SkinFaceTop), hair);
    fillRect(image, layout.faceRect(PartHead, LayerBase, SkinFaceBack), hair);
    SkinRect front = layout.faceRect(PartHead, LayerBase, SkinFaceFront);
    if (front.valid()) fillRect(image, {front.x, front.y, front.width, 2}, hair);
    for (SkinFace side : {SkinFaceLeft, SkinFaceRight}) {
        SkinRect rect = layout.faceRect(PartHead, LayerBase, side);
        if (rect.valid()) fillRect(image, {rect.x, rect.y, rect.width, 2}, hair);
    }
    paintFace(image, front);

    // Hands and shoes.
    paintLimbEnd(image, layout, PartRightArm, skinTone, 3);
    paintLimbEnd(image, layout, PartLeftArm, skinTone, 3);
    paintLimbEnd(image, layout, PartRightLeg, shoe, 3);
    paintLimbEnd(image, layout, PartLeftLeg, shoe, 3);

    // Every outer layer is left at alpha zero. `hasLayer` answers by format
    // rather than by content, so the model gains its outer boxes either way --
    // they are simply cut away entirely, which is what an alpha-tested shell
    // over nothing is meant to do.
    return image;
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

// Round-tripping a generated image through our own PNG codec looks
// roundabout, and is deliberate: `loadFromPng` is the only way into a Skin, so
// going through it means a drawn skin takes exactly the path a real one takes,
// including the classic/slim detection. A second entry point would be a second
// thing to keep in step.
bool adoptImage(Skin& skin, const ImageU8& image) {
    std::vector<uint8_t> png = pngEncode(image);
    return skin.loadFromPng(png.data(), png.size());
}

} // namespace

bool buildAvatar(Avatar& avatar, const std::string& skinPath, std::string* note) {
    std::string error;
    bool loaded = !skinPath.empty() && loadSkinFile(avatar.skin, skinPath, &error);

    if (!loaded) {
        // A blank opaque sheet is a valid skin of the right size, which is all
        // that is needed to ask it where every rectangle lives. It is detected
        // as classic precisely because it is opaque -- a slim skin is the one
        // with two transparent columns in the arm block.
        ImageU8 blank(64, 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) blank.set(x, y, {128, 128, 128, 255});

        if (!adoptImage(avatar.skin, blank)) return false;
        if (!adoptImage(avatar.skin, drawSkin(avatar.skin))) return false;
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
