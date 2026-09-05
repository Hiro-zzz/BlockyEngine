// forge: the editor built on BlockyEngine.
//
// Two documents and one window. A voxel model comes out as C++ that calls
// `voxelize::fromLayers`, or as a sheet of slices; a pixel canvas comes out as
// the PNG the engine already reads as a block texture, an item, or a skin.
//
// ------------------------------------------------------------- why a plugin
//
// Not a scene: a scene renders one thing and exits, and this has a document,
// an undo stack and a person holding the mouse. Not part of the game either,
// which has its own world to keep. So it is the third shape the build knows
// about -- a directory under `plugins/` that becomes its own executable
// linking `blocky`, the same rule `game/` follows.
//
// Nothing is loaded into the engine at runtime and nothing here is a library.
// A plugin in this project is a **program built on the engine**: dynamic
// loading would mean a stable C ABI, a registry and a version story, which is
// real machinery for one consumer that does not need it.
//
//   forge                     a 16-cube model
//   forge size=24             a bigger one
//   forge name=mug            what the exported files are called
//   forge canvas              start on a 16x16 texture instead
//   forge sprites             start arranging sprites around the model
//   forge skin                start on a 64x64 skin, with the layout guides
//   forge open=<file.png>     load a texture, or a sheet named like a sheet
//   forge snapshot <png>      one frame to a file, for a build script
//   forge mcspace=minecraft   namespace for the Minecraft export (P)
//   forge mcfolder=block      block/ instead of item/ inside the pack
//   forge selftest            the checks in selftest.hpp, no window
#include "engine/core/file.hpp"
#include "engine/platform/window.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/render/gl/viewport.hpp"
#include "engine/scene/scene.hpp"

#include "plugins/forge/canvasview.hpp"
#include "plugins/forge/palette.hpp"
#include "plugins/forge/sculptview.hpp"
#include "plugins/forge/spriteview.hpp"
#include "plugins/forge/selftest.hpp"
#include "plugins/forge/studio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace blocky;
using namespace forge;

