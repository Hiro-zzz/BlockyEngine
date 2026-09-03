#include "engine/render/gpu/raycast_gpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace blocky {
namespace gpu {
namespace {

// The traversal, transliterated from engine/world/raycast.cpp rather than
// reinvented. Every difference from that file is either forced by GLSL or is
// a bug, which is a useful thing to be able to say while reading it.
//
// Two departures worth naming, both deliberate:
//
//   * The chunk lookup is a dense index grid, not a hash map. Same cells in
//     the same order; only the way a chunk is found differs.
//   * Both loops carry an iteration cap. A GPU has no way to survive a loop
//     that does not end -- the driver kills the context and takes the process
//     with it -- so the caps exist to turn a bug into a wrong pixel instead
//     of a reset. They are set far above anything these worlds can reach and
//     the host checks that they were never the reason a walk stopped.
const char* kSource = R"GLSL(
#version 430 core
layout(local_size_x = 64) in;

struct RayIn { vec4 o; vec4 d; };
struct Hit   { vec4 tuvh; ivec4 cell; ivec4 face; };

layout(std430, binding = 0) readonly buffer ChunkIndexBuf { int  chunkIndex[]; };
layout(std430, binding = 1) readonly buffer BlocksBuf     { uint blockWords[]; };
layout(std430, binding = 2) readonly buffer OpaqueBuf     { uint opaqueFlag[]; };
layout(std430, binding = 3) readonly buffer RaysBuf       { RayIn rays[]; };
layout(std430, binding = 4)          buffer HitsBuf       { Hit  hits[]; };

uniform ivec3 uGridMin;
uniform ivec3 uGridDim;
uniform ivec3 uWorldMin;
uniform ivec3 uWorldMax;
uniform int   uRayCount;
uniform float uMaxDistance;
uniform int   uPassThrough;
uniform int   uOpaqueOnly;

const float INF = uintBitsToFloat(0x7F800000u);
const int kMaxChunkSteps = 8192;
const int kMaxBlockSteps = 65536;

struct Dda {
    ivec3 cell;
    ivec3 stp;
    vec3  tMax;
    vec3  tDelta;
    float t;
    int   axis;
};

Dda ddaBegin(vec3 ro, vec3 rd, float cellSize, float tStart, int entryAxis) {
    Dda d;
    d.t = tStart;
    d.axis = entryAxis;
    d.tMax = vec3(INF);
    d.tDelta = vec3(INF);
    d.stp = ivec3(0);

    vec3 p = ro + rd * tStart;
    d.cell = ivec3(floor(p / cellSize));

    for (int a = 0; a < 3; ++a) {
        float dir = rd[a];
        if (dir > 0.0) {
            d.stp[a] = 1;
            float boundary = float(d.cell[a] + 1) * cellSize;
            d.tMax[a] = tStart + (boundary - p[a]) / dir;
            d.tDelta[a] = cellSize / dir;
        } else if (dir < 0.0) {
            d.stp[a] = -1;
            float boundary = float(d.cell[a]) * cellSize;
            d.tMax[a] = tStart + (boundary - p[a]) / dir;
            d.tDelta[a] = cellSize / -dir;
        }
    }
    return d;
}

float ddaExit(vec3 tMax) { return min(min(tMax.x, tMax.y), tMax.z); }

void ddaAdvance(inout Dda d) {
    int a = 0;
    if (d.tMax.y < d.tMax[a]) a = 1;
    if (d.tMax.z < d.tMax[a]) a = 2;
    d.t = d.tMax[a];
    d.cell[a] += d.stp[a];
    d.tMax[a] += d.tDelta[a];
    d.axis = a;
}

bool intersectAabb(vec3 ro, vec3 rd, vec3 lo, vec3 hi,
                   out float t0, out float t1, out int entryAxis, out int exitAxis) {
    t0 = 0.0;
    t1 = INF;
    entryAxis = -1;
    exitAxis = -1;
    for (int a = 0; a < 3; ++a) {
        float inv = 1.0 / rd[a];
        float tNear = (lo[a] - ro[a]) * inv;
        float tFar  = (hi[a] - ro[a]) * inv;
        if (inv < 0.0) { float tmp = tNear; tNear = tFar; tFar = tmp; }
        if (tNear > t0) { t0 = tNear; entryAxis = a; }
        if (tFar  < t1) { t1 = tFar;  exitAxis  = a; }
        if (t0 > t1) return false;
    }
    return true;
}

int chunkSlot(ivec3 c) {
    ivec3 g = c - uGridMin;
    if (any(lessThan(g, ivec3(0))) || any(greaterThanEqual(g, uGridDim))) return -1;
    return chunkIndex[(g.y * uGridDim.z + g.z) * uGridDim.x + g.x];
}

uint blockAt(int slot, ivec3 cell) {
    int lx = cell.x & 15, ly = cell.y & 15, lz = cell.z & 15;
    int idx = (ly << 8) | (lz << 4) | lx;
    uint word = blockWords[uint(slot) * 2048u + uint(idx >> 1)];
    return ((idx & 1) == 0) ? (word & 0xFFFFu) : (word >> 16);
}

vec2 faceUv(vec3 local, int axis, int stepSign) {
    if (axis == 0) return vec2(stepSign > 0 ? 1.0 - local.z : local.z, 1.0 - local.y);
    if (axis == 1) return vec2(local.x, stepSign > 0 ? 1.0 - local.z : local.z);
    return vec2(stepSign > 0 ? local.x : 1.0 - local.x, 1.0 - local.y);
}

void writeHit(uint i, vec3 ro, vec3 rd, float t, ivec3 cell, int id, int axis, int stepSign) {
    if (axis < 0) { axis = 1; stepSign = 1; }
    vec3 pos = ro + rd * t;
    vec3 local = clamp(pos - vec3(cell), vec3(0.0), vec3(1.0));
    vec2 uv = faceUv(local, axis, stepSign);
    hits[i].tuvh = vec4(t, uv.x, uv.y, 1.0);
    hits[i].cell = ivec4(cell, id);
    hits[i].face = ivec4(axis, -stepSign, 0, 0);
}

void writeAirBoundary(uint i, vec3 ro, vec3 rd, float t, int axis) {
    if (axis < 0) axis = 1;
    int stepSign = rd[axis] > 0.0 ? 1 : -1;
    vec3 pos = ro + rd * t;
    writeHit(i, ro, rd, t, ivec3(floor(pos)), 0, axis, stepSign);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(uRayCount)) return;

