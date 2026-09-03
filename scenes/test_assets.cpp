// Validates both asset paths against real game files, and writes contact
// sheets so the layouts can be checked by eye rather than by faith.
//
//   out/sheet_blocks.png  -- every palette block, all six faces
//   out/sheet_skin_*.png  -- every body part, both layers, all six faces
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

ImageU8::RGBA toRgba(Vec3 linear, float alpha) {
    Vec3 s = linearToSrgb(minv(maxv(linear, Vec3{0.0f}), Vec3{1.0f}));
    return {uint8_t(s.x * 255.0f + 0.5f), uint8_t(s.y * 255.0f + 0.5f),
            uint8_t(s.z * 255.0f + 0.5f), uint8_t(saturate(alpha) * 255.0f + 0.5f)};
}

// Draw a texture into `sheet` at cell (col,row), scaled up by `scale` and
// centred inside a cell of `cellW` x `cellH` source pixels.
void blitCell(ImageU8& sheet, const Texture& texture, int col, int row,
              int cellW, int cellH, int scale, int pad) {
    if (texture.empty()) return;

    int cellPixelW = cellW * scale + pad;
    int cellPixelH = cellH * scale + pad;
    int originX = pad + col * cellPixelW + (cellW - texture.width()) * scale / 2;
    int originY = pad + row * cellPixelH + (cellH - texture.height()) * scale / 2;

    for (int y = 0; y < texture.height() * scale; ++y) {
        for (int x = 0; x < texture.width() * scale; ++x) {
            int sx = x / scale, sy = y / scale;
            int dx = originX + x, dy = originY + y;
            if (dx < 0 || dy < 0 || dx >= sheet.width() || dy >= sheet.height()) continue;

            float alpha = texture.alphaAt(sx, sy);
            if (alpha <= 0.01f) continue;  // let the checkerboard show through
            sheet.set(dx, dy, toRgba(texture.texel(sx, sy), alpha));
        }
    }
}

ImageU8 makeSheet(int width, int height) {
    ImageU8 sheet(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Checkerboard, so transparent texels are visibly transparent.
            bool light = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t v = light ? 58 : 44;
            sheet.set(x, y, {v, v, uint8_t(v + 6), 255});
        }
    }
    return sheet;
}

void writeSkinSheet(const Skin& skin, const char* path) {
    constexpr int kScale = 6, kPad = 4;
    constexpr int kCellW = 8, kCellH = 12;  // largest face is 8 wide, 12 tall

    int cols = SkinFaceCount;
    int rows = int(PartCount) * int(LayerCount);
    int width  = kPad + cols * (kCellW * kScale + kPad);
    int height = kPad + rows * (kCellH * kScale + kPad);

    ImageU8 sheet = makeSheet(width, height);

    for (int part = 0; part < PartCount; ++part) {
        for (int layer = 0; layer < LayerCount; ++layer) {
            int row = part * int(LayerCount) + layer;
            for (int face = 0; face < SkinFaceCount; ++face) {
                Texture t = skin.faceTexture(SkinPart(part), SkinLayer(layer), SkinFace(face));
                blitCell(sheet, t, face, row, kCellW, kCellH, kScale, kPad);
            }
        }
    }

    std::string error;
    if (!pngSave(path, sheet, &error)) std::printf("  save failed: %s\n", error.c_str());
    else std::printf("  wrote %s  (rows: part x layer, columns: %s %s %s %s %s %s)\n", path,
                     Skin::faceName(SkinFaceLeft), Skin::faceName(SkinFaceRight),
                     Skin::faceName(SkinFaceBottom), Skin::faceName(SkinFaceTop),
                     Skin::faceName(SkinFaceFront), Skin::faceName(SkinFaceBack));
}

