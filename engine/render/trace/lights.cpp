#include "engine/render/trace/lights.hpp"

#include <algorithm>

namespace blocky {
namespace {

float luminance(Vec3 c) { return dot(c, Vec3{0.2126f, 0.7152f, 0.0722f}); }

} // namespace

void LightSet::build(const World& world) {
    lights_.clear();
    cdf_.clear();
    totalPower_ = 0.0f;

    const BlockRegistry& registry = world.registry();

    world.forEachChunk([&](IVec3 chunkCoord, const World::Chunk& chunk) {
        IVec3 base = chunkCoord * World::kChunkSize;

        for (int ly = 0; ly < World::kChunkSize; ++ly) {
            for (int lz = 0; lz < World::kChunkSize; ++lz) {
                for (int lx = 0; lx < World::kChunkSize; ++lx) {
                    BlockId id = chunk.blocks[World::Chunk::index(lx, ly, lz)];
                    if (id == block::Air) continue;

                    const BlockDef& def = registry[id];
                    if (!def.emissive()) continue;

                    IVec3 position = base + IVec3{lx, ly, lz};

                    for (int axis = 0; axis < 3; ++axis) {
                        for (int sign = -1; sign <= 1; sign += 2) {
                            IVec3 normal{0, 0, 0};
                            normal[axis] = sign;

                            // A face buried against an opaque neighbour emits
                            // nowhere and would only add sampling noise.
                            if (world.isOpaque(position + normal)) continue;

                            int t1 = (axis + 1) % 3;
                            int t2 = (axis + 2) % 3;

                            AreaLight light;
                            light.origin = toVec3(position);
                            if (sign > 0) light.origin[axis] += 1.0f;

                            light.edgeU = Vec3{0.0f};
                            light.edgeU[t1] = 1.0f;
                            light.edgeV = Vec3{0.0f};
                            light.edgeV[t2] = 1.0f;

                            light.normal = toVec3(normal);
                            light.radiance = def.emission;
                            light.power = luminance(def.emission);  // unit area

                            if (light.power <= 0.0f) continue;
                            lights_.push_back(light);
                        }
                    }
                }
            }
        }
    });

    cdf_.reserve(lights_.size());
    for (const AreaLight& light : lights_) {
        totalPower_ += light.power;
        cdf_.push_back(totalPower_);
    }
}

bool LightSet::sample(Rng& rng, Vec3 point, Vec3& direction, float& distance,
                      Vec3& radiance, float& pdf) const {
    if (lights_.empty() || totalPower_ <= 0.0f) return false;

    // Choose a light proportionally to its power.
    float target = rng.nextFloat() * totalPower_;
    size_t index = size_t(std::lower_bound(cdf_.begin(), cdf_.end(), target) - cdf_.begin());
    if (index >= lights_.size()) index = lights_.size() - 1;

    const AreaLight& light = lights_[index];

    Vec3 onLight = light.origin + light.edgeU * rng.nextFloat() + light.edgeV * rng.nextFloat();
    Vec3 offset = onLight - point;

    float distanceSq = lengthSq(offset);
    if (distanceSq < 1e-8f) return false;

    distance = std::sqrt(distanceSq);
    direction = offset / distance;

    // The face only emits from its front.
    float cosLight = dot(light.normal, -direction);
    if (cosLight <= 1e-4f) return false;

    // Area density -> solid-angle density.
    float selectionPdf = light.power / totalPower_;
    pdf = selectionPdf * distanceSq / cosLight;  // unit area, so no division by area
    radiance = light.radiance;
    return pdf > 0.0f;
}

} // namespace blocky
