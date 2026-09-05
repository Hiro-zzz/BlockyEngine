#pragma once
// The 2D half of the window: the canvas, drawn as rectangles on the overlay.
//
// Rectangles rather than a texture, and the reason is that the overlay is
// already exactly this -- quads that batch into one draw call. A 64x64 canvas
// is four thousand of them and goes to the card once, which is cheaper than
// the upload it would take to do it "properly", and it means a pixel on
// screen is a pixel in the document with no filtering in between. At this
// size a texture would have to be point-sampled and aligned by hand to look
// the same, so the shortcut is also the accurate route.
//
// Zoom is a whole number of screen pixels per texel, always. Pixel art at a
// fractional scale has rows one pixel taller than their neighbours, and no
// amount of care in the drawing survives that.
#include "engine/render/gl/overlay.hpp"

#include "plugins/forge/studio.hpp"

namespace blocky {
class Window;
}

namespace forge {

using namespace blocky;

// Where the canvas sits on screen this frame, in overlay pixels.
struct CanvasLayout {
    float x = 0.0f, y = 0.0f;
    float zoom = 1.0f;

    bool toPixel(float screenX, float screenY, int width, int height, int& px, int& py) const;
};

struct CanvasView {
    bool painting = false;
    bool showGuides = true;

    int hoverX = -1, hoverY = -1;

    CanvasLayout layout(const Studio& studio, int width, int height) const;

    void update(Studio& studio, const Window& window, int width, int height, bool pointerOverUi);
    void draw(const Studio& studio, Overlay& overlay) const;
};

} // namespace forge
