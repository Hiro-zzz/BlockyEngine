#include "engine/render/gl/overlay.hpp"

#include "engine/render/gl/gl_loader.hpp"

#include <cstddef>

namespace blocky {
namespace {

const char* kVertex = R"GLSL(#version 460 core
layout(location = 0) in vec2  aPosition;   // pixels, origin top-left
layout(location = 1) in vec2  aUv;
layout(location = 2) in vec4  aColour;
layout(location = 3) in float aTextured;

// Two scalars rather than a vec2: the loader carries Uniform3fv and not
// Uniform2fv, and a spare component would be a lie about what this is.
uniform float uWidth;
uniform float uHeight;

out vec2  vUv;
out vec4  vColour;
out float vTextured;

void main() {
    // Pixels to clip space. The vertical flip is the whole conversion: a
    // window counts rows downwards and OpenGL counts them up.
    vec2 ndc = vec2((aPosition.x / uWidth) * 2.0 - 1.0,
                    1.0 - (aPosition.y / uHeight) * 2.0);
    vUv = aUv;
    vColour = aColour;
    vTextured = aTextured;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

const char* kFragment = R"GLSL(#version 460 core
uniform sampler2D uFont;

in vec2  vUv;
in vec4  vColour;
in float vTextured;

out vec4 fragColor;

void main() {
    // Only the coverage is read from the glyph sheet. The ink is white and
    // the sheet is uploaded as sRGB, so taking its colour would mean asking
    // the driver to linearise a value we are about to write out as-is --
    // whereas alpha is alpha in any encoding.
    float coverage = vTextured > 0.5 ? texture(uFont, vUv).a : 1.0;
    if (coverage <= 0.0) discard;

    // Written straight, without a tone curve. This is interface, not light.
    fragColor = vec4(vColour.rgb, vColour.a * coverage);
}
)GLSL";

// The glyph sheet as coverage: white ink, alpha from the font. Built rather
// than taken from `Font::texture()` directly because that one holds linear
// colour, and this only ever needs the mask.
ImageU8 coverageSheet(const Font& font) {
    const Texture& source = font.texture();
    ImageU8 image(source.width(), source.height());
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            uint8_t a = uint8_t(saturate(source.alphaAt(x, y)) * 255.0f + 0.5f);
            image.set(x, y, {255, 255, 255, a});
        }
    }
    return image;
}

} // namespace

bool Overlay::create(const AssetSource* source, std::string* error) {
    // The game font if there is one, and the compiled-in one otherwise. Same
    // fallback the tracer's text takes, and the reason a menu comes up with no
    // game files anywhere.
    if (!source || !font_.loadFromSource(*source)) font_.useBuiltin();
    if (!font_.loaded()) {
        if (error) *error = "overlay: no font";
        return false;
    }

    if (!shader_.build(kVertex, kFragment, error)) return false;
    if (!fontTexture_.upload(coverageSheet(font_))) {
        if (error) *error = "overlay: font upload failed";
        return false;
    }

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);

    const gl::GLsizei stride = sizeof(Vertex);
    auto offset = [](size_t bytes) { return reinterpret_cast<const void*>(bytes); };

    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, gl::FLOAT, 0, stride, offset(offsetof(Vertex, x)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, gl::FLOAT, 0, stride, offset(offsetof(Vertex, u)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, gl::FLOAT, 0, stride, offset(offsetof(Vertex, r)));
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribPointer(3, 1, gl::FLOAT, 0, stride, offset(offsetof(Vertex, textured)));

    gl::BindVertexArray(0);
    gl::BindBuffer(gl::ARRAY_BUFFER, 0);
    return true;
}

void Overlay::destroy() {
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    vbo_ = vao_ = 0;
    capacity_ = 0;
    fontTexture_.destroy();
    shader_.destroy();
}

void Overlay::begin(int widthPixels, int heightPixels) {
    width_ = widthPixels > 0 ? widthPixels : 1;
    height_ = heightPixels > 0 ? heightPixels : 1;
    vertices_.clear();
    inFrame_ = true;
}

void Overlay::push(float x, float y, float w, float h, Vec2 uvMin, Vec2 uvMax, Rgba colour,
                   float textured) {
    if (!inFrame_ || w <= 0.0f || h <= 0.0f || colour.a <= 0.0f) return;

    const Vertex a{x, y, uvMin.x, uvMin.y, colour.r, colour.g, colour.b, colour.a, textured};
    const Vertex b{x + w, y, uvMax.x, uvMin.y, colour.r, colour.g, colour.b, colour.a, textured};
    const Vertex c{x + w, y + h, uvMax.x, uvMax.y, colour.r, colour.g, colour.b, colour.a, textured};
    const Vertex d{x, y + h, uvMin.x, uvMax.y, colour.r, colour.g, colour.b, colour.a, textured};

    vertices_.push_back(a);
    vertices_.push_back(b);
    vertices_.push_back(c);
    vertices_.push_back(a);
    vertices_.push_back(c);
    vertices_.push_back(d);
}

