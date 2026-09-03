#pragma once
// Camera model shared by the tracer and, later, the OpenGL viewport.
//
// Orthographic is a first-class mode, not an afterthought: the classic
// Minecraft "render of a build" look is an isometric orthographic projection,
// and it is what makes a scene read as a diorama rather than a screenshot.
#include "engine/core/math.hpp"
#include "engine/world/raycast.hpp"

namespace blocky {

class Camera {
public:
    enum class Projection { Perspective, Orthographic };

    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};

    Projection projection = Projection::Perspective;
    float fovY = radians(50.0f);  // perspective only
    float orthoHeight = 32.0f;    // orthographic only: visible world units
    float aspect = 16.0f / 9.0f;

    void lookAt(Vec3 eye, Vec3 at, Vec3 upHint = {0.0f, 1.0f, 0.0f}) {
        position = eye;
        target = at;
        up = upHint;
    }

    // Thin lens. `aperture` is the radius of the lens in world units: zero
    // keeps everything sharp, and anything above it throws whatever is not at
    // `focusDistance` out of focus. A few centimetres is plenty at block
    // scale -- this is what gives a build the look of a photographed model.
    float aperture = 0.0f;
    float focusDistance = 10.0f;

    // Screen coordinates are normalised: (0,0) top-left, (1,1) bottom-right.
    Ray generateRay(float u, float v) const;

    // Same, but with a sample on the lens: `lens` is a point in the unit disc.
    // Ignored entirely when the aperture is zero.
    Ray generateRay(float u, float v, Vec2 lens) const;

    // Focus on a point, rather than on a distance guessed by hand.
    void focusOn(Vec3 point) { focusDistance = std::max(0.01f, length(point - position)); }

    // View and projection matrices, for the OpenGL viewport.
    Mat4 viewMatrix() const { return blocky::lookAt(position, target, up); }
    Mat4 projectionMatrix(float zNear = 0.05f, float zFar = 2048.0f) const;

    // A true isometric view of `center`: yaw 45 degrees, pitch atan(1/sqrt2),
    // orthographic. `heightUnits` is how many blocks fit vertically.
    static Camera isometric(Vec3 center, float heightUnits, float distance = 256.0f,
                            float yawDegrees = 45.0f);
};

} // namespace blocky
