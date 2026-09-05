#pragma once
// A pixel canvas: the 2D half of the editor.
//
// The document is an `ImageU8` and nothing else, because that is already what
// the engine reads on both sides. A 16x16 canvas saved as PNG is a block
// texture; a 64x64 one is a skin; a 16x16 item texture is a prop the moment
// `item::buildModel` extrudes it. Inventing a document type would mean
// converting on the way out, and the conversion is where the difference
// between "what I drew" and "what the game shows" gets in.
//
// ---------------------------------------------------------------- no layers
//
// Deliberately one image. A skin's base and outer layers are *regions of the
// same 64x64 picture*, not stacked sheets, and a canvas with its own layer
// stack would have to flatten them into that picture on save -- inventing a
// second truth about a file whose whole meaning is where things sit inside
// it.
//
// -------------------------------------------------------- strokes and drags
//
// Every tool commits its own undo step, unless a stroke is already open, in
// which case it joins that one. A drag is therefore `beginStroke`, a pencil
// call per frame, `endStroke`, and a bucket fill is just a bucket fill --
// without the tools needing two entry points each.
#include "engine/core/image.hpp"
#include "engine/edit/history.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace edit {

// What a new canvas is for. The sizes are not suggestions: a block texture
// that is not 16 across cannot go into the texture array, and a skin that is
// not 64x64 has no layout.
enum class CanvasPreset {
    BlockTexture,   // 16x16
    ItemTexture,    // 16x16, extruded into a prop by item::buildModel
    Skin,           // 64x64
};

// A named rectangle drawn over the canvas to say what a region means. Used
// for the skin layout, where a picture is only correct in the places the
// renderer looks.
struct Guide {
    int x = 0, y = 0, width = 0, height = 0;
    std::string label;
};

class Canvas {
public:
    // Transparent, which is the right empty for both a cutout texture and a
    // skin's outer layer.
    void create(int width, int height);
    void create(CanvasPreset preset);

    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;

    int width() const { return image_.width(); }
    int height() const { return image_.height(); }
    bool empty() const { return image_.empty(); }

    const ImageU8& image() const { return image_; }

    bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < image_.width() && y < image_.height();
    }
    ImageU8::RGBA pixel(int x, int y) const;

    // --------------------------------------------------------------- tools
    //
    // All of them clip; a brush that runs off the edge is normal, not an
    // error the caller should have to check for.
    void pencil(int x, int y, ImageU8::RGBA colour);
    void line(int x0, int y0, int x1, int y1, ImageU8::RGBA colour);
    void rectOutline(int x0, int y0, int x1, int y1, ImageU8::RGBA colour);
    void rectFill(int x0, int y0, int x1, int y1, ImageU8::RGBA colour);

    // Four-connected, matching the seed's exact RGBA. Exact rather than
    // tolerant because this is pixel art: two texels that look the same and
    // are not are a mistake worth seeing, not one worth smoothing over.
    void bucket(int x, int y, ImageU8::RGBA colour);

    void fillAll(ImageU8::RGBA colour);

    // Paints the mirrored column as well, about the canvas centre. Right for
    // a texture; **not** a skin's left-arm-from-right-arm mirror, which is a
    // move between two rectangles rather than a reflection of the sheet.
    bool mirrorX = false;

    // ------------------------------------------------------------- strokes
    void beginStroke(std::string name);
    void endStroke();

    History& history() { return history_; }
    const History& history() const { return history_; }

    bool undo();
    bool redo();

    // Layout guides for the current size, or empty when the size is not one
    // with a layout. Bounding boxes of whole parts rather than of every
    // face: seventy-two rectangles over a 64-pixel picture is not a guide,
    // it is a grid.
    std::vector<Guide> guides(bool slimArms = false) const;

private:
    // The history's cell value is the pixel, packed. One document, one
    // meaning for a uint32.
    static uint32_t pack(ImageU8::RGBA c);
    static ImageU8::RGBA unpack(uint32_t value);

    uint32_t index(int x, int y) const { return uint32_t(y) * uint32_t(image_.width()) + uint32_t(x); }

    // The one place a pixel changes. Everything above goes through it, so
    // there is exactly one place that records history and one that mirrors.
    void put(int x, int y, ImageU8::RGBA colour);
    void putRaw(int x, int y, ImageU8::RGBA colour);

    // Opens a stroke if none is open, and says whether it has to close it.
    bool openStroke(const char* name);

    ImageU8 image_;
    History history_;
};

} // namespace edit
} // namespace blocky
