#pragma once
// A string, laid out into one sprite per glyph.
//
// The orientation belongs to the *label*, not to its letters. Turning each
// glyph towards the camera on its own would spin every one in place while
// leaving it strung out along the axis it was laid on -- readable letters in
// an unreadable line. So the layout happens in the rotated frame from the
// start: `right` and `up` come out of the label's own yaw and pitch, and the
// pen walks along them.
//
// That is also what makes `facing` honest. It resolves the direction once,
// here, while the scene is being built, and hands back ordinary geometry --
// quads that cast shadows, land in reflections and take part in the bounce
// like anything else. Nothing reorients at trace time, because a surface that
// turns to meet each ray is not a surface.
#include "engine/sprite/font.hpp"
#include "engine/sprite/sprite.hpp"

#include <string>
#include <vector>

namespace blocky {

struct TextStyle {
    enum class Align { Left, Center, Right };

    // Height of one line, in blocks. A glyph cell maps onto exactly this.
    float height = 0.5f;

    // Linear light. Wrap a hand-written colour in srgbToLinear.
    Vec3 color{1.0f, 1.0f, 1.0f};

    // Above zero the label glows. It will not light the scene -- see the note
    // on LightSet in sprite_set.hpp -- but it stops reading as unlit paint in
    // a dark corner.
    Vec3 emission{0.0f, 0.0f, 0.0f};

    Align align = Align::Center;

    // Multiples of `height` between consecutive baselines.
    float lineSpacing = 1.25f;

    // Orientation of the whole label. Zero faces +Z, the side a viewer stands
    // on to read an entity's front.
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;

    float alphaCutoff = 0.5f;
    bool  doubleSided = true;
};

namespace text {

// Glyph quads for `content`, centred on `position` both ways unless the
// alignment says otherwise. Newlines start a new line. Blank glyphs produce
// no sprite at all -- a space is pen movement, not geometry.
std::vector<Sprite> build(const Font& font, const std::string& content, Vec3 position,
                          const TextStyle& style = {});

// The same label, turned once to face `target` and then frozen there.
std::vector<Sprite> facing(const Font& font, const std::string& content, Vec3 position,
                           Vec3 target, const TextStyle& style = {});

// Width and height the label occupies, in blocks. Useful for sizing a plate
// to sit behind it.
Vec2 measure(const Font& font, const std::string& content, const TextStyle& style = {});

} // namespace text
} // namespace blocky