namespace {

// The bands themselves are in `studio.hpp`, because the two views draw
// against them too and a second copy here would be a second answer.
constexpr float kSwatch = 40.0f;
constexpr float kSwatchGap = 6.0f;

float paletteLeft(int width) {
    const int count = int(swatches().size());
    const float total = float(count) * kSwatch + float(count - 1) * kSwatchGap;
    return std::floor((float(width) - total) * 0.5f);
}

float paletteTop(int height) { return float(height) - kPaletteBand + 30.0f; }

// Which swatch the pointer is over, or -1.
int swatchAt(Vec2 cursor, int width, int height) {
    const float top = paletteTop(height);
    if (cursor.y < top || cursor.y > top + kSwatch) return -1;

    const float left = paletteLeft(width);
    const float step = kSwatch + kSwatchGap;
    const float offset = cursor.x - left;
    if (offset < 0.0f) return -1;

    const int index = int(offset / step);
    if (index < 0 || index >= int(swatches().size())) return -1;
    // The gap between two swatches belongs to neither.
    if (offset - float(index) * step > kSwatch) return -1;
    return index;
}

// Both bands run the full width, so only the height matters.
bool pointerOverInterface(Vec2 cursor, int height) {
    return cursor.y < kHeaderBand || cursor.y > float(height) - kPaletteBand;
}

void drawChrome(const Studio& studio, Overlay& overlay) {
    const float width = float(overlay.width());
    const float height = float(overlay.height());

    overlay.verticalGradient(0.0f, 0.0f, width, kHeaderBand, rgb8(16, 18, 21, 0.92f),
                             rgb8(16, 18, 21, 0.55f));
    overlay.textShadowed(16.0f, 16.0f, "FORGE", 3.0f, rgb8(255, 170, 40));

    // Right-aligned by measuring, not by a guessed offset. A hint that runs
    // off the edge of the window is worse than no hint: it looks like the
    // window is the wrong size.
    const char* nextMode = studio.mode == Mode::Sculpt   ? "Tab: canvas"
                           : studio.mode == Mode::Canvas ? "Tab: sprites"
                                                         : "Tab: model";
    const std::string hints = nextMode;
    overlay.textShadowed(width - 16.0f - overlay.measure(hints, 2.0f), 22.0f, hints, 2.0f,
                         rgb8(150, 158, 170));

    const std::string second = "E export   P pack   Ctrl+Z undo";
    overlay.textShadowed(width - 16.0f - overlay.measure(second, 2.0f), 46.0f, second, 2.0f,
                         rgb8(112, 120, 132));

    // The palette.
    overlay.verticalGradient(0.0f, height - kPaletteBand, width, kPaletteBand,
                             rgb8(16, 18, 21, 0.55f), rgb8(16, 18, 21, 0.95f));

    const float left = paletteLeft(overlay.width());
    const float top = paletteTop(overlay.height());
    for (int i = 0; i < int(swatches().size()); ++i) {
        const float x = left + float(i) * (kSwatch + kSwatchGap);
        overlay.rect(x, top, kSwatch, kSwatch, swatchOverlay(i));

        if (i == studio.swatch) {
            overlay.frame(x - 3.0f, top - 3.0f, kSwatch + 6.0f, kSwatch + 6.0f, 3.0f,
                          rgb8(255, 170, 40));
        }
    }
    overlay.textShadowed(left, top + kSwatch + 10.0f, swatchName(studio.swatch), 2.0f,
                         rgb8(178, 184, 192));

    if (!studio.status.empty()) {
        overlay.textShadowed(16.0f, statusLineY(height), studio.status, 2.0f, rgb8(255, 210, 130));
    }
}

// A floor for the model to stand on, so a shadow has somewhere to land and
// the eye has something to judge scale against.
void buildStudioFloor(World& world) {
    for (int z = -10; z <= 10; ++z) {
        for (int x = -10; x <= 10; ++x) {
            const bool light = ((x + 10) / 2 + (z + 10) / 2) % 2 == 0;
            world.set({x, -1, z}, light ? Tile : Slab);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // Before anything is built, because it shares nothing with running but
    // the code it is checking.
    if (argc > 1 && std::string(argv[1]) == "selftest") return runSelfTest();

    Studio studio;
    studio.begin();

    std::string snapshotPath;
    std::string openPath;
    int side = 16;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "snapshot" && i + 1 < argc) {
            snapshotPath = argv[++i];
        } else if (argument.rfind("size=", 0) == 0) {
            side = std::atoi(argument.c_str() + 5);
            if (side < 1 || side > 128) {
                std::printf("forge: size must be between 1 and 128\n");
                return 1;
            }
        } else if (argument.rfind("name=", 0) == 0) {
            studio.name = argument.substr(5);
        } else if (argument.rfind("open=", 0) == 0) {
            openPath = argument.substr(5);
        } else if (argument.rfind("mcspace=", 0) == 0) {
            studio.minecraft.space = edit::resourceName(argument.substr(8));
        } else if (argument.rfind("mcfolder=", 0) == 0) {
            studio.minecraft.folder = edit::resourceName(argument.substr(9));
        } else if (argument.rfind("mcformat=", 0) == 0) {
            studio.minecraft.packFormat = std::atoi(argument.c_str() + 9);
        } else if (argument == "canvas") {
            studio.mode = Mode::Canvas;
        } else if (argument == "sprites") {
            studio.mode = Mode::Sprites;
        } else if (argument == "skin") {
            studio.mode = Mode::Canvas;
            studio.canvas.create(edit::CanvasPreset::Skin);
        } else {
            std::printf("forge: unknown argument \"%s\"\n", argument.c_str());
            return 1;
        }
    }

    if (side != 16) studio.sculpt.create(side);
    if (!openPath.empty()) studio.importFile(openPath);

    Scene scene(registry());
    buildStudioFloor(scene.world);

    scene.sun.direction = normalize(Vec3{0.45f, 0.78f, 0.44f});
    scene.sun.intensity = 3.1f;
    scene.camera.aspect = 16.0f / 9.0f;

    PropSet props;
    scene.props = &props;

    SpriteSet spriteSet;
    scene.sprites = &spriteSet;

    Orbit orbit;
    SculptView sculptView;
    CanvasView canvasView;
    SpriteView spriteView;

    // What the plate was last built for. Trimming and loading both change the
    // grid, and a plate left at the old size is worse than none: it says the
    // model stands somewhere it does not.
    VoxelModel plate;
    IVec3 platedFor{0, 0, 0};

    orbit.frame(studio.sculpt);

    ViewportEffects effects;
    effects.bloomIntensity = 0.18f;
    effects.saturation = 1.04f;
    effects.vignette = 0.18f;

    bool freeCursor = true;   // the pointer is the tool; it is never captured

    ViewportSettings settings;
    settings.title = "forge -- BlockyEngine";
    settings.hostCamera = true;
    settings.freeCursor = &freeCursor;
    settings.effects = &effects;
    settings.snapshotPath = snapshotPath;
    settings.snapshotOverlay = true;   // the interface is the thing being shown
    settings.snapshotFrames = 2;

    settings.onFrame = [&](const ViewportFrame& frame) {
        studio.tick(frame.dt);

        const int width = frame.input ? frame.input->width() : 1280;
        const int height = frame.input ? frame.input->height() : 720;
        const float aspect = height > 0 ? float(width) / float(height) : 16.0f / 9.0f;

        orbit.apply(scene.camera, aspect);

        if (frame.input) {
            const Window& window = *frame.input;
            const Vec2 cursor = window.mousePosition();
            const bool overUi = pointerOverInterface(cursor, height);

            // A click on the palette belongs to the palette, whatever mode is
            // running. Handled before the views so neither has to know the
            // interface is there beyond "not for you".
            if (window.mouseLeftPressed()) {
                const int picked = swatchAt(cursor, width, height);
                if (picked >= 0) studio.swatch = picked;
            }

            if (window.keyPressed(0x09)) studio.toggleMode();   // Tab

            if (window.keyDown(key::Control) && window.keyPressed('Z')) studio.undo();
            if (window.keyDown(key::Control) && window.keyPressed('Y')) studio.redo();

            if (window.keyPressed('E')) studio.exportDocument();
            if (window.keyPressed('P')) studio.exportForMinecraft();

            if (window.keyPressed('M') && studio.mode != Mode::Sprites) {
                if (studio.mode == Mode::Sculpt) {
                    studio.sculpt.mirrorX = !studio.sculpt.mirrorX;
                    studio.say(studio.sculpt.mirrorX ? "mirror on" : "mirror off", 1.5f);
                } else {
                    studio.canvas.mirrorX = !studio.canvas.mirrorX;
                    studio.say(studio.canvas.mirrorX ? "mirror on" : "mirror off", 1.5f);
                }
            }

            if (window.keyPressed('G') && studio.mode == Mode::Canvas) {
                canvasView.showGuides = !canvasView.showGuides;
            }

            if (window.keyPressed('T') && studio.mode == Mode::Sculpt) {
                studio.sculpt.trimToContents();
                const IVec3 dims = studio.sculpt.dims();
                studio.say("trimmed to " + std::to_string(dims.x) + "x" + std::to_string(dims.y) +
                               "x" + std::to_string(dims.z) + " (undo history cleared)",
                           5.0f);
            }

            for (int digit = 0; digit < 8; ++digit) {
                if (window.keyPressed('1' + digit)) studio.swatch = digit;
            }

            switch (studio.mode) {
                case Mode::Sculpt:
                    sculptView.update(studio, window, orbit, scene.camera, width, height, overUi);
                    break;
                case Mode::Canvas:
                    canvasView.update(studio, window, width, height, overUi);
                    break;
                case Mode::Sprites:
                    spriteView.update(studio, window, orbit, scene.camera, width, height, overUi);
                    break;
            }
        }

        const IVec3 dims = studio.sculpt.dims();
        if (!(platedFor == dims)) {
            buildPlate(plate, dims);
            orbit.frame(studio.sculpt);
            platedFor = dims;
        }

        // Rebuilt every frame from the document, which is cheap for two props
        // and means there is no second place that has to be told the model
        // changed. The meshes behind them are not rebuilt every frame: the
        // viewport compares `VoxelModel::stamp` and re-meshes only when it
        // moved.
        // Rebuilt from the document for the same reason the props are: one
        // place that knows what is in the world, and it is the document.
        spriteSet.clear();
        for (const Sprite& sprite : studio.sprites.sprites()) spriteSet.add(sprite);
        spriteSet.build();

        props.clear();
        props.addTransformed(&plate, plateToWorld(dims));
        if (!studio.sculpt.model().empty()) {
            props.addTransformed(&studio.sculpt.model(), modelToWorld(dims));
        }
        props.build();
        return true;
    };

    settings.onOverlay = [&](Overlay& overlay) {
        switch (studio.mode) {
            case Mode::Sculpt:  sculptView.draw(studio, overlay); break;
            case Mode::Canvas:  canvasView.draw(studio, overlay); break;
            case Mode::Sprites: spriteView.draw(studio, overlay); break;
        }
        drawChrome(studio, overlay);
    };

    return runViewport(scene, settings);
}
