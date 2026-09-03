// One island, one camera, one set of lights -- five looks.
//
// The point of the scene is the comparison, so nothing else is allowed to
// change between the frames. Everything that differs is named in the table at
// the bottom of main, and every difference lands in one of exactly two places:
//
//   material style  (scene/material_style.hpp)  -- before light transport
//   stylise         (render/post/stylize.hpp)   -- after it
//
// Which of the two a look belongs in is not a matter of taste. Plastic needs
// a highlight, and a highlight is light that must be traced, so it cannot be
// added afterwards. Cel banding needs the *finished* illumination in order to
// quantise it, so it cannot be decided per bounce. Pixelation needs whole
// pixels, which do not exist until the frame does.
//
//   scene_styles           full quality, all five
//   scene_styles draft     small and fast
//   scene_styles cel       just one of them, by name
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/core/png.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/render/trace/direct.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"
#include "scenes/common/island.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

void setUpCamera(Scene& scene) {
    // Three-quarter view down onto the lookout, close enough that block
    // texture is legible -- cel shading has nothing to prove on a frame with
    // no texture in it.
    scene.camera.projection = Camera::Projection::Perspective;
    scene.camera.fovY = radians(38.0f);
    scene.camera.lookAt({44.0f, 38.0f, 47.0f}, {-4.0f, 20.0f, -2.0f});
}

void setUpLight(Scene& scene) {
    scene.sun.direction = normalize(Vec3{-0.44f, 0.68f, 0.59f});
    scene.sun.color = Vec3{1.0f, 0.94f, 0.82f};
    scene.sun.intensity = 12.0f;
    scene.sun.angularRadiusDegrees = 1.2f;

    scene.sky.zenith  = Vec3{0.14f, 0.28f, 0.60f};
    scene.sky.horizon = Vec3{0.68f, 0.76f, 0.90f};
    scene.sky.intensity = 1.05f;
    scene.ambientStrength = 0.95f;
}

