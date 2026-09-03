#include "engine/render/gl/render_target.hpp"

namespace blocky {
namespace {

gl::GLuint makeTexture(int width, int height, gl::GLenum internalFormat, gl::GLenum format,
                       gl::GLenum type, gl::GLenum filter) {
    gl::GLuint texture = 0;
    gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::GLint(internalFormat), width, height, 0, format, type,
                   nullptr);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::GLint(filter));
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::GLint(filter));

    // Clamped, so a filter reaching past the edge of the frame samples the
    // edge rather than wrapping round to the far side -- which on a bloom
    // shows up as a bright object bleeding light onto the opposite border.
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::GLint(gl::CLAMP_TO_EDGE));
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::GLint(gl::CLAMP_TO_EDGE));
    gl::BindTexture(gl::TEXTURE_2D, 0);
    return texture;
}

} // namespace

bool RenderTarget::resize(int width, int height) {
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    if (framebuffer_ != 0 && width == width_ && height == height_) return true;

    destroy();
    width_ = width;
    height_ = height;

    colour_ = makeTexture(width, height, gl::RGBA16F, gl::RGBA, gl::FLOAT, gl::LINEAR);
    normal_ = makeTexture(width, height, gl::RGBA8, gl::RGBA, gl::UNSIGNED_BYTE, gl::NEAREST);
    depth_ = makeTexture(width, height, gl::DEPTH_COMPONENT24, gl::DEPTH_COMPONENT,
                         gl::UNSIGNED_INT, gl::NEAREST);

    gl::GenFramebuffers(1, &framebuffer_);
    gl::BindFramebuffer(gl::FRAMEBUFFER, framebuffer_);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, colour_, 0);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT1, gl::TEXTURE_2D, normal_, 0);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, depth_, 0);

    const gl::GLenum draws[2] = {gl::COLOR_ATTACHMENT0, gl::COLOR_ATTACHMENT1};
    gl::DrawBuffers(2, draws);

    const bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    if (!ok) destroy();
    return ok;
}

void RenderTarget::destroy() {
    if (framebuffer_) gl::DeleteFramebuffers(1, &framebuffer_);
    if (colour_) gl::DeleteTextures(1, &colour_);
    if (normal_) gl::DeleteTextures(1, &normal_);
    if (depth_) gl::DeleteTextures(1, &depth_);
    framebuffer_ = colour_ = normal_ = depth_ = 0;
    width_ = height_ = 0;
}

void RenderTarget::bindForDraw() const {
    gl::BindFramebuffer(gl::FRAMEBUFFER, framebuffer_);
    const gl::GLenum draws[2] = {gl::COLOR_ATTACHMENT0, gl::COLOR_ATTACHMENT1};
    gl::DrawBuffers(2, draws);
    gl::Viewport(0, 0, width_, height_);
}

void RenderTarget::bindDefault() { gl::BindFramebuffer(gl::FRAMEBUFFER, 0); }

void RenderTarget::bindColour(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D, colour_);
}
void RenderTarget::bindNormal(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D, normal_);
}
void RenderTarget::bindDepth(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D, depth_);
}

// ---------------------------------------------------------------------------

bool ColourTarget::resize(int width, int height) {
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    if (framebuffer_ != 0 && width == width_ && height == height_) return true;

    destroy();
    width_ = width;
    height_ = height;

    colour_ = makeTexture(width, height, gl::RGBA16F, gl::RGBA, gl::FLOAT, gl::LINEAR);

    gl::GenFramebuffers(1, &framebuffer_);
    gl::BindFramebuffer(gl::FRAMEBUFFER, framebuffer_);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, colour_, 0);

    const gl::GLenum draws[1] = {gl::COLOR_ATTACHMENT0};
    gl::DrawBuffers(1, draws);

    const bool ok = gl::CheckFramebufferStatus(gl::FRAMEBUFFER) == gl::FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    if (!ok) destroy();
    return ok;
}

void ColourTarget::destroy() {
    if (framebuffer_) gl::DeleteFramebuffers(1, &framebuffer_);
    if (colour_) gl::DeleteTextures(1, &colour_);
    framebuffer_ = colour_ = 0;
    width_ = height_ = 0;
}

void ColourTarget::bindForDraw() const {
    gl::BindFramebuffer(gl::FRAMEBUFFER, framebuffer_);
    const gl::GLenum draws[1] = {gl::COLOR_ATTACHMENT0};
    gl::DrawBuffers(1, draws);
    gl::Viewport(0, 0, width_, height_);
}

void ColourTarget::bindColour(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D, colour_);
}

} // namespace blocky