    hits[i].tuvh = vec4(0.0);
    hits[i].cell = ivec4(0);
    hits[i].face = ivec4(0);

    vec3 ro = rays[i].o.xyz;
    vec3 rd = rays[i].d.xyz;

    bool mediumMode = (uPassThrough != 0);

    vec3 lo = vec3(uWorldMin);
    vec3 hi = vec3(uWorldMax) + vec3(1.0);

    float t0, t1;
    int entryAxis, exitAxis;
    if (!intersectAabb(ro, rd, lo, hi, t0, t1, entryAxis, exitAxis)) return;

    bool clippedByDistance = uMaxDistance < t1;
    t1 = min(t1, uMaxDistance);
    if (t0 > t1) return;

    Dda chunks = ddaBegin(ro, rd, 16.0, t0, entryAxis);

    for (int outer = 0; outer < kMaxChunkSteps; ++outer) {
        if (chunks.t > t1) break;

        int slot = chunkSlot(chunks.cell);
        if (slot >= 0) {
            float segEnd = min(ddaExit(chunks.tMax), t1);
            Dda d = ddaBegin(ro, rd, 1.0, chunks.t, chunks.axis);

            for (int inner = 0; inner < kMaxBlockSteps; ++inner) {
                if (d.t > segEnd) break;

                // Float error at a chunk seam can put the first cell just
                // outside. Skipping it and walking on is what keeps the world
                // from growing 16x16 holes; abandoning the chunk is not.
                if ((d.cell >> 4) != chunks.cell) {
                    if (ddaExit(d.tMax) > segEnd) break;
                    ddaAdvance(d);
                    continue;
                }

                uint id = blockAt(slot, d.cell);
                bool stops = (int(id) != uPassThrough) &&
                             (uOpaqueOnly == 0 || opaqueFlag[id] != 0u);
                if (stops) {
                    int axis = d.axis;
                    int stepSign = (axis >= 0 && d.stp[axis] != 0) ? d.stp[axis] : 1;
                    writeHit(i, ro, rd, d.t, d.cell, int(id), axis, stepSign);
                    return;
                }

                if (ddaExit(d.tMax) > segEnd) break;
                ddaAdvance(d);
            }
        } else if (mediumMode) {
            writeAirBoundary(i, ro, rd, chunks.t, chunks.axis);
            return;
        }

        if (ddaExit(chunks.tMax) > t1) break;
        ddaAdvance(chunks);
    }

