#pragma once
// The game's mark, drawn rather than shipped.
//
// A `.ico` file would need an `.rc`, `rc.exe` in the build and a second image
// format in the tree -- three new things for one picture, in a project whose
// whole argument is that it writes its own. This draws the same mark the title
// screen draws, at icon size, out of the same font.
//
// Using the font matters beyond tidiness: the K in the window corner and the K
// on the front page are then the *same letterform*, and stay the same one when
// the font changes. A hand-plotted grid of pixels here would be a second
// drawing of the same letter, which is a second drawing to keep in step.
#include "engine/core/image.hpp"
#include "engine/sprite/font.hpp"

namespace game {

// `size` is the icon's edge in pixels. 64 is what Windows wants for the big
// one and scales down cleanly to the 16-pixel title-bar version, because
// everything here is drawn on a grid that divides.
inline blocky::ImageU8 buildIcon(int size = 64) {
    using blocky::Font;
    using blocky::ImageU8;

    ImageU8 icon(size, size);

    const ImageU8::RGBA ground{18, 21, 24, 255};
    const ImageU8::RGBA edge{255, 170, 40, 255};
    const ImageU8::RGBA mark{255, 170, 40, 255};
    const ImageU8::RGBA shade{10, 12, 14, 255};

    auto put = [&](int x, int y, ImageU8::RGBA c) {
        if (x >= 0 && y >= 0 && x < size && y < size) icon.set(x, y, c);
    };
    auto fill = [&](int x0, int y0, int w, int h, ImageU8::RGBA c) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) put(x, y, c);
    };

    // A unit that divides the icon, so every edge lands on a whole pixel at
    // 64, at 32 and at 16. Nothing here is allowed to be 1.5 pixels wide.
    const int u = std::max(1, size / 16);

    // The block: a dark square with the corners knocked off, so it reads as a
    // die rather than as a button.
    fill(u, u, size - u * 2, size - u * 2, ground);
    fill(u, u, u, u, {0, 0, 0, 0});
    fill(size - u * 2, u, u, u, {0, 0, 0, 0});
    fill(u, size - u * 2, u, u, {0, 0, 0, 0});
    fill(size - u * 2, size - u * 2, u, u, {0, 0, 0, 0});

    // The frame, in the accent, following the same knocked corners.
    fill(u * 2, u, size - u * 4, u, edge);
    fill(u * 2, size - u * 2, size - u * 4, u, edge);
    fill(u, u * 2, u, size - u * 4, edge);
    fill(size - u * 2, u * 2, u, size - u * 4, edge);
    put(u * 2 - u, u * 2 - u + u, edge);

    // A heavier stub at the lower right, the same one the wordmark carries.
    fill(size - u * 3, size - u * 4, u, u * 3, edge);
    fill(size - u * 4, size - u * 3, u * 3, u, edge);

    // The K itself, from the built-in font: five by seven ink pixels, scaled
    // to whatever multiple fits with a margin left over.
    Font font;
    font.useBuiltin();

    const Font::Glyph glyph = font.glyph('K');
    const int cell = font.cellPixels();
    if (!glyph.blank && cell > 0) {
        const int inkW = int(glyph.width);
        const int scale = std::max(1, (size - u * 6) / 7);

        const int drawW = inkW * scale, drawH = 7 * scale;
        const int x0 = (size - drawW) / 2;
        const int y0 = (size - drawH) / 2;

        const int texX = int(glyph.uvMin.x * float(font.texture().width()) + 0.5f);
        const int texY = int(glyph.uvMin.y * float(font.texture().height()) + 0.5f);

        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < inkW; ++gx) {
                if (font.texture().alphaAt(texX + gx, texY + gy) < 0.5f) continue;

                // A one-pixel drop shadow under each block of the letter, so
                // the K sits in the square instead of floating on it.
                fill(x0 + gx * scale + scale / 4, y0 + gy * scale + scale / 4, scale, scale, shade);
                fill(x0 + gx * scale, y0 + gy * scale, scale, scale, mark);
            }
        }
    }
    return icon;
}

} // namespace game
