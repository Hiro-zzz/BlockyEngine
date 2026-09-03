#pragma once
// The path tracer again, as a wavefront.
//
// The megakernel in pathtrace_gpu.cpp reached 4.2x the CPU while the bare
// traversal it is built on reached 37x. The gap is what a megakernel costs:
// one thread carries a whole path, so a warp runs until its longest path
// finishes, and the register footprint is the union of every stage the path
// might pass through -- which is what holds occupancy down.
//
// A wavefront trades that for queues. Every stage is its own small kernel:
//
//     generate   one camera ray per pixel, all paths alive
//     extend     trace the active rays against the world
//     shade      emit shadow rays, sample the bsdf, keep the survivors
//     shadow     trace the shadow rays and add what got through
//     resolve    fold the finished paths into the frame
//
// Between `shade` and the next `extend` the survivors are compacted into a
// fresh list with one atomic, so bounce four dispatches over the paths that
// reached bounce four and not over the pixels that finished at bounce one.
//
// -------------------------------------------------------------- wave size
//
// One wave is exactly one sample for every pixel. That is not arbitrary: it
// makes a path's index its pixel's index, so the accumulation at the end is a
// plain add with no atomics and no collisions -- two paths in a wave can
// never want the same pixel. The cost is that the buffers are sized by the
// frame, which for 1280x720 comes to a couple of hundred megabytes.
#include "engine/core/image.hpp"
#include "engine/render/gpu/block_textures_gpu.hpp"
#include "engine/render/gpu/entities_gpu.hpp"
#include "engine/render/gpu/offgrid_gpu.hpp"
#include "engine/render/gpu/pathtrace_gpu.hpp"
#include "engine/render/gpu/raycast_gpu.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace gpu {

struct WavefrontStats {
    double uploadSeconds = 0.0;
    double traceSeconds = 0.0;
    double readbackSeconds = 0.0;
    int    waves = 0;
    int    dispatches = 0;
    size_t lights = 0;
    size_t bytesOnGpu = 0;

    // How many paths were still alive at each bounce, summed over the whole
    // render. This is the number the whole architecture exists to shrink, so
    // it is worth being able to look at.
    std::vector<uint64_t> aliveAtBounce;

    double totalSeconds() const { return uploadSeconds + traceSeconds + readbackSeconds; }
};

class WavefrontTracer {
public:
    ~WavefrontTracer();
    WavefrontTracer() = default;
    WavefrontTracer(const WavefrontTracer&) = delete;
    WavefrontTracer& operator=(const WavefrontTracer&) = delete;

    bool build(std::string* error = nullptr);

    // The world, the palette, the light list and the block textures. None of
    // it changes while the camera moves around one set, and packing a road
    // set is a hundred thousand blocks -- so this is called once per set and
    // not once per frame.
    bool uploadStatic(const Scene& scene, std::string* error = nullptr);

    // The cast, the sprites and the props, which a shot rebuilds every frame
    // because a new pose is a new set of boxes.
    bool uploadDynamic(const Scene& scene, std::string* error = nullptr);

    // Both, for a caller that renders one still and does not care.
    bool upload(const Scene& scene, std::string* error = nullptr) {
        return uploadStatic(scene, error) && uploadDynamic(scene, error);
    }

    bool render(const Scene& scene, const PathSettings& settings, Image& out,
                RenderTargets* aovs = nullptr, WavefrontStats* stats = nullptr,
                std::string* error = nullptr);

    void destroy();
    bool valid() const { return generate_ != 0; }

    // Read back the surviving-path count after every bounce. Costs a sync per
    // bounce, so it is off unless something is being measured.
    bool countAlive = false;

private:
    bool resizeTargets(int width, int height);
    void bindCommon() const;

    gl::GLuint generate_ = 0, extend_ = 0, shade_ = 0, shadow_ = 0, resolve_ = 0,
               advance_ = 0, args_ = 0;

    // World and scene, shared by every stage.
    gl::GLuint chunkIndexBuf_ = 0, blocksBuf_ = 0, materialBuf_ = 0, lightBuf_ = 0, cdfBuf_ = 0;

    // Frame-sized.
    gl::GLuint accumBuf_ = 0, albedoBuf_ = 0, normalBuf_ = 0;
    gl::GLuint pathBuf_ = 0, hitBuf_ = 0;
    gl::GLuint activeInBuf_ = 0, activeOutBuf_ = 0, counterBuf_ = 0;
    gl::GLuint sunQueueBuf_ = 0, areaQueueBuf_ = 0, argsBuf_ = 0;

    BlockTextureArray textures_;
    EntityData entities_;
    OffGridData offgrid_;

    PackedWorld meta_;
    size_t lightCount_ = 0;
    float  totalPower_ = 0.0f;
    int    width_ = 0, height_ = 0;
    size_t bytes_ = 0;
};

} // namespace gpu
} // namespace blocky
