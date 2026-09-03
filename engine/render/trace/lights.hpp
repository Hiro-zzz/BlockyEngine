#pragma once
// Emissive geometry, gathered into a list the integrator can sample directly.
//
// A glowstone block buried in stone lights nothing, so only *exposed* faces
// become lights: one unit square per emissive face whose neighbour lets light
// through. Sampling these explicitly (next-event estimation) is the
// difference between a lava lake converging in seconds and never converging
// at all -- a diffuse bounce has almost no chance of hitting a small emitter
// by accident.
#include "engine/core/math.hpp"
#include "engine/core/random.hpp"
#include "engine/world/world.hpp"

#include <vector>

namespace blocky {

struct AreaLight {
    Vec3 origin;   // corner of the unit square with the smallest coordinates
    Vec3 edgeU;    // spans the face, length 1
    Vec3 edgeV;
    Vec3 normal;   // points away from the emitting block
    Vec3 radiance;
    float power = 0.0f;  // luminance * area, used to weight selection
};

class LightSet {
public:
    // Scan the world for exposed emissive faces.
    void build(const World& world);

    bool   empty() const { return lights_.empty(); }
    size_t size()  const { return lights_.size(); }
    float  totalPower() const { return totalPower_; }

    // Pick a point on one light, weighted by power.
    // `pdf` comes back as a solid-angle density measured from `point`.
    // Returns false when the sample is unusable (behind the light, degenerate).
    bool sample(Rng& rng, Vec3 point, Vec3& direction, float& distance,
                Vec3& radiance, float& pdf) const;

    // The gathered faces and the running power total behind them. Exposed so
    // the same list can be handed to a shader instead of being walked here --
    // the GPU integrator needs the identical lights and the identical
    // selection weights, and rebuilding them from the world a second time
    // would be two chances to disagree instead of one.
    const std::vector<AreaLight>& lights() const { return lights_; }
    const std::vector<float>& cdf() const { return cdf_; }

private:
    std::vector<AreaLight> lights_;
    std::vector<float> cdf_;  // cumulative power, last entry == totalPower_
    float totalPower_ = 0.0f;
};

} // namespace blocky
