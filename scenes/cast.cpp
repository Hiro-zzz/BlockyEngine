// Look at the four cast skins: an 8x upscale of each sheet, and the four
// characters standing in a neutral studio, front and back.
//
// A throwaway inspection scene -- it exists so the skins can be read before
// anything is staged with them.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/sprite/font.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

const char* kSkins[] = {
    "C:/Users/yueiw/Downloads/da14781977559b10.png",
    "C:/Users/yueiw/Downloads/7983af27cf866d82.png",
    "C:/Users/yueiw/Downloads/dcbbff5cb0c44e79.png",
    "C:/Users/yueiw/Downloads/59e4706ab7ec90e7.png",
};

// Nearest-neighbour blow-up, on a checkerboard so transparency reads.
ImageU8 upscale(const ImageU8& src, int factor) {
    ImageU8 out(src.width() * factor, src.height() * factor);
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            ImageU8::RGBA c = src.get(x / factor, y / factor);
            if (c.a < 255) {
                bool dark = ((x / factor) + (y / factor)) % 2 == 0;
                uint8_t bg = dark ? 90 : 130;
                float a = float(c.a) / 255.0f;
                c.r = uint8_t(float(c.r) * a + float(bg) * (1.0f - a));
                c.g = uint8_t(float(c.g) * a + float(bg) * (1.0f - a));
                c.b = uint8_t(float(c.b) * a + float(bg) * (1.0f - a));
                c.a = 255;
            }
            out.set(x, y, c);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    bool back = false;
    bool wantFont = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "back") == 0) back = true;
        else if (std::strcmp(argv[i], "font") == 0) wantFont = true;
    }

    // What the game font actually covers, blown up so the cells are readable.
    // The label API addresses a glyph by one byte, so whether Cyrillic is in
    // there at all decides what language the intertitles can be written in.
    if (wantFont) {
        AssetSource source;
        const std::string jar = AssetSource::findClientJar();
        std::string err;
        if (jar.empty() || !source.open(jar, &err)) {
            std::printf("no jar: %s\n", err.c_str());
            return 1;
        }
        std::vector<uint8_t> bytes;
        if (!source.read(Font::kMinecraftAscii, bytes, &err)) {
            std::printf("no ascii.png: %s\n", err.c_str());
            return 1;
        }
        ImageU8 sheet;
        if (!pngDecode(bytes.data(), bytes.size(), sheet, &err)) {
            std::printf("decode: %s\n", err.c_str());
            return 1;
        }
        std::printf("ascii.png is %dx%d\n", sheet.width(), sheet.height());
        // White ink on transparent reads as nothing; invert to dark on light.
        for (int y = 0; y < sheet.height(); ++y) {
            for (int x = 0; x < sheet.width(); ++x) {
                ImageU8::RGBA c = sheet.get(x, y);
                uint8_t v = c.a > 0 ? 0 : 255;
                sheet.set(x, y, {v, v, v, 255});
            }
        }
        pngSave("out/cast_font.png", upscale(sheet, 4), &err);
        std::printf("wrote out/cast_font.png\n");
        return 0;
    }

    std::string error;
    std::vector<Skin> skins(std::size(kSkins));
    std::vector<EntityModel> models(std::size(kSkins));

    for (size_t i = 0; i < std::size(kSkins); ++i) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(kSkins[i], bytes, &error)) {
            std::printf("read %s failed: %s\n", kSkins[i], error.c_str());
            return 1;
        }
        if (!skins[i].loadFromPng(bytes.data(), bytes.size(), &error)) {
            std::printf("decode %s failed: %s\n", kSkins[i], error.c_str());
            return 1;
        }
        models[i] = buildPlayerModel(skins[i]);

        char path[128];
        std::snprintf(path, sizeof path, "out/cast_skin%zu.png", i);
        pngSave(path, upscale(skins[i].image(), 8), &error);

        std::printf("%zu: %s  %dx%d  %s%s\n", i, kSkins[i],
                    skins[i].image().width(), skins[i].image().height(),
                    skins[i].model() == SkinModel::Slim ? "slim" : "classic",
                    skins[i].wasLegacy() ? ", legacy 64x32" : "");
        std::printf("    wrote %s\n", path);
    }

    // ---- studio: a grey floor and a grey wall, nothing to distract
    Scene scene(palette::registry());
    scene.world.fillBox({-12, -1, -8}, {12, -1, 8}, palette::Stone);
    scene.world.fillBox({-12, 0, 5}, {12, 8, 5}, palette::Stone);

    EntitySet entities;
    for (size_t i = 0; i < std::size(kSkins); ++i) {
        Entity e;
        e.model = &models[i];
        e.skin = &skins[i];
        e.position = Vec3{-2.7f + 1.8f * float(i), 0.0f, 0.0f};
        e.yawDegrees = back ? 180.0f : 0.0f;
        e.pose = Pose::standing();
        entities.add(e);
    }
    scene.entities = &entities;

    scene.sun.direction = normalize(Vec3{-0.35f, 0.62f, -0.70f});
    scene.sun.intensity = 9.0f;
    scene.sun.angularRadiusDegrees = 2.0f;
    scene.sky.intensity = 1.1f;
    scene.overrideBackground = true;
    scene.background = srgbToLinear(Vec3{0.30f, 0.32f, 0.36f});

    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(34.0f);
    scene.camera.lookAt({0.0f, 1.05f, back ? 6.5f : -6.5f}, {0.0f, 1.00f, 0.0f});

    PathSettings settings;
    settings.width = 1400;
    settings.height = 620;
    settings.samplesPerPixel = 48;
    settings.maxBounces = 4;

    RenderStats stats;
    Image frame = renderPath(scene, settings, &stats);
    std::printf("rendered in %.1f s\n", stats.seconds);

    const char* path = back ? "out/cast_back.png" : "out/cast_front.png";
    if (!pngSave(path, frame, ToneParams{}, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
