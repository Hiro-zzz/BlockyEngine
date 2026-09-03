#pragma once
// Sparse voxel storage: a hash map of dense 16x16x16 chunks.
//
// Empty regions cost nothing, and the chunk grid doubles as the acceleration
// structure -- the raycaster skips a whole 16-block cell whenever no chunk is
// present, which is most of the volume in a typical scene.
#include "engine/core/math.hpp"
#include "engine/world/block.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blocky {

class World {
public:
    static constexpr int kChunkBits = 4;
    static constexpr int kChunkSize = 1 << kChunkBits;   // 16
    static constexpr int kChunkMask = kChunkSize - 1;
    static constexpr int kChunkVolume = kChunkSize * kChunkSize * kChunkSize;

    struct Chunk {
        BlockId blocks[kChunkVolume]{};
        uint32_t nonAirCount = 0;
        IVec3 coord{};  // set on creation, so a chunk can be walked standalone

        // Value of the world's counter when this chunk last changed. Anything
        // caching per-chunk work -- a mesh, a collision summary, a light list
        // -- keeps the stamp it last saw and compares. See chunkStamp().
        uint64_t stamp = 0;

        static int index(int lx, int ly, int lz) {
            return (ly << (2 * kChunkBits)) | (lz << kChunkBits) | lx;
        }
    };

    // World position -> chunk coordinate. Arithmetic shift floors correctly
    // for negative coordinates, which plain division would not.
    static IVec3 toChunkCoord(IVec3 p) {
        return {p.x >> kChunkBits, p.y >> kChunkBits, p.z >> kChunkBits};
    }

    // A unique 64-bit id for a chunk coordinate. Public because anything
    // keeping a per-chunk cache needs to key it the same way, and a second
    // encoding invented next door is a second thing that can disagree.
    static uint64_t chunkKey(IVec3 c) {
        // 21 bits per axis, biased to keep negatives ordered. Covers a world
        // spanning about +/-1 million chunks, far past anything we will build.
        auto part = [](int32_t v) { return uint64_t(uint32_t(v + (1 << 20)) & 0x1FFFFFu); };
        return part(c.x) | (part(c.y) << 21) | (part(c.z) << 42);
    }

    // A world is built against a palette, and there is no default one to fall
    // back on: the engine ships no blocks, so "which blocks" is a question
    // only the caller can answer. Held by pointer so a world stays copyable,
    // taken by reference so it cannot be null.
    //
    // The palette must outlive the world, and normally does by a mile -- both
    // `palette::registry()` and `game::palette()` are built once and never die.
    explicit World(BlockRegistry& registry) : registry_(&registry) {}

    const BlockRegistry& registry() const { return *registry_; }

    BlockId get(IVec3 p) const;
    void    set(IVec3 p, BlockId id);

    bool isOpaque(IVec3 p) const { return (*registry_)[get(p)].opaque; }

    // What physics asks of a cell. Both go through the palette the world was
    // built with, so a caller that registered its own blocks gets the answer
    // it wrote down rather than the one a built-in id would have implied.
    bool collides(IVec3 p) const { return (*registry_)[get(p)].solid; }
    bool isFluid(IVec3 p) const  { return (*registry_)[get(p)].fluid; }

    // ------------------------------------------------------------- building
    void fillBox(IVec3 minCorner, IVec3 maxCorner, BlockId id);      // inclusive
    void fillSphere(IVec3 center, float radius, BlockId id);
    void fillHollowBox(IVec3 minCorner, IVec3 maxCorner, BlockId id, int thickness = 1);
    void clear();

    // --------------------------------------------------------------- extent
    // Inclusive bounds over every non-air block. Valid only when non-empty.
    bool  hasBlocks() const { return blockCount_ > 0; }
    IVec3 minBlock() const { return minBlock_; }
    IVec3 maxBlock() const { return maxBlock_; }
    uint64_t blockCount() const { return blockCount_; }
    size_t chunkCount() const { return chunks_.size(); }

    // ------------------------------------------------------ raycaster access
    const Chunk* findChunk(IVec3 chunkCoord) const {
        auto it = chunks_.find(chunkKey(chunkCoord));
        return it == chunks_.end() ? nullptr : &it->second;
    }

    // Visit every populated chunk. Used to gather emissive faces into a light
    // list without exposing the storage layout.
    template <class Fn>
    void forEachChunk(Fn&& fn) const {
        for (const auto& entry : chunks_) fn(entry.second.coord, entry.second);
    }

    // ------------------------------------------------------------- staleness
    // When this chunk last changed, or 0 if there is no chunk there.
    //
    // Deliberately a pull-based stamp rather than a push-based dirty set. A
    // dirty set has to know when it may be cleared, which means knowing how
    // many consumers there are; here the mesher, a collision cache and a
    // light cache each keep their own last-seen value and never coordinate.
    //
    // The counter is global and monotonic rather than per-chunk, because a
    // chunk emptied of its last block is erased -- a per-chunk counter would
    // restart at zero when it came back, and a cache holding a stale mesh
    // stamped zero would call itself current.
    uint64_t chunkStamp(IVec3 chunkCoord) const {
        const Chunk* chunk = findChunk(chunkCoord);
        return chunk ? chunk->stamp : 0;
    }

    uint64_t stamp() const { return stamp_; }

private:
    // Stamps every chunk that a change at `p` could alter the appearance of.
    // That is not just the chunk holding `p`: face culling reads the six
    // neighbours and vertex AO reads the eight blocks around each corner, so
    // a block on a chunk edge changes what its neighbour should draw. Marking
    // only the owning chunk leaves seams -- lit wrongly, not holed, which is
    // the kind of error that survives a long time.
    void touchAround(IVec3 p);

    BlockRegistry* registry_;
    std::unordered_map<uint64_t, Chunk> chunks_;

    IVec3 minBlock_{0, 0, 0};
    IVec3 maxBlock_{0, 0, 0};
    uint64_t blockCount_ = 0;
    uint64_t stamp_ = 0;
};

} // namespace blocky
