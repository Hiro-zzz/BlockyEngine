#pragma once
// The one place that decides what colour a block face is, so the direct
// renderer and the path tracer can never disagree about it.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/scene/scene.hpp"
#include "engine/world/raycast.hpp"

namespace blocky {

// Linear-light albedo at a hit point. Uses the block texture library when the
// scene has one, and falls back to the flat palette colour otherwise -- so
// every scene renders with or without game assets present.
inline Vec3 surfaceAlbedo(const Scene& scene, const BlockDef& def, const RayHit& hit) {
    Vec3 fallback = (def.tintTop && hit.normal.y > 0) ? def.topTint : def.albedo;

    if (!scene.blockTextures) return fallback;
    return scene.blockTextures->sampleAlbedo(hit.id, blockFaceIndex(hit.normal), hit.uv, fallback);
}

} // namespace blocky
