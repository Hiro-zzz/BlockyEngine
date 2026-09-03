#pragma once
// Two image types, deliberately kept separate:
//   Image   - linear-light float RGB, what the renderer accumulates into.
//   ImageU8 - 8-bit RGBA, what lives on disk and in texture atlases.
#include "engine/core/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

// --------------------------------------------------------------------- Image
// Linear-light HDR framebuffer. No gamma, no clamping: values above 1 are
// legitimate and are what makes emissive blocks and bloom work later.
class Image {
public:
    Image() = default;
    Image(int width, int height, Vec3 fill = Vec3{0.0f})
        : width_(width), height_(height), pixels_(size_t(width) * size_t(height), fill) {}

    int  width()  const { return width_; }
    int  height() const { return height_; }
    bool empty()  const { return pixels_.empty(); }

    Vec3&       at(int x, int y)       { return pixels_[size_t(y) * size_t(width_) + size_t(x)]; }
    const Vec3& at(int x, int y) const { return pixels_[size_t(y) * size_t(width_) + size_t(x)]; }

    Vec3*       data()       { return pixels_.data(); }
    const Vec3* data() const { return pixels_.data(); }

private:
    int width_ = 0, height_ = 0;
    std::vector<Vec3> pixels_;
};

// ------------------------------------------------------------------- ImageU8
// Tightly packed RGBA8. Alpha is meaningful: Minecraft textures use it for
// cutout foliage and for the transparent regions of a skin overlay.
class ImageU8 {
public:
    ImageU8() = default;
    ImageU8(int width, int height)
        : width_(width), height_(height), pixels_(size_t(width) * size_t(height) * 4, 0) {}

    int  width()  const { return width_; }
    int  height() const { return height_; }
    bool empty()  const { return pixels_.empty(); }

    struct RGBA { uint8_t r, g, b, a; };

    RGBA get(int x, int y) const {
        size_t i = (size_t(y) * size_t(width_) + size_t(x)) * 4;
        return {pixels_[i], pixels_[i + 1], pixels_[i + 2], pixels_[i + 3]};
    }
    void set(int x, int y, RGBA c) {
        size_t i = (size_t(y) * size_t(width_) + size_t(x)) * 4;
        pixels_[i] = c.r; pixels_[i + 1] = c.g; pixels_[i + 2] = c.b; pixels_[i + 3] = c.a;
    }

    uint8_t*       data()       { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    std::vector<uint8_t>&       storage()       { return pixels_; }
    const std::vector<uint8_t>& storage() const { return pixels_; }

    void resize(int width, int height) {
        width_ = width; height_ = height;
        pixels_.assign(size_t(width) * size_t(height) * 4, 0);
    }

private:
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> pixels_;
};

// ------------------------------------------------------------------ transfer
// sRGB transfer functions. Textures authored for Minecraft are sRGB-encoded,
// so they must be linearised before any lighting maths touches them.
float srgbToLinear(float c);
float linearToSrgb(float c);

inline Vec3 srgbToLinear(Vec3 c) { return {srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z)}; }
inline Vec3 linearToSrgb(Vec3 c) { return {linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z)}; }

// ------------------------------------------------------------------ tonemap
enum class Tonemap {
    None,       // straight clamp, for debugging raw values
    Reinhard,   // gentle, keeps the flat Minecraft palette readable
    ACES,       // filmic, punchier contrast and highlight rolloff
};

struct ToneParams {
    float   exposure = 1.0f;
    Tonemap curve    = Tonemap::ACES;
    // 1.0 = plain sRGB output. Raise slightly to lift shadows.
    float   gamma    = 1.0f;
};

// Linear HDR -> display-ready 8-bit sRGB.
ImageU8 tonemapToU8(const Image& hdr, const ToneParams& params = {});

} // namespace blocky
