#include "engine/sprite/scatter.hpp"

#include "engine/core/random.hpp"
#include "engine/world/shapes.hpp"

#include <algorithm>

namespace blocky {
namespace scatter {
namespace {

// Fills in everything that does not depend on where the particle ended up.
Sprite makeSprite(Vec3 position, Rng& rng, const ParticleStyle& style) {
    Sprite sprite;
    sprite.position = position;

    float jitter = 1.0f;
    if (style.sizeJitter > 0.0f) {
        jitter = 1.0f + (rng.nextFloat() * 2.0f - 1.0f) * style.sizeJitter;
        jitter = std::max(0.05f, jitter);
    }
    sprite.size = {style.size.x * jitter, style.size.y * jitter};

    if (style.randomYaw) sprite.yawDegrees = rng.nextFloat() * 360.0f;
    if (style.randomRoll) sprite.rollDegrees = rng.nextFloat() * 360.0f;

    sprite.texture = style.texture;
    sprite.tint = style.tint;
    sprite.emission = style.emission;
    sprite.alphaCutoff = style.alphaCutoff;
    sprite.doubleSided = style.doubleSided;
    return sprite;
}

}  // namespace

std::vector<Sprite> inBox(Vec3 lo, Vec3 hi, int count, uint32_t seed,
                          const ParticleStyle& style) {
    std::vector<Sprite> sprites;
    if (count <= 0) return sprites;
    sprites.reserve(size_t(count));

    Rng rng(seed);
    for (int i = 0; i < count; ++i) {
        Vec3 position{lerp(lo.x, hi.x, rng.nextFloat()),
                      lerp(lo.y, hi.y, rng.nextFloat()),
                      lerp(lo.z, hi.z, rng.nextFloat())};
        sprites.push_back(makeSprite(position, rng, style));
    }
    return sprites;
}

std::vector<Sprite> inSphere(Vec3 centre, float radius, int count, uint32_t seed,
                             const ParticleStyle& style) {
    std::vector<Sprite> sprites;
    if (count <= 0 || radius <= 0.0f) return sprites;
    sprites.reserve(size_t(count));

    Rng rng(seed);
    for (int i = 0; i < count; ++i) {
        // Rejection beats the cube root here: three uniforms and a length
        // test, versus a transcendental, and it is exact either way.
        Vec3 offset;
        do {
            offset = Vec3{rng.nextFloat() * 2.0f - 1.0f, rng.nextFloat() * 2.0f - 1.0f,
                          rng.nextFloat() * 2.0f - 1.0f};
        } while (lengthSq(offset) > 1.0f);

        sprites.push_back(makeSprite(centre + offset * radius, rng, style));
    }
    return sprites;
}

std::vector<Sprite> inBeam(Vec3 from, Vec3 direction, float length, float radius, int count,
                           uint32_t seed, const ParticleStyle& style) {
    std::vector<Sprite> sprites;
    if (count <= 0) return sprites;
    sprites.reserve(size_t(count));

    Vec3 axis = normalize(direction);
    if (lengthSq(axis) < 0.5f) return sprites;

    Vec3 tangent, bitangent;
    orthonormalBasis(axis, tangent, bitangent);

    Rng rng(seed);
    for (int i = 0; i < count; ++i) {
        // Square root on the radius keeps the density even across the disc
        // instead of crowding the axis.
        float r = radius * std::sqrt(rng.nextFloat());
        float phi = kTwoPi * rng.nextFloat();
        float along = length * rng.nextFloat();

        Vec3 position = from + axis * along + tangent * (r * std::cos(phi)) +
                        bitangent * (r * std::sin(phi));
        sprites.push_back(makeSprite(position, rng, style));
    }
    return sprites;
}

std::vector<Sprite> onSurface(const World& world, IVec3 lo, IVec3 hi, int topY, int count,
                              uint32_t seed, float heightAbove, const ParticleStyle& style) {
    std::vector<Sprite> sprites;
    if (count <= 0) return sprites;
    sprites.reserve(size_t(count));

    Rng rng(seed);
    for (int i = 0; i < count; ++i) {
        float fx = lerp(float(lo.x), float(hi.x), rng.nextFloat());
        float fz = lerp(float(lo.z), float(hi.z), rng.nextFloat());

        shape::SurfacePoint surface =
            shape::findSurface(world, int(std::floor(fx)), int(std::floor(fz)), topY, lo.y);
        if (!surface.found) {
            // Keep the stream in step so a hole in the terrain does not
            // reshuffle every particle after it.
            makeSprite(Vec3{0.0f}, rng, style);
            continue;
        }

        Vec3 position{fx, float(surface.block.y) + 1.0f + heightAbove, fz};
        sprites.push_back(makeSprite(position, rng, style));
    }
    return sprites;
}

std::vector<Sprite> above(const World& world, IVec3 lo, IVec3 hi, BlockId id, float height,
                          int perBlock, uint32_t seed, const ParticleStyle& style) {
    std::vector<Sprite> sprites;
    if (perBlock <= 0) return sprites;

    Rng rng(seed);
    for (int z = lo.z; z <= hi.z; ++z) {
        for (int x = lo.x; x <= hi.x; ++x) {
            for (int y = lo.y; y <= hi.y; ++y) {
                if (world.get({x, y, z}) != id) continue;
                // Only a face open to the air throws anything off it.
                if (world.get({x, y + 1, z}) != block::Air) continue;

                for (int i = 0; i < perBlock; ++i) {
                    float u = rng.nextFloat();
                    // Squared keeps the crowd near the source, which is what
                    // a plume actually looks like.
                    float rise = height * u * u;
                    Vec3 position{float(x) + rng.nextFloat(), float(y) + 1.0f + rise,
                                  float(z) + rng.nextFloat()};
                    sprites.push_back(makeSprite(position, rng, style));
                }
            }
        }
    }
    return sprites;
}

void removeInsideSolid(std::vector<Sprite>& sprites, const World& world) {
    sprites.erase(std::remove_if(sprites.begin(), sprites.end(),
                                 [&](const Sprite& sprite) {
                                     return world.isOpaque(floorToInt(sprite.position));
                                 }),
                  sprites.end());
}

} // namespace scatter
} // namespace blocky
