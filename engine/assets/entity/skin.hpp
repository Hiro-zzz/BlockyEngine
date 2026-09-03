#pragma once
// ENTITY ASSETS: player and mob skins.
//
// Nothing here is shared with the block texture path beyond Texture and
// AssetSource, and that is on purpose. A skin is a single 64x64 image whose
// meaning comes entirely from *where* things sit inside it: every body part
// is a box, and every box face is a fixed rectangle. There is no lookup by
// name, no resource-pack search, no animation strips, no biome tint.
//
// Handled here: the modern 64x64 layout, the legacy 64x32 layout (converted
// on load by mirroring the right limbs into the missing left slots), and both
// arm widths -- classic four pixels, slim three.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/texture.hpp"
#include "engine/core/math.hpp"

#include <string>

namespace blocky {

enum class SkinModel { Classic, Slim };

enum SkinPart {
    PartHead = 0,
    PartBody,
    PartRightArm,
    PartLeftArm,
    PartRightLeg,
    PartLeftLeg,
    PartCount
};

// Every part has a base layer and an outer layer -- hat, jacket, sleeves,
// trousers. The outer layer is drawn slightly larger and is usually mostly
// transparent.
enum SkinLayer { LayerBase = 0, LayerOuter, LayerCount };

// Which face of a part box. Same ordering as the block faces, so the two
// asset paths at least agree on what "face 3" means.
enum SkinFace {
    SkinFaceLeft = 0,   // -X, the entity's own left
    SkinFaceRight,      // +X
    SkinFaceBottom,     // -Y
    SkinFaceTop,        // +Y
    SkinFaceFront,      // -Z, the side the entity looks towards
    SkinFaceBack,       // +Z
    SkinFaceCount
};

struct SkinRect {
    int x = 0, y = 0, width = 0, height = 0;
    bool valid() const { return width > 0 && height > 0; }
};

class Skin {
public:
    bool loadFromPng(const uint8_t* bytes, size_t size, std::string* error = nullptr);
    bool loadFromSource(const AssetSource& source, const std::string& path,
                        std::string* error = nullptr);

    bool valid() const { return !texture_.empty(); }

    SkinModel model() const { return model_; }
    // Overrides the auto-detected arm width.
    void setModel(SkinModel model) { model_ = model; }

    // True when the file was a 64x32 skin that we expanded.
    bool wasLegacy() const { return wasLegacy_; }

    const Texture& texture() const { return texture_; }
    const ImageU8& image() const { return image_; }

    // Box dimensions of a part, in model pixels (a block is 16 of them).
    IVec3 partSize(SkinPart part) const;

    // Where in the skin one face of one part lives.
    SkinRect faceRect(SkinPart part, SkinLayer layer, SkinFace face) const;

    // Does this part actually carry an outer layer? Legacy skins only have
    // the hat.
    bool hasLayer(SkinPart part, SkinLayer layer) const;

    // Pull one face out as a standalone texture, for inspection or for
    // uploading to the viewport later.
    Texture faceTexture(SkinPart part, SkinLayer layer, SkinFace face) const;

    // Replace the pixels, keeping this Skin's identity. The rectangles a
    // model already holds stay valid, because the layout is fixed by size
    // alone -- which is what lets the face rig repaint eyes without every
    // box that reads this skin having to be rebuilt.
    //
    // Refuses an image of a different size, since that would move every
    // rectangle out from under whoever is holding one.
    bool replaceImage(const ImageU8& image);

    static const char* partName(SkinPart part);
    static const char* faceName(SkinFace face);

private:
    void adoptImage(const ImageU8& image);
    static ImageU8 expandLegacy(const ImageU8& legacy);
    static SkinModel detectModel(const ImageU8& image);

    ImageU8  image_;
    Texture  texture_;
    SkinModel model_ = SkinModel::Classic;
    bool wasLegacy_ = false;
};

} // namespace blocky