void checkSkin(const AssetSource& source, const char* path, SkinModel expected, const char* sheetPath) {
    Skin skin;
    std::string error;
    if (!skin.loadFromSource(source, path, &error)) {
        std::printf("  FAIL  %s: %s\n", path, error.c_str());
        ++gFailures;
        return;
    }

    std::printf("  %s -> %s%s\n", path,
                skin.model() == SkinModel::Slim ? "slim" : "classic",
                skin.wasLegacy() ? ", expanded from 64x32" : "");

    check(skin.model() == expected, "arm width detected correctly");
    check(skin.texture().width() == 64 && skin.texture().height() == 64, "skin normalised to 64x64");

    // The head front face must be fully opaque in any sane skin.
    Texture headFront = skin.faceTexture(PartHead, LayerBase, SkinFaceFront);
    check(headFront.width() == 8 && headFront.height() == 8, "head front face is 8x8");
    check(!headFront.hasAnyTransparency(), "head front face is opaque");

    // Arm faces must follow the model width.
    Texture armFront = skin.faceTexture(PartRightArm, LayerBase, SkinFaceFront);
    int expectedWidth = expected == SkinModel::Slim ? 3 : 4;
    check(armFront.width() == expectedWidth, "arm front width matches the model");

    writeSkinSheet(skin, sheetPath);
}

void writeBlockSheet(const BlockTextureLibrary& library, const BlockRegistry& registry) {
    constexpr int kScale = 5, kPad = 3, kCell = 16;

    std::vector<BlockId> textured;
    for (BlockId id = 1; id < BlockId(registry.size()); ++id) {
        if (library.hasTexture(id, FacePosY)) textured.push_back(id);
    }
    if (textured.empty()) return;

    int cols = FaceCount;
    int rows = int(textured.size());
    int width  = kPad + cols * (kCell * kScale + kPad);
    int height = kPad + rows * (kCell * kScale + kPad);

    ImageU8 sheet = makeSheet(width, height);

    // Rebuild each face as a small texture by sampling the library, which
    // also exercises the sampling path rather than reaching past it.
    for (int row = 0; row < rows; ++row) {
        for (int face = 0; face < FaceCount; ++face) {
            ImageU8 cell(kCell, kCell);
            for (int y = 0; y < kCell; ++y) {
                for (int x = 0; x < kCell; ++x) {
                    Vec2 uv{(float(x) + 0.5f) / float(kCell), (float(y) + 0.5f) / float(kCell)};
                    Vec3 c = library.sampleAlbedo(textured[size_t(row)], face, uv, Vec3{1, 0, 1});
                    cell.set(x, y, toRgba(c, 1.0f));
                }
            }
            Texture t;
            t.fromImage(cell);
            blitCell(sheet, t, face, row, kCell, kCell, kScale, kPad);
        }
    }

    std::string error;
    if (!pngSave("out/sheet_blocks.png", sheet, &error)) {
        std::printf("  save failed: %s\n", error.c_str());
        return;
    }
    std::printf("  wrote out/sheet_blocks.png  (%d blocks, columns: -X +X -Y +Y -Z +Z)\n", rows);
    for (int row = 0; row < rows; ++row) {
        std::printf("    row %2d  %s\n", row + 1, registry[textured[size_t(row)]].name.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : AssetSource::findClientJar();
    if (path.empty()) {
        std::printf("no Minecraft installation found; pass a jar, zip or folder as an argument\n");
        return 0;  // the engine must not require game files
    }

    AssetSource source;
    std::string error;
    if (!source.open(path, &error)) {
        std::printf("FAIL  open %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    std::printf("source: %s\n\n", source.description().c_str());

    // ---------------------------------------------------- game assets
    std::printf("GAME ASSETS -- block textures\n");
    BlockTextureLibrary blocks;
    if (!blocks.load(source, palette::registry(), palette::minecraftRules(), &error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        ++gFailures;
    } else {
        std::printf("  %zu textures for %zu blocks\n", blocks.textureCount(), blocks.texturedBlockCount());
        check(blocks.textureCount() > 15, "a plausible number of textures loaded");
        check(blocks.hasTexture(palette::Stone, FacePosY), "stone is textured");
        check(blocks.hasTexture(palette::GrassBlock, FacePosX), "grass side is textured");
        check(!blocks.hasTexture(palette::Water, FacePosY), "water is left to the medium code");

        for (const std::string& name : blocks.missing()) {
            std::printf("  missing: %s\n", name.c_str());
        }
        writeBlockSheet(blocks, palette::registry());
    }

    // -------------------------------------------------- entity assets
    std::printf("\nENTITY ASSETS -- skins\n");
    checkSkin(source, "assets/minecraft/textures/entity/player/wide/steve.png",
              SkinModel::Classic, "out/sheet_skin_steve.png");
    checkSkin(source, "assets/minecraft/textures/entity/player/slim/alex.png",
              SkinModel::Slim, "out/sheet_skin_alex.png");

    if (gFailures == 0) {
        std::printf("\nall asset tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
