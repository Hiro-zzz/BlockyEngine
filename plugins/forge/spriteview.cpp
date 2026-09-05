#include "plugins/forge/spriteview.hpp"

#include "engine/platform/window.hpp"

#include "plugins/forge/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

namespace {

// Where the ray lands in the world, whether that is on the model or on the
// plate under it. Returns false when it lands on neither.
bool surfacePoint(const Studio& studio, Vec3 origin, Vec3 direction, Vec3& world, Vec3& normal) {
    const Mat4 toWorld = modelToWorld(studio.sculpt.dims());
    const Mat4 toLocal = inverseAffine(toWorld);

    const Vec3 localOrigin = transformPoint(toLocal, origin);
    const Vec3 localDirection = transformDir(toLocal, direction);

    const edit::Pick pick = studio.sculpt.pick(localOrigin, localDirection);
    if (pick.hit) {
        // The ray parameter is the same on both sides of the transform -- that
        // is what the uniform scale buys -- so the world point is the world
        // ray at the same t, with no second transform to get wrong.
        world = origin + direction * pick.t;
        normal = normalize(transformDir(toWorld, pick.normal));
        return true;
    }

    // The plate, at local y = 0.
    if (localDirection.y > -1e-6f) return false;

    const float t = -localOrigin.y / localDirection.y;
    if (t <= 0.0f) return false;

    const Vec3 landed = localOrigin + localDirection * t;
    const IVec3 dims = studio.sculpt.dims();
    if (landed.x < 0.0f || landed.z < 0.0f || landed.x > float(dims.x) ||
        landed.z > float(dims.z)) {
        return false;
    }

    world = origin + direction * t;
    normal = Vec3{0.0f, 1.0f, 0.0f};
    return true;
}

} // namespace

void SpriteView::update(Studio& studio, const Window& window, Orbit& orbit, const Camera& camera,
                        int width, int height, bool pointerOverUi) {
    hovered = -1;

    // The same camera controls as the model mode, deliberately: switching
    // modes should not change what the right button does.
    if (window.mouseRightDown()) {
        const Vec2 delta = window.mouseDelta();
        orbit.yawDegrees -= delta.x * 0.35f;
        orbit.pitchDegrees = std::min(88.0f, std::max(-88.0f, orbit.pitchDegrees + delta.y * 0.35f));
    }
    const float wheel = window.wheelDelta();
    if (wheel != 0.0f) {
        orbit.distance = std::min(12.0f, std::max(0.35f, orbit.distance * std::pow(0.88f, wheel)));
    }

    if (window.keyPressed(0xDB)) size = std::max(0.02f, size * 0.8f);    // [
    if (window.keyPressed(0xDD)) size = std::min(4.0f, size * 1.25f);    // ]
    if (window.keyPressed('L')) emissive = !emissive;

    if (pointerOverUi || width <= 0 || height <= 0) return;

    const Vec2 cursor = window.mousePosition();
    const float u = cursor.x / float(width);
    const float v = cursor.y / float(height);
    if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) return;

    const Ray ray = camera.generateRay(u, v);
    hovered = studio.sprites.pick(ray.origin, ray.direction);

    if (!window.mouseLeftPressed()) return;

    // Shift removes what the pointer is over. Edge-triggered, because a held
    // button over a heap of sprites would empty it.
    if (window.keyDown(key::Shift)) {
        if (hovered >= 0) {
            studio.sprites.removeAt(size_t(hovered));
            studio.say("removed a sprite", 1.5f);
        }
        return;
    }

    Vec3 world{}, normal{};
    if (!surfacePoint(studio, ray.origin, ray.direction, world, normal)) return;

    Sprite sprite;
    // Lifted off the surface by a fraction of its own size, so it does not
    // fight the face it was placed on for the same depth.
    sprite.position = world + normal * (size * 0.25f);
    sprite.size = {size, size};

    const Vec3 colour = swatchMaterial(studio.swatch).albedo;
    sprite.tint = colour;
    if (emissive) sprite.emission = colour * 4.0f;

    // Turned to face the eye once, here, and frozen. `aimAt` is the engine's
    // own verb for this and takes a point to look at.
    aimAt(sprite, camera.position);

    studio.sprites.add(sprite);
}

void SpriteView::draw(const Studio& studio, Overlay& overlay) const {
    char line[192];
    std::snprintf(line, sizeof(line), "SPRITES  %zu placed   size %.2f%s", studio.sprites.size(),
                  double(size), emissive ? "   GLOWING" : "");
    overlay.textShadowed(16.0f, 44.0f, line, 2.0f, rgb8(226, 230, 236));

    if (hovered >= 0) {
        std::snprintf(line, sizeof(line), "over sprite %d", hovered);
        overlay.textShadowed(16.0f, 68.0f, line, 2.0f, rgb8(255, 170, 40));
    } else {
        overlay.textShadowed(16.0f, 68.0f, "click the model or the plate to place", 2.0f,
                             rgb8(150, 158, 170));
    }

    const char* help =
        "LMB place   Shift+LMB remove   RMB orbit   [ ] size   L glow   E export";
    overlay.textShadowed(16.0f, helpLineY(float(overlay.height())), help, 2.0f,
                         rgb8(132, 140, 152));

    // The one thing about sprites that surprises people, said where they are.
    if (studio.sprites.size() > 0 && emissive) {
        overlay.textShadowed(float(overlay.width()) - 300.0f, 44.0f,
                             "glowing sprites light nothing", 2.0f, rgb8(200, 150, 90));
    }
}

} // namespace forge
