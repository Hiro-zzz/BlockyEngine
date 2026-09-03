#include "engine/scene/camera.hpp"

namespace blocky {

Ray Camera::generateRay(float u, float v) const {
    return generateRay(u, v, Vec2{0.0f, 0.0f});
}

Ray Camera::generateRay(float u, float v, Vec2 lens) const {
    Vec3 forward = normalize(target - position);
    Vec3 right = normalize(cross(forward, up));
    Vec3 trueUp = cross(right, forward);

    // Map [0,1] to [-1,1], with v flipped so that v = 0 is the top row.
    float sx = 2.0f * u - 1.0f;
    float sy = 1.0f - 2.0f * v;

    if (projection == Projection::Orthographic) {
        float halfHeight = orthoHeight * 0.5f;
        float halfWidth = halfHeight * aspect;
        Ray ray;
        ray.origin = position + right * (sx * halfWidth) + trueUp * (sy * halfHeight);
        ray.direction = forward;
        return ray;
    }

    float halfHeight = std::tan(fovY * 0.5f);
    float halfWidth = halfHeight * aspect;

    Ray ray;
    ray.origin = position;
    ray.direction = normalize(forward + right * (sx * halfWidth) + trueUp * (sy * halfHeight));

    if (aperture > 0.0f) {
        // Everything on the focal plane must stay put, so the ray is re-aimed
        // from the displaced lens point at the same focal point. Points off
        // that plane land on a disc instead of a point -- the blur.
        float focalScale = focusDistance / std::max(dot(ray.direction, forward), 1e-4f);
        Vec3 focalPoint = ray.origin + ray.direction * focalScale;

        Vec3 offset = right * (lens.x * aperture) + trueUp * (lens.y * aperture);
        ray.origin = position + offset;
        ray.direction = normalize(focalPoint - ray.origin);
    }
    return ray;
}

Mat4 Camera::projectionMatrix(float zNear, float zFar) const {
    if (projection == Projection::Orthographic) {
        float halfHeight = orthoHeight * 0.5f;
        return orthographic(halfHeight * aspect, halfHeight, zNear, zFar);
    }
    return perspective(fovY, aspect, zNear, zFar);
}

Camera Camera::isometric(Vec3 center, float heightUnits, float distance, float yawDegrees) {
    // Classic isometric: look down at atan(1/sqrt(2)) ~= 35.264 degrees, so
    // the three visible faces of a cube project to equal areas.
    constexpr float kIsoPitch = 0.61547971f;

    float yaw = radians(yawDegrees);
    Vec3 offset{
        std::cos(kIsoPitch) * std::sin(yaw),
        std::sin(kIsoPitch),
        std::cos(kIsoPitch) * std::cos(yaw),
    };

    Camera camera;
    camera.projection = Projection::Orthographic;
    camera.orthoHeight = heightUnits;
    camera.lookAt(center + offset * distance, center);
    return camera;
}

} // namespace blocky
