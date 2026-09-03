#include "engine/render/post/stylize.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace blocky {
namespace {

float luminance(Vec3 c) { return dot(c, Vec3{0.2126f, 0.7152f, 0.0722f}); }

// Quantise x in [0, 1] onto `bands` steps, biased by gamma.
//
// The steps span the range end to end -- the lowest is exactly 0 and the
// highest exactly 1 -- rather than sitting at the centres of their intervals.
// Centres would preserve the frame's mean, which sounds like the better
// property and is not: it lifts a pixel receiving no light at all to the first
// band's centre, and a cel frame whose shadows are all the same dark grey has
// lost the contrast it exists for.
float quantise(float x, int bands, float gamma, float floorLevel) {
    if (bands <= 1) return x;
    float shaped = std::pow(saturate(x), gamma);
    // floor(1.0 * bands) is `bands`, one past the last interval, so a value at
    // the very top of the range would get a step of its own and four bands
    // would quietly render as five.
    float step = std::min(std::floor(shaped * float(bands)), float(bands - 1));
    float level = std::pow(step / float(bands - 1), 1.0f / gamma);
    return floorLevel + (1.0f - floorLevel) * level;
}

} // namespace

// ---------------------------------------------------------------------------
Image celShade(const RenderTargets& targets, const CelSettings& settings) {
    if (!targets.valid() || targets.color.empty()) return targets.color;

    const int width = targets.width;
    const int height = targets.height;
    Image out = targets.color;

    // ---- band the illumination, not the colour
    if (settings.bands > 0) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t i = size_t(y) * size_t(width) + size_t(x);
                if (targets.depth[i] < 0.0f) continue;  // background: leave it flat

                Vec3 albedo = maxv(targets.albedo.data()[i], Vec3{0.02f, 0.02f, 0.02f});
                Vec3 illumination = targets.color.data()[i] / albedo;

                float level = luminance(illumination);
                if (level <= 1e-5f) continue;

                // Above the ceiling the pixel is a light source or a specular
                // glint, not shading. Banding those flattens the one part of
                // the frame that is supposed to be smooth and bright.
                if (settings.highlightCeiling > 0.0f && level > settings.highlightCeiling) continue;

                // Only the *intensity* is quantised; the ratio between the
                // channels is kept, so a wall lit by warm light stays warm
                // inside its band instead of stepping towards grey.
                const float range = std::max(1e-4f, settings.range);
                float banded =
                    quantise(level / range, settings.bands, settings.bandGamma,
                             saturate(settings.shadowFloor)) *
                    range;
                Vec3 shaped = illumination * (banded / level);

                if (settings.saturation != 1.0f) {
                    float grey = luminance(shaped);
                    shaped = Vec3{grey} + (shaped - Vec3{grey}) * settings.saturation;
                }

                out.at(x, y) = shaped * albedo;
            }
        }
    }

    // ---- ink the geometric edges
    if (settings.outlineWidth <= 0 || settings.outlineOpacity <= 0.0f) return out;

    std::vector<uint8_t> edge(size_t(width) * size_t(height), 0);

    auto depthAt = [&](int px, int py) {
        px = std::min(std::max(px, 0), width - 1);
        py = std::min(std::max(py, 0), height - 1);
        return targets.depth[size_t(py) * size_t(width) + size_t(px)];
    };
    auto normalAt = [&](int px, int py) {
        px = std::min(std::max(px, 0), width - 1);
        py = std::min(std::max(py, 0), height - 1);
        return targets.normal.data()[size_t(py) * size_t(width) + size_t(px)];
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = size_t(y) * size_t(width) + size_t(x);
            const float centreDepth = targets.depth[i];

            // A silhouette against the sky is an edge on the solid side only,
            // so the outline hugs the object instead of haloing it.
            if (centreDepth < 0.0f) continue;

            const Vec3 centreNormal = targets.normal.data()[i];

            // Predict each neighbour's depth from the local slope before
            // comparing. Without this a floor running away from the camera
            // outlines itself in stripes: its depth changes by more than the
            // threshold between one pixel and the next, and nothing about
            // that is an edge.
            auto slope = [&](int dx, int dy) {
                float lo = depthAt(x - dx, y - dy);
                float hi = depthAt(x + dx, y + dy);
                if (lo < 0.0f || hi < 0.0f) return 0.0f;
                return std::min(3.0f, std::max(-3.0f, 0.5f * (hi - lo)));
            };
            const float dzdx = slope(1, 0);
            const float dzdy = slope(0, 1);

            bool isEdge = false;
            for (int k = 0; k < 4 && !isEdge; ++k) {
                const int dx[4] = {1, -1, 0, 0};
                const int dy[4] = {0, 0, 1, -1};

                float neighbourDepth = depthAt(x + dx[k], y + dy[k]);
                if (neighbourDepth < 0.0f) {
                    isEdge = true;  // against the background
                    break;
                }
                float predicted = centreDepth + dzdx * float(dx[k]) + dzdy * float(dy[k]);
                if (std::fabs(neighbourDepth - predicted) > settings.depthThreshold) isEdge = true;

                if (1.0f - dot(centreNormal, normalAt(x + dx[k], y + dy[k])) >
                    settings.normalThreshold) {
                    isEdge = true;
                }
            }
            if (isEdge) edge[i] = 1;
        }
    }

    // Widening is a separate pass over the marked pixels: doing it inside the
    // detection loop would let a freshly widened pixel seed further growth and
    // the line would keep thickening across the frame.
    if (settings.outlineWidth > 1) {
        std::vector<uint8_t> grown = edge;
        const int radius = settings.outlineWidth - 1;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (!edge[size_t(y) * size_t(width) + size_t(x)]) continue;
                for (int oy = -radius; oy <= radius; ++oy) {
                    for (int ox = -radius; ox <= radius; ++ox) {
                        int px = std::min(std::max(x + ox, 0), width - 1);
                        int py = std::min(std::max(y + oy, 0), height - 1);
                        grown[size_t(py) * size_t(width) + size_t(px)] = 1;
                    }
                }
            }
        }
        edge.swap(grown);
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!edge[size_t(y) * size_t(width) + size_t(x)]) continue;
            out.at(x, y) = lerp(out.at(x, y), settings.outlineColor, settings.outlineOpacity);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
void pixelate(Image& image, const PixelateSettings& settings) {
    if (image.empty()) return;
    if (settings.factor <= 1 && settings.levels <= 0) return;

    const int width = image.width();
    const int height = image.height();
    const int factor = std::max(1, settings.factor);

    for (int by = 0; by < height; by += factor) {
        for (int bx = 0; bx < width; bx += factor) {
            const int x1 = std::min(bx + factor, width);
            const int y1 = std::min(by + factor, height);

            Vec3 sum{0.0f};
            int n = 0;
            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) {
                    sum += image.at(x, y);
                    ++n;
                }
            }
            Vec3 mean = sum / float(std::max(n, 1));

            if (settings.levels > 0) {
                // Step in a perceptual space and come back. Quantising linear
                // radiance spends almost every level on highlights nobody can
                // tell apart and leaves the shadows in two or three steps.
                const float steps = float(settings.levels);
                Vec3 encoded = linearToSrgb(mean);
                Vec3 stepped{std::round(saturate(encoded.x) * steps) / steps,
                             std::round(saturate(encoded.y) * steps) / steps,
                             std::round(saturate(encoded.z) * steps) / steps};
                mean = srgbToLinear(stepped);
            }

            for (int y = by; y < y1; ++y) {
                for (int x = bx; x < x1; ++x) image.at(x, y) = mean;
            }
        }
    }
}

} // namespace blocky
