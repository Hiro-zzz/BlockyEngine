#include "engine/assets/blocks/texture_gen.hpp"

#include "engine/world/noise.hpp"

#include <algorithm>
#include <cmath>

namespace blocky::texgen {
namespace {

float wrapDistance(int a, int b, int size) {
    int d = std::abs(a - b);
    return float(std::min(d, size - d));
}

}  // namespace

Tile::Tile(int size, Rng& rng)
    : size_(std::max(1, size)),
      rng_(rng),
      noiseSeed_(rng.nextUint()),
      pixels_(size_t(size_) * size_t(size_), Vec3{}),
      alpha_(size_t(size_) * size_t(size_), 1.0f) {}

void Tile::fill(Vec3 colour) {
    for (Vec3& texel : pixels_) texel = colour;
}

void Tile::grain(Vec3 low, Vec3 high, float frequency, int octaves) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            float n = noise::fbm2(float(x) * frequency, float(y) * frequency, octaves, noiseSeed_);
            at(x, y) = lerp(low, high, saturate(n * 0.5f + 0.5f));
        }
    }
}

void Tile::speckle(Vec3 colour, float chance, float strength) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            if (rng_.nextFloat() >= chance) continue;
            at(x, y) = lerp(at(x, y), colour, saturate(strength * (0.5f + 0.5f * rng_.nextFloat())));
        }
    }
}

void Tile::pebbles(Vec3 low, Vec3 high, Vec3 gap, int count, float radius) {
    fill(gap);

    struct Seed { float x, y; Vec3 colour; float radius; };
    std::vector<Seed> seeds;
    seeds.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        seeds.push_back({rng_.nextFloat() * float(size_), rng_.nextFloat() * float(size_),
                         lerp(low, high, rng_.nextFloat()),
                         radius * (0.7f + 0.6f * rng_.nextFloat())});
    }

    // Nearest seed wins, and the tile wraps -- a texture that did not would
    // show a seam down every block edge, which on a wall of cobble is the
    // first thing the eye finds.
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            float best = 1e9f;
            const Seed* winner = nullptr;
            for (const Seed& seed : seeds) {
                float dx = wrapDistance(x, int(seed.x), size_);
                float dy = wrapDistance(y, int(seed.y), size_);
                float d = std::sqrt(dx * dx + dy * dy);
                if (d < best) { best = d; winner = &seed; }
            }
            if (winner && best < winner->radius) {
                // Darker towards the edge of a stone, so each reads as round.
                float edge = saturate(best / winner->radius);
                at(x, y) = lerp(winner->colour, gap, edge * edge * 0.8f);
            }
        }
    }
}

void Tile::courses(Vec3 mortar, int rows, int columns, bool stagger) {
    int rowHeight = std::max(1, size_ / std::max(1, rows));
    int columnWidth = std::max(1, size_ / std::max(1, columns));

    for (int y = 0; y < size_; ++y) {
        int row = y / rowHeight;
        bool onRowLine = (y % rowHeight) == 0;

        // Every other course shifts by half a brick, which is what makes a
        // wall look built rather than tiled.
        int shift = (stagger && (row & 1)) ? columnWidth / 2 : 0;

        for (int x = 0; x < size_; ++x) {
            bool onColumnLine = ((x + shift) % columnWidth) == 0;
            if (onRowLine || onColumnLine) at(x, y) = mortar;
        }
    }
}

void Tile::boards(Vec3 wood, Vec3 seam, int count, float grainStrength) {
    int width = std::max(1, size_ / std::max(1, count));

    for (int x = 0; x < size_; ++x) {
        int board = x / width;
        // Each board gets its own shade, so a wall of planks is not a stripe
        // pattern repeated.
        float shade = 0.86f + 0.28f * noise::hashToFloat(board, 0, 0, noiseSeed_);

        for (int y = 0; y < size_; ++y) {
            float g = noise::fbm2(float(x) * 0.35f, float(y) * 0.06f, 3, noiseSeed_ + 17u);
            Vec3 colour = wood * shade * (1.0f + grainStrength * g);
            if ((x % width) == 0) colour = seam;
            at(x, y) = colour;
        }
    }
}

