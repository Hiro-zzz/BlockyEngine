#include "engine/world/raycast.hpp"

#include <limits>

namespace blocky {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Grid walker over cells of a uniform size. Used twice: once with the chunk
// size, once with a cell size of one block.
struct Dda {
    IVec3 cell{};
    IVec3 step{};
    Vec3  tMax{kInf, kInf, kInf};    // ray t at the next boundary, per axis
    Vec3  tDelta{kInf, kInf, kInf};  // ray t needed to cross one whole cell
    float t = 0.0f;                  // ray t where the current cell was entered
    int   axis = -1;                 // axis crossed on entry, -1 if none yet

    static Dda begin(const Ray& ray, float cellSize, float tStart, int entryAxis) {
        Dda d;
        d.t = tStart;
        d.axis = entryAxis;

        Vec3 p = ray.origin + ray.direction * tStart;
        d.cell = floorToInt(p / cellSize);

        for (int a = 0; a < 3; ++a) {
            float dir = ray.direction[a];
            if (dir > 0.0f) {
                d.step[a] = 1;
                float boundary = float(d.cell[a] + 1) * cellSize;
                d.tMax[a] = tStart + (boundary - p[a]) / dir;
                d.tDelta[a] = cellSize / dir;
            } else if (dir < 0.0f) {
                d.step[a] = -1;
                float boundary = float(d.cell[a]) * cellSize;
                d.tMax[a] = tStart + (boundary - p[a]) / dir;
                d.tDelta[a] = cellSize / -dir;
            } else {
                d.step[a] = 0;  // parallel to this axis: never crosses it
            }
        }
        return d;
    }

    // Ray t at which the current cell is left.
    float tExit() const { return minComponent(tMax); }

    void advance() {
        int a = 0;
        if (tMax.y < tMax[a]) a = 1;
        if (tMax.z < tMax[a]) a = 2;

        t = tMax[a];
        cell[a] += step[a];
        tMax[a] += tDelta[a];
        axis = a;
    }
};

// Slab test. `entryAxis` and `exitAxis` name the planes the ray crosses going
// in and coming out, which the traversal needs to report face normals at the
// boundary of the populated region.
bool intersectAabb(const Ray& ray, Vec3 lo, Vec3 hi, float& t0, float& t1,
                   int& entryAxis, int& exitAxis) {
    t0 = 0.0f;
    t1 = kInf;
    entryAxis = -1;
    exitAxis = -1;

    for (int a = 0; a < 3; ++a) {
        float inv = 1.0f / ray.direction[a];  // deliberately allowed to be infinite
        float tNear = (lo[a] - ray.origin[a]) * inv;
        float tFar  = (hi[a] - ray.origin[a]) * inv;
        if (inv < 0.0f) std::swap(tNear, tFar);

        if (tNear > t0) { t0 = tNear; entryAxis = a; }
        if (tFar < t1) { t1 = tFar; exitAxis = a; }
        if (t0 > t1) return false;
    }
    return true;
}

// Face-local coordinates, so the texture stage has something to sample with.
// Convention: u runs along the first non-normal axis, v runs downward, which
// matches how Minecraft textures are laid out.
Vec2 faceUv(Vec3 local, int axis, int stepSign) {
    switch (axis) {
        case 0: return {stepSign > 0 ? 1.0f - local.z : local.z, 1.0f - local.y};
        case 1: return {local.x, stepSign > 0 ? 1.0f - local.z : local.z};
        default: return {stepSign > 0 ? local.x : 1.0f - local.x, 1.0f - local.y};
    }
}

void fillHit(RayHit& hit, const Ray& ray, float t, IVec3 cell, BlockId id, int axis, int stepSign) {
    if (axis < 0) { axis = 1; stepSign = 1; }

    hit.t = t;
    hit.id = id;
    hit.block = cell;
    hit.position = ray.origin + ray.direction * t;
    hit.axis = axis;
    hit.normal = IVec3{0, 0, 0};
    hit.normal[axis] = -stepSign;

    Vec3 local = hit.position - toVec3(cell);
    local = minv(maxv(local, Vec3{0.0f}), Vec3{1.0f});
    hit.uv = faceUv(local, axis, stepSign);
}

// The far side of a medium, where it meets air rather than another block.
void fillAirBoundary(RayHit& hit, const Ray& ray, float t, int axis) {
    if (axis < 0) axis = 1;
    int stepSign = ray.direction[axis] > 0.0f ? 1 : -1;
    Vec3 position = ray.origin + ray.direction * t;
    fillHit(hit, ray, t, floorToInt(position), block::Air, axis, stepSign);
}

// Walk blocks inside one chunk over the ray interval [tStart, tEnd].
bool traceChunk(const World& world, const World::Chunk& chunk, IVec3 chunkCoord, const Ray& ray,
                float tStart, float tEnd, int entryAxis, RayFilter filter, RayHit* hit) {
    const BlockRegistry& registry = world.registry();
    Dda dda = Dda::begin(ray, 1.0f, tStart, entryAxis);

    while (dda.t <= tEnd) {
        // Float error at a chunk seam can land the first cell just outside
        // the chunk. Skip such cells and keep walking -- abandoning the whole
        // chunk here punches 16x16 holes straight through the world.
        if (World::toChunkCoord(dda.cell) != chunkCoord) {
            if (dda.tExit() > tEnd) return false;
            dda.advance();
            continue;
        }

        int lx = dda.cell.x & World::kChunkMask;
        int ly = dda.cell.y & World::kChunkMask;
        int lz = dda.cell.z & World::kChunkMask;
        BlockId id = chunk.blocks[World::Chunk::index(lx, ly, lz)];

        bool stops = id != filter.passThrough && (!filter.opaqueOnly || registry[id].opaque);
        if (stops) {
            if (!hit) return true;
            int axis = dda.axis;
            int stepSign = (axis >= 0 && dda.step[axis] != 0) ? dda.step[axis] : 1;
            fillHit(*hit, ray, dda.t, dda.cell, id, axis, stepSign);
            return true;
        }

        if (dda.tExit() > tEnd) return false;
        dda.advance();
    }
    return false;
}

bool traceWorld(const World& world, const Ray& ray, float maxDistance, RayFilter filter, RayHit* hit) {
    // In medium mode the caller is inside a block and is asking where that
    // block type stops. Air then counts as a surface, so running out of world
    // is an answer rather than a miss.
    const bool mediumMode = filter.passThrough != block::Air;

    if (!world.hasBlocks() || maxDistance <= 0.0f) return false;

    // The populated region, in world units. A block at p spans [p, p+1).
    Vec3 lo = toVec3(world.minBlock());
    Vec3 hi = toVec3(world.maxBlock()) + Vec3{1.0f};

    float t0 = 0.0f, t1 = 0.0f;
    int entryAxis = -1, exitAxis = -1;
    if (!intersectAabb(ray, lo, hi, t0, t1, entryAxis, exitAxis)) return false;

    bool clippedByDistance = maxDistance < t1;
    t1 = std::min(t1, maxDistance);
    if (t0 > t1) return false;

    Dda chunks = Dda::begin(ray, float(World::kChunkSize), t0, entryAxis);

    while (chunks.t <= t1) {
        if (const World::Chunk* chunk = world.findChunk(chunks.cell)) {
            float segmentEnd = std::min(chunks.tExit(), t1);
            if (traceChunk(world, *chunk, chunks.cell, ray, chunks.t, segmentEnd,
                           chunks.axis, filter, hit)) {
                return true;
            }
        } else if (mediumMode) {
            // A chunk that does not exist is solid air, so the medium ends
            // exactly where the ray enters it.
            if (hit) fillAirBoundary(*hit, ray, chunks.t, chunks.axis);
            return true;
        }
        if (chunks.tExit() > t1) break;
        chunks.advance();
    }

    if (mediumMode && !clippedByDistance) {
        // The ray left the populated region: nothing can be inside a medium
        // out there, so the interface is at the boundary itself.
        if (hit) fillAirBoundary(*hit, ray, t1, exitAxis);
        return true;
    }
    return false;
}

} // namespace

