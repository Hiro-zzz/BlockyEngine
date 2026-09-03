#include "engine/world/noise.hpp"

namespace blocky {
namespace noise {
namespace {

uint32_t hashInt(uint32_t x) {
    // Murmur3 finaliser: cheap and mixes single-bit changes thoroughly.
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

uint32_t hashCoords(int32_t x, int32_t y, uint32_t seed) {
    return hashInt(uint32_t(x) * 0x27d4eb2du ^ uint32_t(y) * 0x165667b1u ^ seed);
}

uint32_t hashCoords(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return hashInt(uint32_t(x) * 0x27d4eb2du ^ uint32_t(y) * 0x165667b1u ^
                   uint32_t(z) * 0x9e3779b9u ^ seed);
}

// Quintic fade: zero first and second derivatives at the ends, so octaves
// stack without visible lattice creases.
float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Gradient dot product for 2D: one of eight directions, picked by hash.
float grad2(uint32_t h, float x, float y) {
    switch (h & 7u) {
        case 0: return  x + y;
        case 1: return  x - y;
        case 2: return -x + y;
        case 3: return -x - y;
        case 4: return  x;
        case 5: return -x;
        case 6: return  y;
        default: return -y;
    }
}

// Gradient dot product for 3D: the twelve edge-midpoint directions of a cube.
float grad3(uint32_t h, float x, float y, float z) {
    switch (h & 15u) {
        case 0:  return  x + y;
        case 1:  return -x + y;
        case 2:  return  x - y;
        case 3:  return -x - y;
        case 4:  return  x + z;
        case 5:  return -x + z;
        case 6:  return  x - z;
        case 7:  return -x - z;
        case 8:  return  y + z;
        case 9:  return -y + z;
        case 10: return  y - z;
        case 11: return -y - z;
        case 12: return  x + y;
        case 13: return -y + z;
        case 14: return -x + y;
        default: return -y - z;
    }
}

} // namespace

float perlin2(float x, float y, uint32_t seed) {
    int32_t xi = int32_t(std::floor(x));
    int32_t yi = int32_t(std::floor(y));
    float xf = x - float(xi);
    float yf = y - float(yi);

    float u = fade(xf);
    float v = fade(yf);

    float n00 = grad2(hashCoords(xi,     yi,     seed), xf,        yf);
    float n10 = grad2(hashCoords(xi + 1, yi,     seed), xf - 1.0f, yf);
    float n01 = grad2(hashCoords(xi,     yi + 1, seed), xf,        yf - 1.0f);
    float n11 = grad2(hashCoords(xi + 1, yi + 1, seed), xf - 1.0f, yf - 1.0f);

    float a = lerp(n00, n10, u);
    float b = lerp(n01, n11, u);
    // The 2D gradient magnitude peaks at sqrt(2); scale back into [-1, 1].
    return lerp(a, b, v) * 0.70710678f;
}

float perlin3(Vec3 p, uint32_t seed) {
    int32_t xi = int32_t(std::floor(p.x));
    int32_t yi = int32_t(std::floor(p.y));
    int32_t zi = int32_t(std::floor(p.z));
    float xf = p.x - float(xi);
    float yf = p.y - float(yi);
    float zf = p.z - float(zi);

    float u = fade(xf), v = fade(yf), w = fade(zf);

    auto corner = [&](int dx, int dy, int dz) {
        return grad3(hashCoords(xi + dx, yi + dy, zi + dz, seed),
                     xf - float(dx), yf - float(dy), zf - float(dz));
    };

    float x00 = lerp(corner(0, 0, 0), corner(1, 0, 0), u);
    float x10 = lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    float x01 = lerp(corner(0, 0, 1), corner(1, 0, 1), u);
    float x11 = lerp(corner(0, 1, 1), corner(1, 1, 1), u);

    float y0 = lerp(x00, x10, v);
    float y1 = lerp(x01, x11, v);
    return lerp(y0, y1, w) * 0.70710678f;
}

float fbm2(float x, float y, int octaves, uint32_t seed, float lacunarity, float gain) {
    float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += perlin2(x * frequency, y * frequency, seed + uint32_t(i) * 1013u) * amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float fbm3(Vec3 p, int octaves, uint32_t seed, float lacunarity, float gain) {
    float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += perlin3(p * frequency, seed + uint32_t(i) * 1013u) * amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float ridged2(float x, float y, int octaves, uint32_t seed, float lacunarity, float gain) {
    float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.0f - std::fabs(perlin2(x * frequency, y * frequency, seed + uint32_t(i) * 1013u));
        sum += n * n * amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return total > 0.0f ? saturate(sum / total) : 0.0f;
}

float hashToFloat(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return float(hashCoords(x, y, z, seed) >> 8) * 0x1.0p-24f;
}

} // namespace noise
} // namespace blocky
