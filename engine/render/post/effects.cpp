#include "engine/render/post/effects.hpp"

#include "engine/core/random.hpp"

#include <vector>

namespace blocky {
namespace {

float luminance(Vec3 c) { return dot(c, Vec3{0.2126f, 0.7152f, 0.0722f}); }

// Halve an image with a 2x2 box, which is all the pyramid needs -- the tent
// filter on the way back up does the smoothing.
Image downsample(const Image& source) {
    int width = std::max(1, source.width() / 2);
    int height = std::max(1, source.height() / 2);
    Image out(width, height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sx = std::min(x * 2, source.width() - 1);
            int sy = std::min(y * 2, source.height() - 1);
            int sx1 = std::min(sx + 1, source.width() - 1);
            int sy1 = std::min(sy + 1, source.height() - 1);

            out.at(x, y) = (source.at(sx, sy) + source.at(sx1, sy) +
                            source.at(sx, sy1) + source.at(sx1, sy1)) * 0.25f;
        }
    }
    return out;
}

// Bilinear upsample, accumulating into an existing image.
void upsampleAdd(const Image& source, Image& target, float weight) {
    float scaleX = float(source.width()) / float(target.width());
    float scaleY = float(source.height()) / float(target.height());

    for (int y = 0; y < target.height(); ++y) {
        float fy = (float(y) + 0.5f) * scaleY - 0.5f;
        int y0 = int(std::floor(fy));
        float ty = fy - float(y0);
        int y1 = std::min(std::max(y0 + 1, 0), source.height() - 1);
        y0 = std::min(std::max(y0, 0), source.height() - 1);

        for (int x = 0; x < target.width(); ++x) {
            float fx = (float(x) + 0.5f) * scaleX - 0.5f;
            int x0 = int(std::floor(fx));
            float tx = fx - float(x0);
            int x1 = std::min(std::max(x0 + 1, 0), source.width() - 1);
            x0 = std::min(std::max(x0, 0), source.width() - 1);

            Vec3 top = lerp(source.at(x0, y0), source.at(x1, y0), tx);
            Vec3 bottom = lerp(source.at(x0, y1), source.at(x1, y1), tx);
            target.at(x, y) += lerp(top, bottom, ty) * weight;
        }
    }
}

} // namespace

void applyBloom(Image& image, const BloomSettings& settings) {
    if (image.empty() || settings.intensity <= 0.0f || settings.levels < 1) return;

    // ---- bright pass, with a soft knee so the threshold is not a hard edge
    Image bright(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            Vec3 c = image.at(x, y);
            float l = luminance(c);
            float knee = std::max(settings.knee, 1e-4f);
            float soft = saturate((l - settings.threshold + knee) / (2.0f * knee));
            float weight = l > settings.threshold ? 1.0f : soft * soft;
            bright.at(x, y) = c * weight;
        }
    }

    // ---- down the pyramid
    std::vector<Image> levels;
    levels.push_back(std::move(bright));
    for (int i = 1; i < settings.levels; ++i) {
        const Image& previous = levels.back();
        if (previous.width() <= 2 || previous.height() <= 2) break;
        levels.push_back(downsample(previous));
    }

    // ---- and back up, each level folded into the one above it
    for (size_t i = levels.size() - 1; i > 0; --i) {
        upsampleAdd(levels[i], levels[i - 1], 1.0f);
    }

    // Normalise by the number of levels so `intensity` means the same thing
    // regardless of how deep the pyramid went.
    float scale = settings.intensity / float(levels.size());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.at(x, y) += levels[0].at(x, y) * scale;
        }
    }
}

void applyGrade(Image& image, const GradeSettings& settings) {
    if (image.empty()) return;

    // A crude but well-behaved white balance: push red up and blue down for
    // warmth, the other way for cool.
    Vec3 balance{1.0f + settings.temperature * 0.18f,
                 1.0f + settings.temperature * 0.03f,
                 1.0f - settings.temperature * 0.18f};

    constexpr float kMidGrey = 0.18f;

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            Vec3 c = maxv(image.at(x, y) * settings.exposure, Vec3{0.0f});

            c = c * balance;
            c = c * settings.gain + settings.lift;

            if (settings.contrast != 1.0f) {
                // Pivot around mid grey in log space, so contrast does not
                // simply brighten everything.
                Vec3 safe = maxv(c, Vec3{1e-5f});
                c = Vec3{kMidGrey * std::pow(safe.x / kMidGrey, settings.contrast),
                         kMidGrey * std::pow(safe.y / kMidGrey, settings.contrast),
                         kMidGrey * std::pow(safe.z / kMidGrey, settings.contrast)};
            }

            if (settings.saturation != 1.0f) {
                float l = luminance(c);
                c = lerp(Vec3{l}, c, settings.saturation);
            }

            image.at(x, y) = maxv(c, Vec3{0.0f});
        }
    }
}

void applyVignette(Image& image, const VignetteSettings& settings) {
    if (image.empty() || settings.amount <= 0.0f) return;

    float halfWidth = float(image.width()) * 0.5f;
    float halfHeight = float(image.height()) * 0.5f;

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            // Distance from centre, normalised so the corners sit at 1.
            float dx = (float(x) + 0.5f - halfWidth) / halfWidth;
            float dy = (float(y) + 0.5f - halfHeight) / halfHeight;
            float distance = std::sqrt(dx * dx + dy * dy) / 1.41421356f;

            float t = saturate((distance - settings.radius) /
                               std::max(settings.softness, 1e-3f));
            float falloff = 1.0f - settings.amount * t * t;
            image.at(x, y) *= falloff;
        }
    }
}

void applyGrain(Image& image, float amount, uint64_t seed) {
    if (image.empty() || amount <= 0.0f) return;

    for (int y = 0; y < image.height(); ++y) {
        Rng rng(seed, uint64_t(y) + 1);
        for (int x = 0; x < image.width(); ++x) {
            // Scale the noise with the pixel, so grain is not louder in the
            // shadows than in the highlights.
            float n = (rng.nextFloat() - 0.5f) * 2.0f * amount;
            image.at(x, y) *= (1.0f + n);
        }
    }
}

} // namespace blocky
