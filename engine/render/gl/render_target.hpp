#pragma once
// Somewhere to draw that is not the window.
//
// Every viewport shader used to finish its own frame: shade, expose, tone map,
// encode to sRGB, done. That is the right shape for a program whose only job
// is to put shaded blocks on screen, and the wrong one the moment anything
// needs to look at the finished picture -- an outline has to know what the
// neighbouring pixel is, and bloom has to know which pixels were bright.
//
// So the world is drawn here instead, in **linear light**, and one pass at the
// end does the looking. That is the arrangement the CPU renderers have always
// had: `Image` is linear HDR, `post/` filters it, `tonemap` runs once at the
// far end. The viewport now agrees with them about the order of operations as
// well as about the tone curve.
//
// ------------------------------------------------------------ the two halves
//
// Colour is RGBA16F because bloom needs values above one to find, and eight
// bits of sRGB has thrown those away by construction.
//
// Normals go in a second attachment rather than being reconstructed from
// depth. Reconstruction gives the normal of the *depth gradient*, which on a
// voxel world is the same thing right up until a flat floor viewed at a
// glancing angle, where it reads as a slope and the outline pass draws a line
// across the middle of nothing. Storing them costs one RGBA8 and removes the
// question.
#include "engine/render/gl/gl_loader.hpp"

namespace blocky {

class RenderTarget {
public:
    // Recreates the attachments when the size changed, and does nothing when
    // it did not. Called every frame with the window size, which is what makes
    // resizing a non-event for the caller.
    bool resize(int width, int height);

    void destroy();

    bool valid() const { return framebuffer_ != 0; }
    int  width() const { return width_; }
    int  height() const { return height_; }

    // Binds for drawing and points the fragment shaders at both attachments.
    void bindForDraw() const;

    // Back to the window.
    static void bindDefault();

    void bindColour(int unit) const;
    void bindNormal(int unit) const;
    void bindDepth(int unit) const;

private:
    gl::GLuint framebuffer_ = 0;
    gl::GLuint colour_ = 0;
    gl::GLuint normal_ = 0;
    gl::GLuint depth_ = 0;
    int width_ = 0, height_ = 0;
};

// A single colour attachment, for the bloom chain.
//
// Half and quarter sized, with linear filtering, because the whole trick of a
// cheap bloom is that the blur happens at a resolution where a four-tap filter
// covers as much of the screen as a much wider one would at full size.
class ColourTarget {
public:
    bool resize(int width, int height);
    void destroy();

    bool valid() const { return framebuffer_ != 0; }
    int  width() const { return width_; }
    int  height() const { return height_; }

    void bindForDraw() const;
    void bindColour(int unit) const;

private:
    gl::GLuint framebuffer_ = 0;
    gl::GLuint colour_ = 0;
    int width_ = 0, height_ = 0;
};

} // namespace blocky
