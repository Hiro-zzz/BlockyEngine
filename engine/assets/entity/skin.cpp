#include "engine/assets/entity/skin.hpp"

#include "engine/core/png.hpp"

namespace blocky {
namespace {

// Top-left corner of each part box inside the skin, per layer.
// A value of {-1,-1} means the layer does not exist for that part.
struct PartOrigin { int x, y; };

constexpr PartOrigin kOrigins[PartCount][LayerCount] = {
    /* Head     */ {{ 0,  0}, {32,  0}},
    /* Body     */ {{16, 16}, {16, 32}},
    /* RightArm */ {{40, 16}, {40, 32}},
    /* LeftArm  */ {{32, 48}, {48, 48}},
    /* RightLeg */ {{ 0, 16}, { 0, 32}},
    /* LeftLeg  */ {{16, 48}, { 0, 48}},
};

// The classic box unwrap. For a box w wide, h tall and d deep whose corner
// sits at (u,v), the six faces are laid out as:
//
//         .  [top ][bot ] .
//   [right][front][left ][back]
//
// which is the arrangement Minecraft has used since the beginning.
//
// The strip is a *roll*: reading it left to right walks once around the box
// in one direction, so two rectangles that touch in the file touch on the
// model as well, and an artist can paint a hairline straight across the seam.
// That is what fixes the order, and it is checkable without owning a copy of
// the game -- boxFaceUv() says the front rectangle's leftmost column is the
// entity's +X edge, so the rectangle to the left of it in the file has to be
// the +X face. `test_entity` walks all four seams and asserts exactly that.
//
// These names are the entity's own sides, not the viewer's. The first
// rectangle is the character's RIGHT cheek, which is the one a viewer facing
// them sees on their left -- the same reason the front face reads mirrored.
// Getting this pair backwards is invisible on Steve and on every other
// symmetric skin: swapping the two sides *and* reversing each front-to-back
// is precisely the mirroring that made a legacy 64x32 skin's left limbs, so
// a symmetric character survives it and an asymmetric one wears their fringe
// on the wrong side of their head.
SkinRect faceRectFor(int u, int v, int w, int h, int d, SkinFace face) {
    switch (face) {
        case SkinFaceTop:    return {u + d,             v,     w, d};
        case SkinFaceBottom: return {u + d + w,         v,     w, d};
        case SkinFaceRight:  return {u,                 v + d, d, h};
        case SkinFaceFront:  return {u + d,             v + d, w, h};
        case SkinFaceLeft:   return {u + d + w,         v + d, d, h};
        case SkinFaceBack:   return {u + d + w + d,     v + d, w, h};
        default:             return {};
    }
}

// One mirrored copy performed while expanding a legacy skin.
struct LegacyCopy {
    int srcX, srcY, width, height, dstX, dstY;
};

// Right limb faces, remapped into the left limb slots. Left and right swap,
// and every rectangle is flipped horizontally.
constexpr LegacyCopy kLegacyCopies[] = {
    // Right leg (0,16) -> left leg (16,48)
    { 4, 16, 4,  4, 20, 48},  // top
    { 8, 16, 4,  4, 24, 48},  // bottom
    { 0, 20, 4, 12, 24, 52},  // right face becomes the left face
    { 4, 20, 4, 12, 20, 52},  // front
    { 8, 20, 4, 12, 16, 52},  // left face becomes the right face
    {12, 20, 4, 12, 28, 52},  // back
    // Right arm (40,16) -> left arm (32,48)
    {44, 16, 4,  4, 36, 48},
    {48, 16, 4,  4, 40, 48},
    {40, 20, 4, 12, 40, 52},
    {44, 20, 4, 12, 36, 52},
    {48, 20, 4, 12, 32, 52},
    {52, 20, 4, 12, 44, 52},
};

} // namespace

const char* Skin::partName(SkinPart part) {
    switch (part) {
        case PartHead:     return "head";
        case PartBody:     return "body";
        case PartRightArm: return "right_arm";
        case PartLeftArm:  return "left_arm";
        case PartRightLeg: return "right_leg";
        case PartLeftLeg:  return "left_leg";
        default:           return "?";
    }
}

const char* Skin::faceName(SkinFace face) {
    switch (face) {
        case SkinFaceLeft:   return "left";
        case SkinFaceRight:  return "right";
        case SkinFaceBottom: return "bottom";
        case SkinFaceTop:    return "top";
        case SkinFaceFront:  return "front";
        case SkinFaceBack:   return "back";
        default:             return "?";
    }
}

IVec3 Skin::partSize(SkinPart part) const {
    int armWidth = model_ == SkinModel::Slim ? 3 : 4;
    switch (part) {
        case PartHead:     return {8, 8, 8};
        case PartBody:     return {8, 12, 4};
        case PartRightArm:
        case PartLeftArm:  return {armWidth, 12, 4};
        case PartRightLeg:
        case PartLeftLeg:  return {4, 12, 4};
        default:           return {0, 0, 0};
    }
}

SkinRect Skin::faceRect(SkinPart part, SkinLayer layer, SkinFace face) const {
    if (part < 0 || part >= PartCount || layer < 0 || layer >= LayerCount) return {};

    PartOrigin origin = kOrigins[part][layer];
    IVec3 size = partSize(part);
    return faceRectFor(origin.x, origin.y, size.x, size.y, size.z, face);
}

bool Skin::hasLayer(SkinPart part, SkinLayer layer) const {
    if (layer == LayerBase) return true;
    // A legacy skin carries only the hat; the other outer layers were added
    // with the 64x64 format and are blank in an expanded legacy skin.
    if (wasLegacy_ && part != PartHead) return false;
    return true;
}

Texture Skin::faceTexture(SkinPart part, SkinLayer layer, SkinFace face) const {
    SkinRect rect = faceRect(part, layer, face);
    if (!rect.valid() || texture_.empty()) return {};
    return texture_.subRegion(rect.x, rect.y, rect.width, rect.height);
}

ImageU8 Skin::expandLegacy(const ImageU8& legacy) {
    ImageU8 out(64, 64);

    // The original 64x32 becomes the top half unchanged.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) out.set(x, y, legacy.get(x, y));
    }

