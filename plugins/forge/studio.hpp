#pragma once
// The document forge is holding, and what it does with it.
//
// Two documents rather than one, kept side by side. A voxel model and a pixel
// canvas are not the same thing with a different depth: one has a slice
// plane, a camera and a face to point at, the other has layers of meaning
// fixed by where things sit in a 64x64 sheet. `docs/architecture.md` says the
// same about `assets/blocks` and `assets/entity` -- one API over both would
// have to be the worse of each.
//
// What they do share is the verb list at the top of this file: switch, undo,
// export, and say so.
#include "engine/edit/canvas.hpp"
#include "engine/edit/minecraft.hpp"
#include "engine/edit/sprite_doc.hpp"
#include "engine/edit/sculpt.hpp"

#include <string>

namespace forge {

enum class Mode { Sculpt, Canvas, Sprites };

// Interface furniture, in overlay pixels, in one place because three files
// draw against it. Fixed rather than proportional for the reason the spawn
// menu gives: a swatch has to be big enough to hit with a mouse, and that is
// a size in pixels rather than a fraction of however wide somebody dragged
// the window.
inline constexpr float kHeaderBand = 84.0f;
inline constexpr float kPaletteBand = 132.0f;

// The two lines that sit between the canvas and the palette. Both measured
// up from the bottom, so neither can end up underneath a swatch.
inline float helpLineY(float windowHeight) { return windowHeight - kPaletteBand - 26.0f; }
inline float statusLineY(float windowHeight) { return windowHeight - kPaletteBand - 52.0f; }

// Where exported files land. Under `out/` with everything else the engine
// writes, because it is output: reproducible from the document, and the
// document is what somebody keeps.
inline const char* kOutputDirectory = "out/forge";

struct Studio {
    Mode mode = Mode::Sculpt;

    blocky::edit::Sculpt sculpt;
    blocky::edit::Canvas canvas;
    blocky::edit::SpriteDoc sprites;

    // Silver, not the ink at index four: the first stroke somebody makes
    // should be visible against a dark studio without their having chosen a
    // colour first.
    int swatch = 1;
    std::string name = "untitled";

    // What the last action did, shown for a few seconds and then gone. A
    // status line that stays forever is one nobody reads, because it is
    // always saying something.
    std::string status;
    float statusSeconds = 0.0f;

    void begin();
    void tick(float dt);
    void say(std::string line, float seconds = 3.5f);

    // Switching is not a mode flag over one document: each of the three keeps
    // its own undo stack, so going to the canvas, drawing, coming back and
    // pressing Ctrl+Z undoes the last thing done *here*.
    void toggleMode();

    void undo();
    void redo();

    // The selected colour, as whichever of the two forms the caller needs.
    // The voxel one is added to the model's palette on the way past, which is
    // what makes the model carry only the colours it actually uses.
    uint16_t voxelMaterial();
    blocky::ImageU8::RGBA pixelColour() const;

    // Where a Minecraft export goes and under what names. Set from the
    // command line, because "which namespace" and "block or item" are facts
    // about somebody else's project that nothing here can work out.
    blocky::edit::McExportOptions minecraft;

    // Writes whatever the current mode has, in this engine's own forms: source
    // and a picture, for both halves. The status line names every file.
    void exportDocument();

    // The same document in Minecraft's forms, as a resource pack tree. A
    // model becomes a JSON model plus a palette texture; a canvas becomes the
    // texture it already is.
    //
    // A separate verb rather than a third file from `exportDocument`, because
    // it answers a different question. The C++ is what this repository keeps;
    // the pack is a thing you hand to another program, and handing it over is
    // a decision, not a save.
    void exportForMinecraft();

    // Reads a file back in. A sheet is recognised by the size in its name, a
    // plain PNG becomes a canvas.
    void importFile(const std::string& path);
};

} // namespace forge
