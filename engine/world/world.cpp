#include "engine/world/world.hpp"

namespace blocky {

BlockId World::get(IVec3 p) const {
    const Chunk* chunk = findChunk(toChunkCoord(p));
    if (!chunk) return block::Air;
    return chunk->blocks[Chunk::index(p.x & kChunkMask, p.y & kChunkMask, p.z & kChunkMask)];
}

void World::set(IVec3 p, BlockId id) {
    IVec3 chunkCoord = toChunkCoord(p);
    int index = Chunk::index(p.x & kChunkMask, p.y & kChunkMask, p.z & kChunkMask);

    if (id == block::Air) {
        // Do not materialise a chunk just to write air into it.
        auto it = chunks_.find(chunkKey(chunkCoord));
        if (it == chunks_.end()) return;

        BlockId& slot = it->second.blocks[index];
        if (slot == block::Air) return;
        slot = block::Air;
        --it->second.nonAirCount;
        --blockCount_;
        // Bounds are deliberately not shrunk: recomputing them on every
        // removal would be quadratic, and a slightly loose bound only costs
        // the raycaster a few empty chunk steps.
        if (it->second.nonAirCount == 0) chunks_.erase(it);
        // After the erase: a chunk that is gone has no stamp to carry, and a
        // cache holding its mesh finds no chunk and drops the entry.
        touchAround(p);
        return;
    }

    Chunk& chunk = chunks_[chunkKey(chunkCoord)];
    chunk.coord = chunkCoord;
    BlockId& slot = chunk.blocks[index];

    // A write that changes nothing must not stamp anything. Scene building
    // paints the same block over a region often enough that stamping here
    // would invalidate every cache in the neighbourhood for no reason.
    if (slot == id) return;

    if (slot == block::Air) {
        ++chunk.nonAirCount;
        if (blockCount_ == 0) {
            minBlock_ = maxBlock_ = p;
        } else {
            minBlock_ = minv(minBlock_, p);
            maxBlock_ = maxv(maxBlock_, p);
        }
        ++blockCount_;
    }
    slot = id;
    touchAround(p);
}

void World::touchAround(IVec3 p) {
    // The chunks spanned by [p-1, p+1]: at most eight, and exactly one
    // whenever p is not within a block of a chunk boundary.
    IVec3 lo = toChunkCoord(p - IVec3{1});
    IVec3 hi = toChunkCoord(p + IVec3{1});

    for (int cz = lo.z; cz <= hi.z; ++cz) {
        for (int cy = lo.y; cy <= hi.y; ++cy) {
            for (int cx = lo.x; cx <= hi.x; ++cx) {
                auto it = chunks_.find(chunkKey({cx, cy, cz}));
                if (it != chunks_.end()) it->second.stamp = ++stamp_;
            }
        }
    }
}

void World::fillBox(IVec3 minCorner, IVec3 maxCorner, BlockId id) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                set({x, y, z}, id);
}

void World::fillHollowBox(IVec3 minCorner, IVec3 maxCorner, BlockId id, int thickness) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);
    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                bool onShell = (x - lo.x) < thickness || (hi.x - x) < thickness ||
                               (y - lo.y) < thickness || (hi.y - y) < thickness ||
                               (z - lo.z) < thickness || (hi.z - z) < thickness;
                if (onShell) set({x, y, z}, id);
            }
        }
    }
}

void World::fillSphere(IVec3 center, float radius, BlockId id) {
    int r = int(std::ceil(radius));
    float r2 = radius * radius;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                // Measure from block centres so the sphere stays symmetric.
                float d2 = float(dx * dx + dy * dy + dz * dz);
                if (d2 <= r2) set({center.x + dx, center.y + dy, center.z + dz}, id);
            }
        }
    }
}

void World::clear() {
    chunks_.clear();
    blockCount_ = 0;
    minBlock_ = maxBlock_ = IVec3{0, 0, 0};
    // stamp_ is deliberately left alone. It only ever has to be different
    // from what a cache last saw, and rewinding it is the one way to make
    // two different worlds produce the same value for the same chunk.
}

} // namespace blocky
