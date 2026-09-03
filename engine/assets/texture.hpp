#pragma once
// A decoded texture, held in linear light and sampled with nearest-neighbour
// filtering.
//
// Nearest is not a shortcut here, it is the correct choice: Minecraft art is
// pixel art, and bilinear filtering would smear every 16x16 texel into mush.
// This is the one piece shared by the block and the entity asset paths --
// everything above it is separate.
#include "engine/core/image.hpp"
#include "engine/core/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

class Texture {
public:
    bool loadFromPng(const uint8_t* bytes, size_t size, std::string* error = nullptr);
    void fromImage(const ImageU8& image);

    // Minecraft stores animated textures as a vertical strip of square frames
    // with a companion .mcmeta file. Without an animation system we take the
    // first frame, which is what a still render wants anyway.
    void cropToFirstSquareFrame();

    // Cut out a sub-rectangle. Entity skins are one image holding dozens of
    // separate faces, so this is how a body part gets its own texture.
    Texture subRegion(int x, int y, int width, int height) const;

    bool empty()  const { return pixels_.empty(); }
    int  width()  const { return width_; }
    int  height() const { return height_; }

    // Texel access, without any filtering or wrapping beyond a clamp.
    Vec3  texel(int x, int y) const { return pixels_[index(x, y)]; }
    float alphaAt(int x, int y) const { return alpha_[index(x, y)]; }

    // uv runs [0,1) across the texture, v downwards, matching image layout.
    Vec3  sample(Vec2 uv) const;
    float sampleAlpha(Vec2 uv) const;

    // Mean colour over the opaque texels. Used as the fallback albedo when a
    // texture is missing, and to keep distant blocks from aliasing.
    Vec3 averageColor() const { return average_; }

    // True if any texel is neither fully opaque nor fully transparent.
    bool hasPartialAlpha() const { return hasPartialAlpha_; }
    bool hasAnyTransparency() const { return hasTransparency_; }

private:
    size_t index(int x, int y) const {
        int cx = x < 0 ? 0 : (x >= width_ ? width_ - 1 : x);
        int cy = y < 0 ? 0 : (y >= height_ ? height_ - 1 : y);
        return size_t(cy) * size_t(width_) + size_t(cx);
    }
    void recomputeSummary();

    int width_ = 0, height_ = 0;
    std::vector<Vec3> pixels_;   // linear light
    std::vector<float> alpha_;
    Vec3 average_{1.0f, 0.0f, 1.0f};  // magenta, so a missing texture is obvious
    bool hasTransparency_ = false;
    bool hasPartialAlpha_ = false;
};

} // namespace blocky
