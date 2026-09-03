#pragma once
// PCG32. Small, fast, and each render thread owns one seeded from its tile
// index, so a render is reproducible no matter how the work is distributed.
#include "engine/core/math.hpp"

#include <cstdint>

namespace blocky {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x853c49e6748fea9bULL, uint64_t sequence = 1) {
        state_ = 0;
        inc_ = (sequence << 1u) | 1u;
        nextUint();
        state_ += seed;
        nextUint();
    }

    uint32_t nextUint() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    // Uniform in [0, 1).
    float nextFloat() { return float(nextUint() >> 8) * 0x1.0p-24f; }

    Vec2 nextVec2() { float a = nextFloat(); float b = nextFloat(); return {a, b}; }

    // Cosine-weighted direction in the hemisphere around unit normal `n`.
    // This is the importance-sampled distribution a diffuse surface wants.
    Vec3 cosineHemisphere(Vec3 n) {
        float u1 = nextFloat(), u2 = nextFloat();
        float r = std::sqrt(u1);
        float phi = kTwoPi * u2;
        float x = r * std::cos(phi);
        float y = r * std::sin(phi);
        float z = std::sqrt(std::max(0.0f, 1.0f - u1));

        Vec3 t, b;
        orthonormalBasis(n, t, b);
        return normalize(t * x + b * y + n * z);
    }

    // Uniform direction inside a cone of half-angle `cosMax`, around `axis`.
    // Used to give the sun an angular size, and therefore soft shadows.
    Vec3 uniformCone(Vec3 axis, float cosThetaMax) {
        float u1 = nextFloat(), u2 = nextFloat();
        float cosTheta = 1.0f - u1 * (1.0f - cosThetaMax);
        float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        float phi = kTwoPi * u2;

        Vec3 t, b;
        orthonormalBasis(axis, t, b);
        return normalize(t * (sinTheta * std::cos(phi)) + b * (sinTheta * std::sin(phi)) + axis * cosTheta);
    }

private:
    uint64_t state_ = 0;
    uint64_t inc_ = 0;
};

} // namespace blocky
