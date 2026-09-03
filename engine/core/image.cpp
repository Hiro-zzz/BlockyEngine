#include "engine/core/image.hpp"

namespace blocky {

float srgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c) {
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

namespace {

Vec3 reinhard(Vec3 c) {
    // Luminance-preserving Reinhard: tone the brightness, keep the hue.
    float l = dot(c, Vec3{0.2126f, 0.7152f, 0.0722f});
    if (l <= kEps) return c;
    return c * ((l / (1.0f + l)) / l);
}

Vec3 acesFilmic(Vec3 c) {
    // Narkowicz 2015 fit of the ACES RRT+ODT curve.
    constexpr float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
    Vec3 num = c * (c * a + Vec3{b});
    Vec3 den = c * (c * cc + Vec3{d}) + Vec3{e};
    return {num.x / den.x, num.y / den.y, num.z / den.z};
}

uint8_t quantize(float linear, float gamma) {
    float s = linearToSrgb(saturate(linear));
    if (gamma != 1.0f) s = std::pow(s, 1.0f / gamma);
    return uint8_t(saturate(s) * 255.0f + 0.5f);
}

} // namespace

ImageU8 tonemapToU8(const Image& hdr, const ToneParams& params) {
    ImageU8 out(hdr.width(), hdr.height());
    for (int y = 0; y < hdr.height(); ++y) {
        for (int x = 0; x < hdr.width(); ++x) {
            Vec3 c = hdr.at(x, y) * params.exposure;
            switch (params.curve) {
                case Tonemap::None:     c = minv(maxv(c, Vec3{0.0f}), Vec3{1.0f}); break;
                case Tonemap::Reinhard: c = reinhard(maxv(c, Vec3{0.0f}));         break;
                case Tonemap::ACES:     c = acesFilmic(maxv(c, Vec3{0.0f}));       break;
            }
            out.set(x, y, {quantize(c.x, params.gamma),
                           quantize(c.y, params.gamma),
                           quantize(c.z, params.gamma),
                           255});
        }
    }
    return out;
}

} // namespace blocky
