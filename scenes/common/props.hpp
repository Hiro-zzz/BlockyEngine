#pragma once
// The models and the skin the physics demos throw around.
//
// Shared rather than copied, because two scenes wanting the same crate is
// exactly how two crates end up subtly different -- and then a measurement
// taken in one scene stops meaning anything in the other.
#include "engine/assets/entity/skin.hpp"
#include "engine/core/file.hpp"
#include "engine/core/image.hpp"
#include "engine/core/png.hpp"
#include "engine/prop/voxel_model.hpp"

#include <string>
#include <vector>

namespace demo {

inline blocky::VoxelMaterial colour(float r, float g, float b, float roughness = 0.85f) {
    blocky::VoxelMaterial material;
    material.albedo = blocky::srgbToLinear(blocky::Vec3{r, g, b});
    material.roughness = roughness;
    return material;
}

// A crate. The bands are not decoration: a uniformly coloured cube tumbles
// invisibly, and the whole point of the picture is to show that it turned.
inline blocky::VoxelModel buildCrate() {
    blocky::VoxelModel model;
    model.resize({8, 8, 8});
    uint16_t body = model.addMaterial(colour(0.72f, 0.52f, 0.30f));
    uint16_t band = model.addMaterial(colour(0.34f, 0.26f, 0.20f));
    uint16_t top = model.addMaterial(colour(0.86f, 0.68f, 0.42f));

    for (int y = 0; y < 8; ++y) {
        for (int z = 0; z < 8; ++z) {
            for (int x = 0; x < 8; ++x) {
                bool edge = (x == 0 || x == 7) && (z == 0 || z == 7);
                uint16_t material = body;
                if (edge || y == 0 || y == 7) material = band;
                if (y == 7 && !edge) material = top;
                model.set({x, y, z}, material);
            }
        }
    }
    return model;
}

inline blocky::VoxelModel buildPlank() {
    blocky::VoxelModel model;
    model.resize({18, 3, 6});
    uint16_t wood = model.addMaterial(colour(0.60f, 0.44f, 0.26f));
    uint16_t dark = model.addMaterial(colour(0.42f, 0.30f, 0.18f));
    for (int y = 0; y < 3; ++y)
        for (int z = 0; z < 6; ++z)
            for (int x = 0; x < 18; ++x)
                model.set({x, y, z}, (x % 6 == 0) ? dark : wood);
    return model;
}

// An L, so that a shape whose centre of mass is not its bounding-box centre
// gets exercised. It should settle onto its long face, not balance.
inline blocky::VoxelModel buildEll() {
    blocky::VoxelModel model;
    model.resize({10, 10, 5});
    uint16_t stone = model.addMaterial(colour(0.55f, 0.57f, 0.60f));
    uint16_t moss = model.addMaterial(colour(0.36f, 0.48f, 0.32f));
    for (int y = 0; y < 10; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 10; ++x)
                if (x < 4 || y < 4) model.set({x, y, z}, (y == 9 || x == 9) ? moss : stone);
    return model;
}

// ------------------------------------------------------------------- a skin
//
// A character the demos can use with no Minecraft installed at all. The engine
// has always been able to render without game files -- a scene falls back to
// the flat palette -- and a sandbox that needed a jar before it could show a
// person would break that.
//
// Built the long way round, and the round is the only public route: a Skin
// gets its rectangle table from an image, so a blank one is encoded with our
// own PNG codec, loaded, painted through the rectangles it then reports, and
// handed back with `replaceImage`.
inline blocky::Skin buildBlockySkin(blocky::ImageU8::RGBA shirt, blocky::ImageU8::RGBA trousers,
                                    blocky::ImageU8::RGBA face) {
    using namespace blocky;

    ImageU8 image(64, 64);
    Skin skin;
    {
        std::vector<uint8_t> bytes = pngEncode(image);
        skin.loadFromPng(bytes.data(), bytes.size(), nullptr);
    }
    if (!skin.valid()) return skin;

    const ImageU8::RGBA dark{40, 36, 34, 255};

    auto fill = [&](SkinPart part, SkinFace side, ImageU8::RGBA colour) {
        SkinRect rect = skin.faceRect(part, LayerBase, side);
        if (!rect.valid()) return;
        for (int y = 0; y < rect.height; ++y)
            for (int x = 0; x < rect.width; ++x) image.set(rect.x + x, rect.y + y, colour);
    };

    auto paint = [&](SkinPart part, ImageU8::RGBA colour) {
        for (int side = 0; side < SkinFaceCount; ++side) fill(part, SkinFace(side), colour);
    };

    paint(PartHead, face);
    paint(PartBody, shirt);
    paint(PartRightArm, shirt);
    paint(PartLeftArm, shirt);
    paint(PartRightLeg, trousers);
    paint(PartLeftLeg, trousers);

    // Hair, hands and shoes, so the figure reads as a person rather than as a
    // stack of coloured boxes.
    fill(PartHead, SkinFaceTop, dark);
    fill(PartRightArm, SkinFaceBottom, face);
    fill(PartLeftArm, SkinFaceBottom, face);
    fill(PartRightLeg, SkinFaceBottom, dark);
    fill(PartLeftLeg, SkinFaceBottom, dark);

    // Two eyes on the front of the head. The face rect is stored as a viewer
    // sees it, so left and right here are the viewer's -- which is why this
    // looks mirrored against the entity's own sides and is not.
    SkinRect front = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
    if (front.valid() && front.width >= 8 && front.height >= 8) {
        for (int eye = 0; eye < 2; ++eye) {
            int x = front.x + (eye == 0 ? 2 : front.width - 3);
            int y = front.y + front.height / 2 - 1;
            image.set(x, y, dark);
            image.set(x, y + 1, ImageU8::RGBA{235, 235, 240, 255});
        }
    }

    skin.replaceImage(image);
    return skin;
}

// The project ships one real skin; use it when it is there, and fall back to
// the built-in figure when it is not. A demo must never fail to start over an
// asset.
inline blocky::Skin loadDemoSkin(const std::string& path = "assets/skins/charlie.png") {
    using namespace blocky;

    std::vector<uint8_t> bytes;
    if (readFileBytes(path, bytes, nullptr)) {
        Skin skin;
        if (skin.loadFromPng(bytes.data(), bytes.size(), nullptr) && skin.valid()) return skin;
    }
    return buildBlockySkin({92, 118, 176, 255}, {58, 60, 74, 255}, {214, 174, 140, 255});
}

} // namespace demo
