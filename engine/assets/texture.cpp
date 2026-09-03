#include "engine/assets/texture.hpp"

#include "engine/core/png.hpp"

namespace blocky {

bool Texture::loadFromPng(const uint8_t* bytes, size_t size, std::string* error) {
    ImageU8 image;
    if (!pngDecode(bytes, size, image, error)) return false;
    fromImage(image);
    return true;
}

void Texture::fromImage(const ImageU8& image) {
    width_ = image.width();
    height_ = image.height();
    pixels_.assign(size_t(width_) * size_t(height_), Vec3{0.0f});
    alpha_.assign(size_t(width_) * size_t(height_), 0.0f);

    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            ImageU8::RGBA c = image.get(x, y);
            size_t i = size_t(y) * size_t(width_) + size_t(x);
            // Texture art is authored in sRGB; lighting maths needs linear.
            pixels_[i] = srgbToLinear(Vec3{float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f});
            alpha_[i] = float(c.a) / 255.0f;
        }
    }
    recomputeSummary();
}

void Texture::recomputeSummary() {
    hasTransparency_ = false;
    hasPartialAlpha_ = false;

    Vec3 sum{0.0f};
    float weight = 0.0f;
    for (size_t i = 0; i < alpha_.size(); ++i) {
        float a = alpha_[i];
        if (a < 0.999f) hasTransparency_ = true;
        if (a > 0.001f && a < 0.999f) hasPartialAlpha_ = true;
        sum += pixels_[i] * a;
        weight += a;
    }
    average_ = weight > 0.0f ? sum / weight : Vec3{0.0f};
}

void Texture::cropToFirstSquareFrame() {
    if (empty() || height_ <= width_) return;
    if (height_ % width_ != 0) return;  // not a frame strip; leave it alone

    pixels_.resize(size_t(width_) * size_t(width_));
    alpha_.resize(size_t(width_) * size_t(width_));
    height_ = width_;
    recomputeSummary();
}

Texture Texture::subRegion(int x, int y, int width, int height) const {
    Texture out;
    if (width <= 0 || height <= 0 || empty()) return out;

    out.width_ = width;
    out.height_ = height;
    out.pixels_.resize(size_t(width) * size_t(height));
    out.alpha_.resize(size_t(width) * size_t(height));

    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            size_t src = index(x + i, y + j);
            size_t dst = size_t(j) * size_t(width) + size_t(i);
            out.pixels_[dst] = pixels_[src];
            out.alpha_[dst] = alpha_[src];
        }
    }
    out.recomputeSummary();
    return out;
}

Vec3 Texture::sample(Vec2 uv) const {
    if (empty()) return average_;
    int x = int(uv.x * float(width_));
    int y = int(uv.y * float(height_));
    return pixels_[index(x, y)];
}

float Texture::sampleAlpha(Vec2 uv) const {
    if (empty()) return 1.0f;
    int x = int(uv.x * float(width_));
    int y = int(uv.y * float(height_));
    return alpha_[index(x, y)];
}

} // namespace blocky