bool save(const char* path, const Image& frame, const ToneParams& tone) {
    std::string error;
    if (!pngSave(path, frame, tone, &error)) {
        std::printf("  save failed: %s\n", error.c_str());
        return false;
    }
    std::printf("  wrote %s\n", path);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool draft = false;
    std::string only;
    std::string assetPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "draft") == 0) draft = true;
        else if (std::strcmp(argv[i], "flat") == 0 || std::strcmp(argv[i], "realistic") == 0 ||
                 std::strcmp(argv[i], "plastic") == 0 || std::strcmp(argv[i], "cel") == 0 ||
                 std::strcmp(argv[i], "pixel") == 0) {
            only = argv[i];
        } else {
            assetPath = argv[i];
        }
    }

    Scene scene(palette::registry());
    island::build(scene.world);
    setUpCamera(scene);
    setUpLight(scene);

    // Textures are optional as everywhere else, but this scene is much less
    // interesting without them: half of what cel shading has to survive is
    // texture detail.
    AssetSource source;
    BlockTextureLibrary blockTextures;
    if (assetPath.empty()) assetPath = AssetSource::findClientJar();
    if (!assetPath.empty() && source.open(assetPath, nullptr) &&
        blockTextures.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
        scene.blockTextures = &blockTextures;
    }
    std::printf("[styles] %llu blocks, textures %s\n",
                (unsigned long long)scene.world.blockCount(),
                scene.blockTextures ? "game" : "flat palette");

    PathSettings settings;
    settings.width  = draft ? 640 : 1280;
    settings.height = draft ? 360 : 720;
    settings.samplesPerPixel = draft ? 16 : 64;
    settings.maxBounces = draft ? 4 : 8;
    settings.progress = false;

    auto wanted = [&](const char* name) { return only.empty() || only == name; };

    // Renders the scene as it currently stands and hands back both the frame
    // and its auxiliary buffers, so a style that needs geometry can have it.
    auto trace = [&](RenderTargets& targets) {
        RenderStats stats;
        Image raw = renderPath(scene, settings, &stats, &targets);
        std::printf("  %.1f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);
        return raw;
    };

    // ---------------------------------------------------------------- flat
    // No shaders at all: one brightness per face direction, no shadow rays,
    // no sky samples. This is the direct renderer, not the path tracer, and
    // it finishes in a fraction of a second.
    if (wanted("flat")) {
        std::printf("[flat] fixed per-face light, no shadows\n");
        DirectSettings direct;
        direct.width = settings.width;
        direct.height = settings.height;
        direct.samplesPerPixel = draft ? 2 : 4;
        direct.flatLighting = true;

        RenderStats stats;
        Image frame = renderDirect(scene, direct, &stats);
        std::printf("  %.2f s, %.0f Mrays\n", stats.seconds, double(stats.totalRays) / 1e6);

        // Nothing in a flat frame exceeds one, so a filmic curve would only
        // crush a picture that is already display-referred. Straight through.
        ToneParams tone;
        tone.curve = Tonemap::None;
        save(draft ? "out/style_flat_draft.png" : "out/style_flat.png", frame, tone);
    }

    // ----------------------------------------------------------- realistic
    // The baseline every other frame is a departure from.
    if (wanted("realistic")) {
        std::printf("[realistic] path traced, materials as declared\n");
        scene.materialStyle = MaterialStyle::realistic();

        RenderTargets targets;
        trace(targets);
        Image frame = denoise(targets, {});

        BloomSettings bloom;
        bloom.threshold = 1.5f;
        bloom.intensity = 0.045f;
        applyBloom(frame, bloom);

        ToneParams tone;
        tone.curve = Tonemap::ACES;
        save(draft ? "out/style_realistic_draft.png" : "out/style_realistic.png", frame, tone);
    }

    // ------------------------------------------------------------- plastic
    // Every surface becomes a dielectric with a tight white coat. Note what
    // happens to the water: nothing. A refracting medium is left alone, since
    // a plastic ocean would stop being an ocean.
    if (wanted("plastic")) {
        std::printf("[plastic] dielectric coat on everything, metals included\n");
        scene.materialStyle = MaterialStyle::plastic();

        RenderTargets targets;
        trace(targets);
        Image frame = denoise(targets, {});

        BloomSettings bloom;
        bloom.threshold = 1.3f;
        bloom.intensity = 0.075f;   // the coat makes small, bright glints worth blooming
        applyBloom(frame, bloom);

        GradeSettings grade;
        grade.contrast = 1.06f;
        applyGrade(frame, grade);

        ToneParams tone;
        tone.curve = Tonemap::ACES;
        save(draft ? "out/style_plastic_draft.png" : "out/style_plastic.png", frame, tone);
    }

    // ----------------------------------------------------------------- cel
    // Matte first, then band the illumination and ink the geometry. The matte
    // style is not decoration: a highlight is a smooth gradient a few pixels
    // wide, and quantising it turns it into a staircase of hard rings.
    if (wanted("cel")) {
        std::printf("[cel] matte materials, banded illumination, geometric outline\n");
        scene.materialStyle = MaterialStyle::matte();

        RenderTargets targets;
        trace(targets);

        // Denoise first. The bands are decided per pixel, so any noise left in
        // the illumination becomes two neighbouring pixels landing in
        // different bands -- speckle with hard edges, far uglier than the
        // noise it came from.
        targets.color = denoise(targets, {});

        CelSettings cel;
        cel.bands = 4;
        cel.range = 2.4f;
        cel.bandGamma = 0.7f;
        cel.depthThreshold = 0.4f;
        cel.normalThreshold = 0.3f;
        cel.outlineWidth = draft ? 1 : 2;
        cel.saturation = 1.15f;
        Image frame = celShade(targets, cel);

        ToneParams tone;
        tone.curve = Tonemap::ACES;
        save(draft ? "out/style_cel_draft.png" : "out/style_cel.png", frame, tone);
    }

    // --------------------------------------------------------------- pixel
    // A path-traced frame crushed onto a coarse grid and a coarse palette.
    // Averaging each cell rather than sampling one pixel of it is what keeps
    // the result from crawling with the tracer's own noise.
    if (wanted("pixel")) {
        std::printf("[pixel] full trace, then crushed to a grid\n");
        scene.materialStyle = MaterialStyle::realistic();

        RenderTargets targets;
        trace(targets);
        Image frame = denoise(targets, {});

        PixelateSettings pixel;
        pixel.factor = draft ? 4 : 8;
        pixel.levels = 12;
        pixelate(frame, pixel);

        ToneParams tone;
        tone.curve = Tonemap::ACES;
        save(draft ? "out/style_pixel_draft.png" : "out/style_pixel.png", frame, tone);
    }

    return 0;
}
