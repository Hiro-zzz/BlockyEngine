#pragma once
// Two-dimensional drawing over a finished frame: panels, and text.
//
// The viewport had no interface of any kind, and for what it was built to do
// that was right -- a tool for finding a camera angle needs a title bar and
// nothing else. A game needs a menu, a crosshair and a row of blocks, and
// none of those are expressible as geometry in the world.
//
// ------------------------------------------------------------------ the unit
//
// Everything here is in **pixels from the top-left of the window**, because
// that is the only coordinate system a person laying out a menu can hold in
// their head. Normalised device coordinates would put the origin in the
// middle, flip the vertical, and make every size depend on the aspect ratio.
//
// ------------------------------------------------------------- one draw call
//
// Quads accumulate into one buffer between `begin` and `end`, and go to the
// card once. That is not an optimisation reached for early: it is what lets
// the caller write a menu as a list of statements without thinking about
// batching, which is the whole reason an immediate-mode interface is pleasant
// to write against.
//
// Solid quads and glyph quads share the buffer and the shader, told apart by
// a per-vertex flag. Two passes would mean either two draw calls or an
// ordering rule between them, and a panel drawn after the text it is behind
// is a bug nobody enjoys finding.
#include "engine/core/math.hpp"
#include "engine/render/gl/gl_resources.hpp"
#include "engine/sprite/font.hpp"

#include <string>
#include <vector>

namespace blocky {

// Straight sRGB, the way a designer would write it, plus coverage. The
// overlay draws after tonemapping -- it is interface, not light, and running
// a menu through ACES would mean the colour picked is not the colour shown.
struct Rgba {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

inline Rgba rgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }

// From the 0-255 form a palette is usually written in.
inline Rgba rgb8(int r, int g, int b, float a = 1.0f) {
    return {float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, a};
}

class Overlay {
public:
    // Builds the shader and the font. `source` may be null, in which case the
    // built-in font is used -- the same fallback the tracer's text takes, and
    // the reason a menu works with no game files present.
    bool create(const AssetSource* source, std::string* error = nullptr);
    void destroy();
    bool ready() const { return shader_.valid(); }

    const Font& font() const { return font_; }

    // Between these two, nothing reaches the card. Depth testing is off and
    // blending is on for the duration, and both are put back afterwards.
    void begin(int widthPixels, int heightPixels);
    void end();

    int width() const { return width_; }
    int height() const { return height_; }

    // ------------------------------------------------------------- shapes
    void rect(float x, float y, float w, float h, Rgba colour);

    // A one-pixel-ish frame drawn as four rects, `thickness` wide, inside the
    // given box.
    void frame(float x, float y, float w, float h, float thickness, Rgba colour);

    // Vertical gradient. Cheap, and the difference between a panel that looks
    // placed and one that looks pasted.
    void verticalGradient(float x, float y, float w, float h, Rgba top, Rgba bottom);

    // --------------------------------------------------------------- text
    //
    // `scale` is pixels per glyph pixel, so a scale of 3 on an 8-pixel font
    // gives 24-pixel-tall text. Integers keep the glyphs crisp; the font is a
    // bitmap and half a texel is a blurry letter.
    void text(float x, float y, const std::string& line, float scale, Rgba colour);

    // The same, with a hard shadow one glyph pixel down and right. Minecraft
    // draws its interface this way, and the reason is legibility over an
    // arbitrary world rather than decoration.
    void textShadowed(float x, float y, const std::string& line, float scale, Rgba colour);

    // Width in pixels of what `text` would draw, for centring.
    float measure(const std::string& line, float scale) const;
    float lineHeight(float scale) const;

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
        float textured;   // 0 solid, 1 sample the font
    };

    void push(float x, float y, float w, float h, Vec2 uvMin, Vec2 uvMax, Rgba colour,
              float textured);

    ShaderProgram shader_;
    GlTexture2D   fontTexture_;
    Font          font_;

    unsigned vao_ = 0, vbo_ = 0;
    size_t   capacity_ = 0;

    std::vector<Vertex> vertices_;
    int width_ = 0, height_ = 0;
    bool inFrame_ = false;
};

} // namespace blocky