void Overlay::rect(float x, float y, float w, float h, Rgba colour) {
    push(x, y, w, h, {0.0f, 0.0f}, {0.0f, 0.0f}, colour, 0.0f);
}

void Overlay::frame(float x, float y, float w, float h, float thickness, Rgba colour) {
    if (thickness <= 0.0f) return;
    rect(x, y, w, thickness, colour);                          // top
    rect(x, y + h - thickness, w, thickness, colour);          // bottom
    rect(x, y + thickness, thickness, h - thickness * 2.0f, colour);              // left
    rect(x + w - thickness, y + thickness, thickness, h - thickness * 2.0f, colour);  // right
}

void Overlay::verticalGradient(float x, float y, float w, float h, Rgba top, Rgba bottom) {
    if (!inFrame_ || w <= 0.0f || h <= 0.0f) return;

    const Vertex a{x, y, 0, 0, top.r, top.g, top.b, top.a, 0.0f};
    const Vertex b{x + w, y, 0, 0, top.r, top.g, top.b, top.a, 0.0f};
    const Vertex c{x + w, y + h, 0, 0, bottom.r, bottom.g, bottom.b, bottom.a, 0.0f};
    const Vertex d{x, y + h, 0, 0, bottom.r, bottom.g, bottom.b, bottom.a, 0.0f};

    vertices_.push_back(a);
    vertices_.push_back(b);
    vertices_.push_back(c);
    vertices_.push_back(a);
    vertices_.push_back(c);
    vertices_.push_back(d);
}

void Overlay::text(float x, float y, const std::string& line, float scale, Rgba colour) {
    if (scale <= 0.0f) return;

    const float cell = float(font_.cellPixels());
    if (cell <= 0.0f) return;

    float pen = x;
    for (unsigned char code : line) {
        Font::Glyph g = font_.glyph(code);
        if (!g.blank && g.width > 0.0f) {
            // The quad is the glyph's **ink** wide and the whole cell tall,
            // which is exactly the shape of the range the font already hands
            // out: `uvMax.x` is cropped to the ink and `uvMax.y` is not.
            //
            // Drawing a full cell wide instead is the mistake worth naming,
            // because it does not look like a mistake. The pen advances by the
            // ink plus one column, so a cell-wide quad overlaps the next glyph
            // by the difference, and the text comes out looking like a font
            // problem rather than a layout one.
            push(pen, y, g.width * scale, cell * scale, g.uvMin, g.uvMax, colour, 1.0f);
        }
        pen += g.advance * scale;
    }
}

void Overlay::textShadowed(float x, float y, const std::string& line, float scale, Rgba colour) {
    const Rgba shadow{0.0f, 0.0f, 0.0f, colour.a * 0.65f};
    text(x + scale, y + scale, line, scale, shadow);
    text(x, y, line, scale, colour);
}

float Overlay::measure(const std::string& line, float scale) const {
    return font_.measure(line) * scale;
}

float Overlay::lineHeight(float scale) const {
    return float(font_.cellPixels()) * scale;
}

void Overlay::end() {
    inFrame_ = false;
    if (vertices_.empty() || !shader_.valid()) return;

    gl::BindVertexArray(vao_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);

    const size_t bytes = vertices_.size() * sizeof(Vertex);
    if (vertices_.size() > capacity_) {
        // Grow, and keep the room. A menu's vertex count is steady from one
        // frame to the next, so this reallocates a handful of times at most
        // and then never again.
        capacity_ = vertices_.size() * 2;
        gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(capacity_ * sizeof(Vertex)), nullptr,
                       gl::DYNAMIC_DRAW);
    }
    gl::BufferSubData(gl::ARRAY_BUFFER, 0, gl::GLsizeiptr(bytes), vertices_.data());

    // Interface sits on top of everything and never occludes itself, so the
    // depth buffer has no say. Culling off because the quads are wound one way
    // and nobody should have to care which.
    gl::Disable(gl::DEPTH_TEST);
    gl::Disable(gl::CULL_FACE);
    gl::Enable(gl::BLEND);
    gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);

    shader_.use();
    shader_.setFloat("uWidth", float(width_));
    shader_.setFloat("uHeight", float(height_));
    shader_.setInt("uFont", 0);
    fontTexture_.bind(0);

    gl::DrawArrays(gl::TRIANGLES, 0, gl::GLsizei(vertices_.size()));

    gl::Disable(gl::BLEND);
    gl::Enable(gl::CULL_FACE);
    gl::Enable(gl::DEPTH_TEST);
    gl::BindVertexArray(0);
}

} // namespace blocky
