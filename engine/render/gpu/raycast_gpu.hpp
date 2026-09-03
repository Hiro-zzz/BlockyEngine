#pragma once
// The voxel traversal, on the GPU.
//
// Step one of moving the path tracer off the CPU, and deliberately only step
// one: this answers the same question `raycast()` answers -- which block does
// this ray hit, on which face -- and nothing else. No shading, no bounces, no
// entities, sprites or props. What it buys is a thing that can be checked
// against the CPU answer ray for ray, which is the only honest way to start.
//
// ------------------------------------------------------- no new dependency
//
// Compute shaders and shader storage buffers are both core OpenGL 4.3, and
// the viewport already opens a 4.6 core context through our own loader. So
// this costs nothing that the zero-dependency rule would object to: no
// Vulkan, no CUDA, no OptiX. The context is already there.
//
// -------------------------------------------------------- the world layout
//
// On the CPU the world is a hash map of dense 16^3 chunks, and the chunk grid
// doubles as the acceleration structure. On the GPU the hash map is the one
// part that does not travel: what replaces it is a *dense* index grid over
// the world's chunk bounding box, holding a slot number or -1.
//
// That is not a compromise. The worlds this engine builds are bounded and
// mostly solid within their bounds -- a film set is tens of chunks across --
// so a dense outer grid costs a few kilobytes and turns the hash lookup into
// one array read. The two-level DDA is otherwise the same algorithm, walking
// the same cells in the same order.
//
// ----------------------------------------------------------- what "matches"
//
// Not bit-exact, and it should not be claimed. GLSL may contract a multiply
// and an add into an FMA where the C++ did not, and division is allowed a
// different last bit. So the comparison this is built for tests the parts
// that are discrete and must be identical -- which block, which face, which
// id -- and holds `t` to a tolerance. A disagreement about the cell is a real
// failure; a disagreement in the last bits of `t` is arithmetic.
#include "engine/core/math.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/world/raycast.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace blocky {
namespace gpu {

// The world, flattened for the shader. Built on the CPU, then uploaded once.
struct PackedWorld {
    IVec3 gridMin{};   // chunk coordinate of the grid's low corner
    IVec3 gridDim{};   // extent of the index grid, in chunks
    IVec3 worldMin{};  // inclusive block bounds, as World reports them
    IVec3 worldMax{};

    // Slot into `blocks` for each cell of the chunk grid, or -1 for a chunk
    // that does not exist. Indexed (y * dim.z + z) * dim.x + x.
    std::vector<int32_t> chunkIndex;

    // 2048 words per occupied chunk: two BlockIds to a word, low half first,
    // addressed by the same (ly << 8) | (lz << 4) | lx that Chunk::index uses.
    std::vector<uint32_t> blocks;

    // One entry per BlockId, non-zero when the block stops light. Needed
    // because the opaque-only filter is a registry lookup on the CPU.
    std::vector<uint32_t> opaque;

    size_t chunkCount = 0;

    size_t bytes() const {
        return chunkIndex.size() * sizeof(int32_t) + blocks.size() * sizeof(uint32_t) +
               opaque.size() * sizeof(uint32_t);
    }
};

PackedWorld packWorld(const World& world);

// One ray in, one hit out. Laid out to match the shader's std430 blocks, so
// the upload and the read-back are both a straight memcpy.
struct GpuRay {
    float ox, oy, oz, pad0;
    float dx, dy, dz, pad1;
};

struct GpuHit {
    float t, u, v;
    float hit;        // 1 when something was found, 0 otherwise
    int32_t bx, by, bz, id;
    int32_t axis, normalSign, pad2, pad3;
};

// Owns the program and the buffers. One per context.
class Raycaster {
public:
    ~Raycaster();
    Raycaster() = default;
    Raycaster(const Raycaster&) = delete;
    Raycaster& operator=(const Raycaster&) = delete;

    // Compiles the compute program. Needs a current GL 4.3+ context.
    bool build(std::string* error = nullptr);

    // Uploads a packed world. Safe to call again with a different one.
    bool upload(const PackedWorld& world, std::string* error = nullptr);

    // Traces every ray and fills `hits`, which is resized to match.
    // `passThrough` and `opaqueOnly` are the same filter the CPU takes.
    bool trace(const std::vector<GpuRay>& rays, std::vector<GpuHit>& hits, float maxDistance,
               BlockId passThrough = block::Air, bool opaqueOnly = false,
               std::string* error = nullptr);

    void destroy();
    bool valid() const { return program_ != 0; }

    // Where the last trace's time went. Split three ways on purpose: for a
    // batch handed over from the CPU, the bus is the interesting number and
    // the traversal is not, and a single total hides that completely.
    double lastSeconds() const { return lastSeconds_; }
    double lastUploadSeconds() const { return lastUpload_; }
    double lastDispatchSeconds() const { return lastDispatch_; }
    double lastReadbackSeconds() const { return lastReadback_; }

private:
    gl::GLuint program_ = 0;
    gl::GLuint chunkIndexBuf_ = 0, blocksBuf_ = 0, opaqueBuf_ = 0;
    gl::GLuint raysBuf_ = 0, hitsBuf_ = 0;
    size_t raysCapacity_ = 0, hitsCapacity_ = 0;

    PackedWorld meta_;   // dimensions only; the vectors are dropped after upload
    double lastSeconds_ = 0.0;
    double lastUpload_ = 0.0, lastDispatch_ = 0.0, lastReadback_ = 0.0;
};

} // namespace gpu
} // namespace blocky
