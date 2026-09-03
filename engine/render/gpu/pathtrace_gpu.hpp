#pragma once
// The path tracer, on the GPU.
//
// Step two of the port, and the one that was always going to decide whether
// the exercise is worth anything. Step one measured the traversal at ten to
// thirty-seven times the CPU -- and then measured the bus eating three
// quarters of that, because a design that ships rays out and hits back pays
// PCIe on every bounce.
//
// So nothing crosses the bus here except the finished picture. Camera rays
// are generated on the GPU, the whole path loop runs there, and the
// accumulation buffer comes back once, at the end.
//
// ------------------------------------------------------------- what it does
//
// The voxel world only: no entities, sprites or props, and no block textures.
// Everything else is the integrator from render/trace/pathtrace.cpp -- the
// same four lobes, the same next-event estimation over sun and emissive
// faces, the same refracting media with Beer-Lambert absorption, the same
// Russian roulette, the same firefly clamp.
//
// The CPU renderer is not going anywhere. It is the reference this is checked
// against, the way ffmpeg is the reference for the codec: a thing that agrees
// with itself proves nothing.
//
// ------------------------------------------------- why it dispatches in bits
//
// Windows kills a GPU kernel that runs longer than a couple of seconds and
// resets the driver under it. A 1280x720 frame at 64 samples is far past
// that in one go, so the samples are dispatched in small batches that
// accumulate into a persistent buffer. That is also where the progress
// report comes from, and it costs nothing: the batching is on the host.
#include "engine/core/image.hpp"
#include "engine/render/gpu/raycast_gpu.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace gpu {

// Per-block material constants, in the shader's layout. Sixty-four bytes a
// block and there are a few dozen blocks, so this is nothing.
struct GpuMaterial {
    float albedo[3], roughness;
    float emission[3], metallic;
    float absorption[3], ior;
    float transmission, opaque, pad0, pad1;
    // Grass is brown on five faces and green on the sixth. The CPU resolves
    // that in surfaceAlbedo(); without it here the GPU render came back with
    // a brown lawn, which is what the comparison was built to catch.
    float topTint[3], tintTop;
};

// One emissive face, and the same power weighting the CPU selects by.
struct GpuLight {
    float origin[3], power;
    float edgeU[3], pad0;
    float edgeV[3], pad1;
    float normal[3], pad2;
    float radiance[3], pad3;
};

struct TraceStats {
    double uploadSeconds = 0.0;
    double traceSeconds = 0.0;
    double readbackSeconds = 0.0;
    int    batches = 0;
    size_t lights = 0;

    double totalSeconds() const { return uploadSeconds + traceSeconds + readbackSeconds; }
};

class PathTracer {
public:
    ~PathTracer();
    PathTracer() = default;
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    bool build(std::string* error = nullptr);

    // Packs and uploads the world, the palette and the light list. Call again
    // whenever the world changes; a shot that only moves the camera does not
    // need to.
    bool upload(const Scene& scene, std::string* error = nullptr);

    // Renders into `out`, and fills `aovs` when given. The scene is passed
    // again because the camera, sun and sky are uniforms rather than uploads.
    bool render(const Scene& scene, const PathSettings& settings, Image& out,
                RenderTargets* aovs = nullptr, TraceStats* stats = nullptr,
                std::string* error = nullptr);

    void destroy();
    bool valid() const { return program_ != 0; }

    // Samples per dispatch. Zero picks a size from the frame's pixel count:
    // Windows resets a driver whose kernel runs longer than a couple of
    // seconds, so a whole 64-sample frame cannot go in one go -- but 128
    // dispatches of four samples spend most of their time synchronising.
    // Set it by hand to force one or the other.
    int samplesPerBatch = 0;

private:
    bool resizeTargets(int width, int height);

    gl::GLuint program_ = 0;
    gl::GLuint chunkIndexBuf_ = 0, blocksBuf_ = 0;
    gl::GLuint materialBuf_ = 0, lightBuf_ = 0, cdfBuf_ = 0;
    gl::GLuint accumBuf_ = 0, albedoBuf_ = 0, normalBuf_ = 0;

    PackedWorld meta_;
    size_t lightCount_ = 0;
    float  totalPower_ = 0.0f;
    int    width_ = 0, height_ = 0;
};

} // namespace gpu
} // namespace blocky
