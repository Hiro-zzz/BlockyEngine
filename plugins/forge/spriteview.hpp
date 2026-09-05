#pragma once
// The third half of the window, which makes it a third: arranging sprites in
// the space around the model.
//
// A sprite is a quad standing in the world -- a mote in a shaft of light, a
// spark over a fire, a glyph of floating text. Scenes place them today as a
// column of numbers in a `.cpp`, which is fine for six and unbearable for
// sixty. This puts them where they look right and writes those numbers.
//
// ------------------------------------------------ frozen facing, on purpose
//
// A placed sprite is turned to face the camera **once**, at the moment it is
// placed, and then it is ordinary geometry. That is not a shortcut here; it
// is the engine's whole position on billboards, spelled out in
// `sprite.hpp`: a quad that reorients itself per ray has no coherent answer
// for a reflection or a shadow. So this tool does what `aimAt` does, at the
// only moment a viewer is defined.
//
// It follows that turning the camera afterwards shows the sprite edge-on, and
// that is correct rather than a bug. Placing from where the shot will be taken
// is the workflow.
#include "engine/render/gl/overlay.hpp"
#include "engine/scene/camera.hpp"

#include "plugins/forge/sculptview.hpp"
#include "plugins/forge/studio.hpp"

namespace blocky {
class Window;
}

namespace forge {

using namespace blocky;

struct SpriteView {
    // The next sprite's edge, in blocks. A Minecraft particle is about 0.1,
    // which is the scale this starts near.
    float size = 0.12f;

    // An emissive sprite glows and shows up in reflections. It is **not**
    // added to the light set -- see `sprite_set.hpp` -- so a spark lights
    // nothing, and the tool says so rather than letting it be discovered.
    bool emissive = false;

    int hovered = -1;

    void update(Studio& studio, const Window& window, Orbit& orbit, const Camera& camera,
                int width, int height, bool pointerOverUi);

    void draw(const Studio& studio, Overlay& overlay) const;
};

} // namespace forge
