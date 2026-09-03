#include "engine/sprite/sprite.hpp"

namespace blocky {

void orientationTowards(Vec3 direction, float& yawDegrees, float& pitchDegrees) {
    Vec3 n = normalize(direction);
    if (lengthSq(n) < 0.5f) { yawDegrees = 0.0f; pitchDegrees = 0.0f; return; }

    // The quad normal is Ry(yaw) * Rx(pitch) * (0,0,1), which works out to
    // (sin y * cos p, -sin p, cos y * cos p). Inverting that is direct.
    pitchDegrees = degrees(std::asin(std::max(-1.0f, std::min(1.0f, -n.y))));
    yawDegrees = degrees(std::atan2(n.x, n.z));
}

void aimAt(Sprite& sprite, Vec3 target) {
    orientationTowards(target - sprite.position, sprite.yawDegrees, sprite.pitchDegrees);
}

void aimAt(std::vector<Sprite>& sprites, Vec3 target) {
    for (Sprite& sprite : sprites) aimAt(sprite, target);
}

} // namespace blocky
