#pragma once
// A bitmap font, laid out as a 16x16 grid of square cells.
//
// That is exactly Minecraft's own `textures/font/ascii.png`: 128x128, one
// bit per pixel, two palette entries that are both white, and a tRNS chunk
// making index 0 transparent. Our PNG decoder already reads indexed images at
// depth 1 with palette alpha, so the game font costs nothing to adopt.
//
// Glyph widths are not stored anywhere in the file -- the game derives them
// by finding the rightmost column of a cell that has any ink in it, and so do
// we. Without that every letter would be monospaced eight pixels wide and an
// `i` would sit in the middle of a puddle.
//
// A small built-in font covers the same grid when no game assets are around,
// keeping labels working the way an untextured scene still renders in flat
// palette colours.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/texture.hpp"
#include "engine/core/math.hpp"

#include <string>

namespace blocky {

class Font {
public:
    // Where the game keeps it.
    static constexpr const char* kMinecraftAscii =
        "assets/minecraft/textures/font/ascii.png";

    bool loadFromSource(const AssetSource& source, const std::string& path = kMinecraftAscii,
                        std::string* error = nullptr);
    bool loadFromPng(const uint8_t* bytes, size_t size, std::string* error = nullptr);

    // Uppercase, digits and common punctuation, compiled in. Lowercase is
    // served by its uppercase form; anything else comes back blank.
    void useBuiltin();

    bool loaded() const { return !texture_.empty(); }
    bool isBuiltin() const { return builtin_; }
    const Texture& texture() const { return texture_; }

    struct Glyph {
        Vec2  uvMin{};
        Vec2  uvMax{};
        float width = 0.0f;    // ink width, in glyph pixels
        float advance = 0.0f;  // pen movement, ink plus the gap
        bool  blank = true;    // space, and anything the font has no cell for
    };

    Glyph glyph(unsigned char code) const;

    // Cell size in pixels -- the height one line of text occupies.
    int cellPixels() const { return cell_; }

    // Total advance of a string in glyph pixels, with the trailing gap
    // trimmed so a measured label centres on what you can actually see.
    float measure(const std::string& text) const;

private:
    void measureGlyphs();

    Texture texture_;
    int  cell_ = 8;
    bool builtin_ = false;

    float width_[256]{};
    float advance_[256]{};
    bool  blank_[256]{};
};

} // namespace blocky