    if (mediumMode && !clippedByDistance) {
        writeAirBoundary(i, ro, rd, t1, exitAxis);
    }
}
)GLSL";

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

gl::GLuint compile(std::string* error) {
    const gl::GLuint shader = gl::CreateShader(gl::COMPUTE_SHADER);
    gl::ShaderSource(shader, 1, &kSource, nullptr);
    gl::CompileShader(shader);

    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        setError(error, "compute shader: " + log);
        gl::DeleteShader(shader);
        return 0;
    }

    const gl::GLuint program = gl::CreateProgram();
    gl::AttachShader(program, shader);
    gl::LinkProgram(program);
    gl::DeleteShader(shader);

    gl::GetProgramiv(program, gl::LINK_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetProgramiv(program, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetProgramInfoLog(program, length, nullptr, log.data());
        setError(error, "compute link: " + log);
        gl::DeleteProgram(program);
        return 0;
    }
    return program;
}

void uploadBuffer(gl::GLuint& buffer, const void* data, size_t bytes, gl::GLenum usage) {
    if (buffer == 0) gl::GenBuffers(1, &buffer);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buffer);
    gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(bytes), data, usage);
}

} // namespace

// --------------------------------------------------------------- packing

PackedWorld packWorld(const World& world) {
    PackedWorld packed;
    if (!world.hasBlocks()) return packed;

    packed.worldMin = world.minBlock();
    packed.worldMax = world.maxBlock();

    // The chunk bounding box, from the chunks that actually exist rather than
    // from the block bounds -- those are deliberately never narrowed when a
    // block is removed, so they can be wider than the populated chunks.
    IVec3 lo{ 1 << 30,  1 << 30,  1 << 30};
    IVec3 hi{-(1 << 30), -(1 << 30), -(1 << 30)};
    size_t count = 0;
    world.forEachChunk([&](IVec3 coord, const World::Chunk&) {
        lo = minv(lo, coord);
        hi = maxv(hi, coord);
        ++count;
    });
    if (count == 0) return packed;

    packed.gridMin = lo;
    packed.gridDim = IVec3{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
    packed.chunkCount = count;

    const size_t cells = size_t(packed.gridDim.x) * size_t(packed.gridDim.y) *
                         size_t(packed.gridDim.z);
    packed.chunkIndex.assign(cells, -1);

    constexpr size_t kWordsPerChunk = size_t(World::kChunkVolume) / 2;
    packed.blocks.assign(count * kWordsPerChunk, 0u);

    size_t slot = 0;
    world.forEachChunk([&](IVec3 coord, const World::Chunk& chunk) {
        const IVec3 g{coord.x - packed.gridMin.x, coord.y - packed.gridMin.y,
                      coord.z - packed.gridMin.z};
        const size_t cell = (size_t(g.y) * size_t(packed.gridDim.z) + size_t(g.z)) *
                                size_t(packed.gridDim.x) + size_t(g.x);
        packed.chunkIndex[cell] = int32_t(slot);

        uint32_t* out = packed.blocks.data() + slot * kWordsPerChunk;
        for (size_t i = 0; i < size_t(World::kChunkVolume); i += 2) {
            out[i / 2] = uint32_t(chunk.blocks[i]) | (uint32_t(chunk.blocks[i + 1]) << 16);
        }
        ++slot;
    });

    const BlockRegistry& registry = world.registry();
    packed.opaque.resize(registry.size());
    for (size_t i = 0; i < registry.size(); ++i) {
        packed.opaque[i] = registry[BlockId(i)].opaque ? 1u : 0u;
    }
    return packed;
}

// -------------------------------------------------------------- raycaster

Raycaster::~Raycaster() { destroy(); }

bool Raycaster::build(std::string* error) {
    destroy();
    program_ = compile(error);
    return program_ != 0;
}

bool Raycaster::upload(const PackedWorld& world, std::string* error) {
    if (world.chunkIndex.empty() || world.blocks.empty()) {
        setError(error, "gpu raycaster: the world is empty");
        return false;
    }

    uploadBuffer(chunkIndexBuf_, world.chunkIndex.data(),
                 world.chunkIndex.size() * sizeof(int32_t), gl::STATIC_DRAW);
    uploadBuffer(blocksBuf_, world.blocks.data(), world.blocks.size() * sizeof(uint32_t),
                 gl::STATIC_DRAW);
    uploadBuffer(opaqueBuf_, world.opaque.data(), world.opaque.size() * sizeof(uint32_t),
                 gl::STATIC_DRAW);

    meta_ = PackedWorld{};
    meta_.gridMin = world.gridMin;
    meta_.gridDim = world.gridDim;
    meta_.worldMin = world.worldMin;
    meta_.worldMax = world.worldMax;
    meta_.chunkCount = world.chunkCount;

    return gl::GetError() == gl::NO_ERROR_;
}

bool Raycaster::trace(const std::vector<GpuRay>& rays, std::vector<GpuHit>& hits,
                      float maxDistance, BlockId passThrough, bool opaqueOnly,
                      std::string* error) {
    if (program_ == 0) { setError(error, "gpu raycaster: not built"); return false; }
    if (rays.empty()) { hits.clear(); return true; }

    const auto started = std::chrono::steady_clock::now();

    if (rays.size() > raysCapacity_) {
        uploadBuffer(raysBuf_, nullptr, rays.size() * sizeof(GpuRay), gl::DYNAMIC_DRAW);
        raysCapacity_ = rays.size();
    }
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, raysBuf_);
    gl::BufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                      gl::GLsizeiptr(rays.size() * sizeof(GpuRay)), rays.data());

    if (rays.size() > hitsCapacity_) {
        uploadBuffer(hitsBuf_, nullptr, rays.size() * sizeof(GpuHit), gl::DYNAMIC_READ);
        hitsCapacity_ = rays.size();
    }

    gl::UseProgram(program_);
    gl::Uniform3i(gl::GetUniformLocation(program_, "uGridMin"), meta_.gridMin.x, meta_.gridMin.y,
                  meta_.gridMin.z);
    gl::Uniform3i(gl::GetUniformLocation(program_, "uGridDim"), meta_.gridDim.x, meta_.gridDim.y,
                  meta_.gridDim.z);
    gl::Uniform3i(gl::GetUniformLocation(program_, "uWorldMin"), meta_.worldMin.x,
                  meta_.worldMin.y, meta_.worldMin.z);
    gl::Uniform3i(gl::GetUniformLocation(program_, "uWorldMax"), meta_.worldMax.x,
                  meta_.worldMax.y, meta_.worldMax.z);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uRayCount"), int(rays.size()));
    gl::Uniform1f(gl::GetUniformLocation(program_, "uMaxDistance"), maxDistance);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uPassThrough"), int(passThrough));
    gl::Uniform1i(gl::GetUniformLocation(program_, "uOpaqueOnly"), opaqueOnly ? 1 : 0);

    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, chunkIndexBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 1, blocksBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 2, opaqueBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, raysBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, hitsBuf_);
    gl::Finish();
    const auto uploaded = std::chrono::steady_clock::now();

    const gl::GLuint groups = gl::GLuint((rays.size() + 63) / 64);
    gl::DispatchCompute(groups, 1, 1);
    gl::MemoryBarrier(gl::SHADER_STORAGE_BARRIER_BIT | gl::BUFFER_UPDATE_BARRIER_BIT);
    gl::Finish();
    const auto dispatched = std::chrono::steady_clock::now();

    hits.assign(rays.size(), GpuHit{});
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, hitsBuf_);
    gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                         gl::GLsizeiptr(hits.size() * sizeof(GpuHit)), hits.data());

    const auto done = std::chrono::steady_clock::now();
    lastUpload_ = std::chrono::duration<double>(uploaded - started).count();
    lastDispatch_ = std::chrono::duration<double>(dispatched - uploaded).count();
    lastReadback_ = std::chrono::duration<double>(done - dispatched).count();
    lastSeconds_ = std::chrono::duration<double>(done - started).count();

    const gl::GLenum err = gl::GetError();
    if (err != gl::NO_ERROR_) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "gpu raycaster: GL error 0x%04X", unsigned(err));
        setError(error, buf);
        return false;
    }
    return true;
}

void Raycaster::destroy() {
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
    const gl::GLuint buffers[] = {chunkIndexBuf_, blocksBuf_, opaqueBuf_, raysBuf_, hitsBuf_};
    for (gl::GLuint b : buffers) {
        if (b) gl::DeleteBuffers(1, &b);
    }
    chunkIndexBuf_ = blocksBuf_ = opaqueBuf_ = raysBuf_ = hitsBuf_ = 0;
    raysCapacity_ = hitsCapacity_ = 0;
}

} // namespace gpu
} // namespace blocky
