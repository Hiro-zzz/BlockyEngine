#include "engine/scene/scene.hpp"

namespace blocky {

void Scene::frameAll(float heightMargin, float yawDegrees) {
    if (!world.hasBlocks()) return;

    Vec3 lo = toVec3(world.minBlock());
    Vec3 hi = toVec3(world.maxBlock()) + Vec3{1.0f};
    Vec3 center = (lo + hi) * 0.5f;
    Vec3 extent = hi - lo;

    // An isometric view sees roughly the diagonal of the footprint, so size
    // the frustum from that rather than from the tallest axis alone.
    float footprint = std::sqrt(extent.x * extent.x + extent.z * extent.z);
    float visible = std::max(extent.y + footprint * 0.5f, footprint * 0.62f);

    float distance = std::max(64.0f, maxComponent(extent) * 3.0f);
    camera = Camera::isometric(center, visible * heightMargin, distance, yawDegrees);
}

} // namespace blocky
