#pragma once
// Gradient (Perlin) noise with an integer hash instead of a permutation
// table, so any seed works without a setup step and the functions stay pure.
// All results are in [-1, 1] unless stated otherwise.
#include "engine/core/math.hpp"

#include <cstdint>

namespace blocky {
namespace noise {

float perlin2(float x, float y, uint32_t seed = 0);
float perlin3(Vec3 p, uint32_t seed = 0);

// Fractal sum of octaves. Amplitude is normalised, so the range stays [-1, 1].
float fbm2(float x, float y, int octaves, uint32_t seed = 0,
           float lacunarity = 2.0f, float gain = 0.5f);
float fbm3(Vec3 p, int octaves, uint32_t seed = 0,
           float lacunarity = 2.0f, float gain = 0.5f);

// Ridged multifractal: sharp crests, good for mountain silhouettes. [0, 1].
float ridged2(float x, float y, int octaves, uint32_t seed = 0,
              float lacunarity = 2.0f, float gain = 0.5f);

// Deterministic hash of a lattice point, uniform in [0, 1). Handy for
// scattering decorations without carrying an Rng around.
float hashToFloat(int32_t x, int32_t y, int32_t z, uint32_t seed = 0);

} // namespace noise
} // namespace blocky
