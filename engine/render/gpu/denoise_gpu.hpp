#pragma once
// The a-trous denoiser, on the GPU.
//
// It earned its turn by arithmetic rather than by ambition. With the tracer
// moved across, a 1280x720 frame of this film costs about four seconds to
// trace and about three to denoise -- so the filter went from a tenth of the
// frame to half of it without getting any slower. The absolute number never
// changed; everything around it did.
//
// ------------------------------------------------------- why it stands alone
//
// It could have been another stage inside the wavefront, reading the buffers
// that are already in video memory when `resolve` finishes. It is not,
// because a filter that takes `RenderTargets` is a drop-in for the CPU one --
// the same call in the same place, usable by either tracer and by anything
// that produces the buffers by other means.
//
// The bus argument that shaped the tracer does not apply here. That design
// paid PCIe on every bounce; this pays it once for four frame-sized buffers
// and once for the answer, which is a few milliseconds against seconds of
// filtering. Measure before assuming the rule carries over.
#include "engine/core/image.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/trace/pathtrace.hpp"

#include <string>

namespace blocky {
namespace gpu {

class Denoiser {
public:
    ~Denoiser();
    Denoiser() = default;
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    bool build(std::string* error = nullptr);

    // Same contract as the CPU denoise(): with the auxiliary buffers missing
    // the input comes back unchanged rather than blurred blindly.
    bool run(const RenderTargets& targets, const DenoiseSettings& settings, Image& out,
             std::string* error = nullptr);

    void destroy();
    bool valid() const { return prepare_ != 0; }

    double lastUploadSeconds() const { return lastUpload_; }
    double lastFilterSeconds() const { return lastFilter_; }
    double lastReadbackSeconds() const { return lastReadback_; }
    double lastSeconds() const { return lastUpload_ + lastFilter_ + lastReadback_; }

private:
    bool resize(int width, int height);

    gl::GLuint prepare_ = 0, atrous_ = 0, finish_ = 0;
    gl::GLuint colorBuf_ = 0, albedoBuf_ = 0, normalBuf_ = 0, pingBuf_ = 0, pongBuf_ = 0;
    int width_ = 0, height_ = 0;

    double lastUpload_ = 0.0, lastFilter_ = 0.0, lastReadback_ = 0.0;
};

} // namespace gpu
} // namespace blocky
