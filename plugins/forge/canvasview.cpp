#include "plugins/forge/canvasview.hpp"

#include "engine/platform/window.hpp"

#include "plugins/forge/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

namespace {

// Reserved at the top for the header and at the bottom for the palette and
// the two lines above it, so the canvas is never underneath any of them.
constexpr float kTopMargin = kHeaderBand + 24.0f;
constexpr float kBottomMargin = kPaletteBand + 64.0f;

} // namespace

bool CanvasLayout::toPixel(float screenX, float screenY, int width, int height, int& px,
                           int& py) const {
    if (zoom <= 0.0f) return false;

    const float localX = (screenX - x) / zoom;
    const float localY = (screenY - y) / zoom;
    if (localX < 0.0f || localY < 0.0f) return false;

    px = int(localX);
    py = int(localY);
    return px < width && py < height;
}

CanvasLayout CanvasView::layout(const Studio& studio, int width, int height) const {
    CanvasLayout out;

    const int cw = studio.canvas.width(), ch = studio.canvas.height();
    if (cw <= 0 || ch <= 0) return out;

    const float availableW = float(width) - 48.0f;
    const float availableH = float(height) - kTopMargin - kBottomMargin;

    // Whole pixels per texel, at least one. A canvas bigger than the window
    // is still drawn at 1:1 and clipped rather than squeezed, because a
    // squeezed pixel is a lie about what the file holds.
    out.zoom = std::max(1.0f, std::floor(std::min(availableW / float(cw), availableH / float(ch))));

    out.x = std::floor((float(width) - out.zoom * float(cw)) * 0.5f);
    out.y = std::floor(kTopMargin + (availableH - out.zoom * float(ch)) * 0.5f);
    return out;
}

void CanvasView::update(Studio& studio, const Window& window, int width, int height,
                        bool pointerOverUi) {
    hoverX = hoverY = -1;

    const CanvasLayout place = layout(studio, width, height);
    const Vec2 cursor = window.mousePosition();

    int px = 0, py = 0;
    const bool over = !pointerOverUi && place.toPixel(cursor.x, cursor.y, studio.canvas.width(),
                                                      studio.canvas.height(), px, py);
    if (over) {
        hoverX = px;
        hoverY = py;
    }

    const bool erase = window.keyDown(key::Shift);
    const bool bucket = window.keyDown(key::F);

    if (window.mouseLeftDown()) {
        if (!painting) {
            // A bucket is a single act, so it does not open a drag: it commits
            // its own step and the button being held afterwards does nothing.
            if (bucket) {
                if (over) {
                    studio.canvas.bucket(px, py,
                                         erase ? ImageU8::RGBA{0, 0, 0, 0} : studio.pixelColour());
                }
                painting = true;
                return;
            }
            studio.canvas.beginStroke(erase ? "erase" : "paint");
            painting = true;
        }
        if (over && !bucket) {
            studio.canvas.pencil(px, py, erase ? ImageU8::RGBA{0, 0, 0, 0} : studio.pixelColour());
        }
    } else if (painting) {
        studio.canvas.endStroke();
        painting = false;
    }
}

void CanvasView::draw(const Studio& studio, Overlay& overlay) const {
    const int cw = studio.canvas.width(), ch = studio.canvas.height();
    if (cw <= 0 || ch <= 0) return;

    const CanvasLayout place = layout(studio, overlay.width(), overlay.height());
    const float zoom = place.zoom;

    char line[192];
    std::snprintf(line, sizeof(line), "CANVAS  %dx%d   %s", cw, ch, studio.name.c_str());
    overlay.textShadowed(16.0f, 44.0f, line, 2.0f, rgb8(226, 230, 236));

    if (hoverX >= 0) {
        std::snprintf(line, sizeof(line), "at  %d, %d", hoverX, hoverY);
        overlay.textShadowed(16.0f, 68.0f, line, 2.0f, rgb8(150, 158, 170));
    }

    // The world is still being drawn behind this -- it is the same viewport --
    // so it gets covered. Judging a colour against a lit checkerboard that
    // happens to be there is not judging it at all.
    overlay.rect(0.0f, 0.0f, float(overlay.width()), float(overlay.height()),
                 rgb8(22, 24, 28, 0.94f));

    // The board, one shade off the background so the canvas edge is visible
    // even where the art is transparent.
    overlay.rect(place.x - 4.0f, place.y - 4.0f, zoom * float(cw) + 8.0f, zoom * float(ch) + 8.0f,
                 rgb8(18, 20, 24));

    // Transparency, as the chequer everybody already reads as "nothing here".
    // Eight texels to a square, so it never lines up with the art's own
    // rhythm and cannot be mistaken for part of it.
    const Rgba lightSquare = rgb8(58, 62, 70);
    const Rgba darkSquare = rgb8(46, 50, 57);
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            overlay.rect(place.x + zoom * float(x), place.y + zoom * float(y), zoom, zoom,
                         light ? lightSquare : darkSquare);
        }
    }

    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const ImageU8::RGBA pixel = studio.canvas.pixel(x, y);
            if (pixel.a == 0) continue;
            overlay.rect(place.x + zoom * float(x), place.y + zoom * float(y), zoom, zoom,
                         rgb8(pixel.r, pixel.g, pixel.b, float(pixel.a) / 255.0f));
        }
    }

    // Skin guides, when the size has a layout to guide. Drawn as frames over
    // the art rather than under it: they say where the renderer will look,
    // and something you cannot see through is not a guide.
    if (showGuides) {
        for (const edit::Guide& guide : studio.canvas.guides()) {
            overlay.frame(place.x + zoom * float(guide.x), place.y + zoom * float(guide.y),
                          zoom * float(guide.width), zoom * float(guide.height), 1.0f,
                          rgb8(255, 170, 40, 0.45f));
        }
    }

    if (hoverX >= 0) {
        overlay.frame(place.x + zoom * float(hoverX), place.y + zoom * float(hoverY), zoom, zoom,
                      std::max(1.0f, zoom * 0.12f), rgb8(255, 255, 255, 0.8f));
    }

    const char* help = "LMB paint   Shift+LMB erase   F+LMB fill   M mirror   G guides";
    overlay.textShadowed(16.0f, helpLineY(float(overlay.height())), help, 2.0f,
                         rgb8(132, 140, 152));

    if (studio.canvas.mirrorX) {
        overlay.textShadowed(float(overlay.width()) - 132.0f, 44.0f, "MIRROR X", 2.0f,
                             rgb8(255, 170, 40));
    }
}

} // namespace forge
