#pragma once
// A sprite is a flat, alpha-cut textured quad standing in world space.
//
// It is the one primitive behind both features that wanted it: a particle is
// a sprite, and a line of text is a row of them, one per glyph. Neither is
// animated -- a particle here is a frozen instant, snow caught in the air,
// which is exactly what a still render wants.
//
// Nothing about it chases the camera at trace time. A quad that reorients
// itself per ray has no coherent answer for a reflection or a shadow, and the
// whole renderer is built on every hit being a real surface. What `aimAt`
// gives instead is a billboard resolved *once*, while the scene is being
// built, and frozen into ordinary geometry from then on.
#include "engine/assets/texture.hpp"
#include "engine/core/math.hpp"

#include <vector>

namespace blocky {

struct Sprite {
    // Centre of the quad, in blocks.
    Vec3 position{};

    // Width and height, in blocks. A Minecraft particle is about 0.1.
    Vec2 size{1.0f, 1.0f};

    // Applied as yaw about +Y, then pitch about the turned +X, then roll
    // about the quad's own normal. All zero leaves the normal on +Z, facing
    // a viewer standing on the +Z side -- the same side an entity faces.
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float rollDegrees = 0.0f;

    // Null renders the quad in a flat `tint`, which is all a dust mote needs.
    const Texture* texture = nullptr;

    // Sub-rectangle of the texture, so one atlas serves every glyph in a
    // string without a texture per letter.
    Vec2 uvMin{0.0f, 0.0f};
    Vec2 uvMax{1.0f, 1.0f};

    // Linear light, multiplied into the sampled texel.
    Vec3 tint{1.0f, 1.0f, 1.0f};

    // Linear radiance, may exceed one. An emissive sprite glows and shows up
    // in reflections, but it is *not* added to LightSet -- see sprite_set.hpp.
    Vec3 emission{0.0f, 0.0f, 0.0f};

    float roughness = 1.0f;

    // Texels below this alpha let the ray straight through. This is what
    // makes a glyph a glyph rather than a rectangle it sits on.
    float alphaCutoff = 0.5f;

    // Visible from behind, with the normal flipped and the texture read
    // through the back -- mirrored, the way a sign in the game reads.
    bool doubleSided = true;
};

// Turns the sprite to face a point: after this its normal aims from its own
// position at `target`. Roll is left alone, so text stays upright.
void aimAt(Sprite& sprite, Vec3 target);

// The same, for a whole batch -- every glyph of a label turns together.
void aimAt(std::vector<Sprite>& sprites, Vec3 target);

// Yaw and pitch, in degrees, whose quad normal is `direction`.
void orientationTowards(Vec3 direction, float& yawDegrees, float& pitchDegrees);

} // namespace blocky