bool raycast(const World& world, const Ray& ray, float maxDistance, RayHit& hit, RayFilter filter) {
    return traceWorld(world, ray, maxDistance, filter, &hit);
}

bool raycastOccluded(const World& world, const Ray& ray, float maxDistance) {
    RayFilter filter;
    filter.opaqueOnly = true;
    return traceWorld(world, ray, maxDistance, filter, nullptr);
}

Vec3 rayTransmittance(const World& world, const Ray& ray, float maxDistance) {
    Vec3 transmittance{1.0f, 1.0f, 1.0f};
    if (!world.hasBlocks() || maxDistance <= 0.0f) return transmittance;

    Vec3 lo = toVec3(world.minBlock());
    Vec3 hi = toVec3(world.maxBlock()) + Vec3{1.0f};

    float t0 = 0.0f, t1 = 0.0f;
    int entryAxis = -1, exitAxis = -1;
    if (!intersectAabb(ray, lo, hi, t0, t1, entryAxis, exitAxis)) return transmittance;

    t1 = std::min(t1, maxDistance);
    if (t0 > t1) return transmittance;

    const BlockRegistry& registry = world.registry();
    Dda chunks = Dda::begin(ray, float(World::kChunkSize), t0, entryAxis);

    while (chunks.t <= t1) {
        const World::Chunk* chunk = world.findChunk(chunks.cell);
        if (chunk) {
            float segmentEnd = std::min(chunks.tExit(), t1);
            Dda dda = Dda::begin(ray, 1.0f, chunks.t, chunks.axis);

            while (dda.t <= segmentEnd) {
                if (World::toChunkCoord(dda.cell) != chunks.cell) {
                    if (dda.tExit() > segmentEnd) break;
                    dda.advance();
                    continue;
                }

                int lx = dda.cell.x & World::kChunkMask;
                int ly = dda.cell.y & World::kChunkMask;
                int lz = dda.cell.z & World::kChunkMask;
                BlockId id = chunk->blocks[World::Chunk::index(lx, ly, lz)];

                if (id != block::Air) {
                    const BlockDef& def = registry[id];
                    if (def.opaque) return Vec3{0.0f};

                    if (def.transmissive()) {
                        // How far the ray actually travels inside this block.
                        float length = std::max(0.0f, std::min(dda.tExit(), segmentEnd) - dda.t);
                        Vec3 a = def.absorption * length;
                        transmittance *= Vec3{std::exp(-a.x), std::exp(-a.y), std::exp(-a.z)};
                        if (maxComponent(transmittance) < 1e-4f) return Vec3{0.0f};
                    }
                }

                if (dda.tExit() > segmentEnd) break;
                dda.advance();
            }
        }
        if (chunks.tExit() > t1) break;
        chunks.advance();
    }
    return transmittance;
}

} // namespace blocky