    for (const LegacyCopy& copy : kLegacyCopies) {
        for (int y = 0; y < copy.height; ++y) {
            for (int x = 0; x < copy.width; ++x) {
                // Horizontal flip: a limb seen from the other side.
                int sourceX = copy.srcX + (copy.width - 1 - x);
                out.set(copy.dstX + x, copy.dstY + y, legacy.get(sourceX, copy.srcY + y));
            }
        }
    }
    return out;
}

SkinModel Skin::detectModel(const ImageU8& image) {
    if (image.width() < 64 || image.height() < 64) return SkinModel::Classic;

    // A slim arm is three pixels wide, so the two rightmost columns of the
    // classic right-arm block are never used and are left transparent.
    for (int y = 20; y < 32; ++y) {
        for (int x = 54; x < 56; ++x) {
            if (image.get(x, y).a != 0) return SkinModel::Classic;
        }
    }
    for (int y = 16; y < 20; ++y) {
        for (int x = 50; x < 52; ++x) {
            if (image.get(x, y).a != 0) return SkinModel::Classic;
        }
    }
    return SkinModel::Slim;
}

void Skin::adoptImage(const ImageU8& image) {
    image_ = image;
    texture_.fromImage(image_);
    model_ = detectModel(image_);
}

bool Skin::replaceImage(const ImageU8& image) {
    if (image.empty() || image.width() != image_.width() || image.height() != image_.height()) {
        return false;
    }
    // Deliberately not re-running detectModel: arm width is a property of the
    // character, and a caller repainting a face has no business changing it.
    image_ = image;
    texture_.fromImage(image_);
    return true;
}

bool Skin::loadFromPng(const uint8_t* bytes, size_t size, std::string* error) {
    ImageU8 decoded;
    if (!pngDecode(bytes, size, decoded, error)) return false;

    if (decoded.width() != 64 || (decoded.height() != 64 && decoded.height() != 32)) {
        if (error) {
            *error = "skin: expected 64x64 or 64x32, got " + std::to_string(decoded.width()) +
                     "x" + std::to_string(decoded.height());
        }
        return false;
    }

    wasLegacy_ = decoded.height() == 32;
    adoptImage(wasLegacy_ ? expandLegacy(decoded) : decoded);
    // An expanded legacy skin has no slim variant.
    if (wasLegacy_) model_ = SkinModel::Classic;
    return true;
}

bool Skin::loadFromSource(const AssetSource& source, const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, error)) return false;
    return loadFromPng(bytes.data(), bytes.size(), error);
}

} // namespace blocky
