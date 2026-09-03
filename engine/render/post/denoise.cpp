#include "engine/render/post/denoise.hpp"

#include <cmath>
#include <vector>

namespace blocky {
namespace {

// The B3-spline kernel the a-trous scheme is built on.
constexpr float kKernel[5] = {1.0f / 16.0f, 1.0f / 4.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};

float luminance(Vec3 c) { return dot(c, Vec3{0.2126f, 0.7152f, 0.0722f}); }

} // namespace

Image denoise(const RenderTargets& targets, const DenoiseSettings& settings) {
    if (!targets.valid() || targets.color.empty()) return targets.color;

    const int width = targets.width;
    const int height = targets.height;
    const size_t count = size_t(width) * size_t(height);

    // ---- demodulate: keep the lighting, set the texture aside
    std::vector<Vec3> current(count);
    std::vector<Vec3> next(count);

    auto safeAlbedo = [&](size_t i) {
        Vec3 a = targets.albedo.data()[i];
        return maxv(a, Vec3{0.02f, 0.02f, 0.02f});
    };

    for (size_t i = 0; i < count; ++i) {
        current[i] = targets.color.data()[i] / safeAlbedo(i);
    }

    // ---- a-trous passes, each one twice as wide as the last
    for (int iteration = 0; iteration < settings.iterations; ++iteration) {
        const int step = 1 << iteration;

        // Colour tolerance loosens as the filter widens, which is what stops
        // the later passes from re-introducing blotches.
        const float colorSigma = settings.colorSigma * float(step);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t centre = size_t(y) * size_t(width) + size_t(x);

                const float centreDepth = targets.depth[centre];
                if (centreDepth < 0.0f) {
                    // Background: nothing to filter against, leave it alone.
                    next[centre] = current[centre];
                    continue;
                }

                const Vec3 centreColor = current[centre];
                const Vec3 centreNormal = targets.normal.data()[centre];
                const Vec3 centreAlbedo = targets.albedo.data()[centre];

                // Depth gradient, so a sloped surface is not mistaken for an
                // edge. Without this the tolerance has to be widened for
                // slopes, and a widened tolerance blurs real steps away.
                // Clamped, because a central difference taken across an actual
                // edge reports a nonsensical slope.
                auto depthAt = [&](int px, int py) {
                    px = std::min(std::max(px, 0), width - 1);
                    py = std::min(std::max(py, 0), height - 1);
                    float d = targets.depth[size_t(py) * size_t(width) + size_t(px)];
                    return d < 0.0f ? centreDepth : d;
                };
                const float maxSlope = 3.0f;
                float dzdx = std::min(maxSlope, std::max(-maxSlope,
                                 0.5f * (depthAt(x + 1, y) - depthAt(x - 1, y))));
                float dzdy = std::min(maxSlope, std::max(-maxSlope,
                                 0.5f * (depthAt(x, y + 1) - depthAt(x, y - 1))));

                Vec3 sum{0.0f};
                float weightSum = 0.0f;

                for (int ky = 0; ky < 5; ++ky) {
                    int sy = y + (ky - 2) * step;
                    if (sy < 0 || sy >= height) continue;

                    for (int kx = 0; kx < 5; ++kx) {
                        int sx = x + (kx - 2) * step;
                        if (sx < 0 || sx >= width) continue;

                        const size_t tap = size_t(sy) * size_t(width) + size_t(sx);
                        const float tapDepth = targets.depth[tap];
                        if (tapDepth < 0.0f) continue;

                        // Normals: a dot product below one means the surface
                        // turned, so the tap belongs to a different face.
                        float normalCloseness = std::max(0.0f, dot(centreNormal, targets.normal.data()[tap]));
                        float normalWeight = std::pow(normalCloseness, 1.0f / std::max(settings.normalSigma, 1e-3f));

                        // Depth, compared against what the gradient predicts
                        // rather than against the centre. Deliberately *not*
                        // scaled by the filter width: an earlier version was,
                        // and the wide iterations blurred straight across a
                        // two-block terrace step.
                        float expected = centreDepth + dzdx * float(sx - x) + dzdy * float(sy - y);
                        float depthScale = settings.depthSigma * std::max(1.0f, centreDepth * 0.02f);
                        float depthDelta = std::fabs(tapDepth - expected) / std::max(depthScale, 1e-3f);
                        float depthWeight = std::exp(-depthDelta * depthDelta);

                        // Albedo, which separates two differently textured
                        // surfaces that happen to share a plane.
                        Vec3 albedoDelta = targets.albedo.data()[tap] - centreAlbedo;
                        float albedoWeight = std::exp(-lengthSq(albedoDelta) /
                                                      (settings.albedoSigma * settings.albedoSigma));

                        // And the lighting itself, so a genuine shadow edge
                        // survives even when geometry and texture do not move.
                        float colorDelta = std::fabs(luminance(current[tap]) - luminance(centreColor));
                        float colorWeight = std::exp(-colorDelta / std::max(colorSigma, 1e-3f));

                        float weight = kKernel[ky] * kKernel[kx] *
                                       normalWeight * depthWeight * albedoWeight * colorWeight;

                        sum += current[tap] * weight;
                        weightSum += weight;
                    }
                }

                next[centre] = weightSum > 1e-6f ? sum / weightSum : centreColor;
            }
        }
        current.swap(next);
    }

    // ---- remodulate and blend back towards the original
    Image out(width, height);
    float strength = saturate(settings.strength);

    for (size_t i = 0; i < count; ++i) {
        Vec3 filtered = current[i] * safeAlbedo(i);
        out.data()[i] = lerp(targets.color.data()[i], filtered, strength);
    }
    return out;
}

} // namespace blocky
