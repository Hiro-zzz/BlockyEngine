#include "engine/sprite/text.hpp"

#include <algorithm>

namespace blocky {
namespace text {
namespace {

std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : content) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch == '\r') continue;
        current.push_back(ch);
    }
    lines.push_back(current);
    return lines;
}

struct Frame {
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};

Frame frameOf(const TextStyle& style) {
    Mat4 rotation = rotateAxis({0.0f, 1.0f, 0.0f}, radians(style.yawDegrees)) *
                    rotateAxis({1.0f, 0.0f, 0.0f}, radians(style.pitchDegrees));
    Frame frame;
    frame.right = transformDir(rotation, {1.0f, 0.0f, 0.0f});
    frame.up = transformDir(rotation, {0.0f, 1.0f, 0.0f});
    return frame;
}

float lineStartOffset(const TextStyle& style, float lineWidth) {
    switch (style.align) {
        case TextStyle::Align::Left:  return 0.0f;
        case TextStyle::Align::Right: return -lineWidth;
        default:                      return -lineWidth * 0.5f;
    }
}

}  // namespace

Vec2 measure(const Font& font, const std::string& content, const TextStyle& style) {
    if (!font.loaded()) return {0.0f, 0.0f};

    const float scale = style.height / float(font.cellPixels());
    std::vector<std::string> lines = splitLines(content);

    float widest = 0.0f;
    for (const std::string& line : lines) {
        widest = std::max(widest, font.measure(line) * scale);
    }

    const float step = style.height * style.lineSpacing;
    const float total = style.height + float(lines.size() - 1) * step;
    return {widest, total};
}

std::vector<Sprite> build(const Font& font, const std::string& content, Vec3 position,
                          const TextStyle& style) {
    std::vector<Sprite> sprites;
    if (!font.loaded()) return sprites;

    const float scale = style.height / float(font.cellPixels());
    const Frame frame = frameOf(style);

    std::vector<std::string> lines = splitLines(content);
    const float step = style.height * style.lineSpacing;
    const float totalHeight = style.height + float(lines.size() - 1) * step;

    // Vertical centring on the whole stack, so a two-line label straddles the
    // anchor the same way a one-line label does.
    float lineCentre = totalHeight * 0.5f - style.height * 0.5f;

    for (const std::string& line : lines) {
        float pen = lineStartOffset(style, font.measure(line) * scale);

        for (char ch : line) {
            Font::Glyph glyph = font.glyph(static_cast<unsigned char>(ch));

            // A space is pen movement, not geometry.
            if (glyph.blank) {
                pen += glyph.advance * scale;
                continue;
            }

            const float glyphWidth = glyph.width * scale;

            Sprite sprite;
            sprite.position = position + frame.right * (pen + glyphWidth * 0.5f) +
                              frame.up * lineCentre;
            sprite.size = {glyphWidth, style.height};
            sprite.yawDegrees = style.yawDegrees;
            sprite.pitchDegrees = style.pitchDegrees;
            sprite.texture = &font.texture();
            sprite.uvMin = glyph.uvMin;
            sprite.uvMax = glyph.uvMax;
            sprite.tint = style.color;
            sprite.emission = style.emission;
            sprite.alphaCutoff = style.alphaCutoff;
            sprite.doubleSided = style.doubleSided;
            sprites.push_back(sprite);

            pen += glyph.advance * scale;
        }

        lineCentre -= step;
    }

    return sprites;
}

std::vector<Sprite> facing(const Font& font, const std::string& content, Vec3 position,
                           Vec3 target, const TextStyle& style) {
    TextStyle turned = style;
    orientationTowards(target - position, turned.yawDegrees, turned.pitchDegrees);
    return build(font, content, position, turned);
}

} // namespace text
} // namespace blocky