void Tile::rings(Vec3 light, Vec3 dark, float spacing) {
    float centre = float(size_) * 0.5f;
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            float dx = float(x) + 0.5f - centre;
            float dy = float(y) + 0.5f - centre;
            float r = std::sqrt(dx * dx + dy * dy);
            // Wobble the radius so the rings are not perfect circles.
            r += noise::fbm2(float(x) * 0.2f, float(y) * 0.2f, 2, noiseSeed_) * 1.6f;
            float t = 0.5f + 0.5f * std::sin(r * spacing);
            at(x, y) = lerp(dark, light, t);
        }
    }
}

void Tile::streaks(Vec3 low, Vec3 high, float density) {
    for (int x = 0; x < size_; ++x) {
        float column = noise::hashToFloat(x, 0, 0, noiseSeed_);
        for (int y = 0; y < size_; ++y) {
            float n = noise::fbm2(float(x) * density, float(y) * 0.08f, 3, noiseSeed_ + 5u);
            at(x, y) = lerp(low, high, saturate(0.5f + 0.5f * n + (column - 0.5f) * 0.5f));
        }
    }
}

void Tile::fringe(Vec3 colour, int depth, int ragged) {
    for (int x = 0; x < size_; ++x) {
        int extra = int(noise::hashToFloat(x, 7, 0, noiseSeed_) * float(ragged + 1));
        int limit = depth + extra;
        for (int y = 0; y < size_; ++y) {
            if (y < limit) {
                at(x, y) = colour;
                alphaAt(x, y) = 1.0f;
            } else {
                alphaAt(x, y) = 0.0f;
            }
        }
    }
}

void Tile::holes(float chance) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            float n = noise::fbm2(float(x) * 0.45f, float(y) * 0.45f, 2, noiseSeed_ + 31u);
            if (n * 0.5f + 0.5f < chance) alphaAt(x, y) = 0.0f;
        }
    }
}

void Tile::panel(Vec3 face, Vec3 rim, int inset) {
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            bool onRim = x < inset || y < inset || x >= size_ - inset || y >= size_ - inset;
            at(x, y) = onRim ? rim : face;
        }
    }
}

void Tile::edgeShade(float strength) {
    if (strength <= 0.0f) return;

    // A one-texel rim at 16 a face, two at 32 and above: the cue has to stay
    // the same *fraction* of a block or it disappears at high resolution.
    int rim = std::max(1, size_ / 16);

    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            int distance = std::min(std::min(x, y), std::min(size_ - 1 - x, size_ - 1 - y));
            if (distance >= rim) continue;

            float t = 1.0f - float(distance) / float(rim);
            at(x, y) = at(x, y) * (1.0f - strength * t);
        }
    }
}

void Tile::clearAlpha(float value) {
    for (float& a : alpha_) a = value;
}

Texture Tile::toTexture() const {
    ImageU8 image(size_, size_);
    for (int y = 0; y < size_; ++y) {
        for (int x = 0; x < size_; ++x) {
            size_t i = size_t(y) * size_t(size_) + size_t(x);
            Vec3 encoded = linearToSrgb(minv(maxv(pixels_[i], Vec3{0.0f}), Vec3{1.0f}));
            image.set(x, y,
                      ImageU8::RGBA{uint8_t(encoded.x * 255.0f + 0.5f),
                                    uint8_t(encoded.y * 255.0f + 0.5f),
                                    uint8_t(encoded.z * 255.0f + 0.5f),
                                    uint8_t(saturate(alpha_[i]) * 255.0f + 0.5f)});
        }
    }

    Texture texture;
    texture.fromImage(image);
    return texture;
}

// ============================================================== the recipes
// `generatePalette` lived here and is gone: a recipe for what grass looks
// like is content, and it reached into `block::` for ids to bind to. The
// verbs above are the engine's half. Konstruct's set is `game::generateTextures`.


} // namespace blocky::texgen
