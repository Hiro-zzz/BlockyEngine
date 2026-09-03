#pragma once
// Per-chunk meshes that survive between frames, rebuilt only where the world
// changed.
//
// The offline half of this engine meshes the whole world once, before the
// viewport's loop starts, and that is right for what it does: `buildWorldMesh`
// costs a fraction of a render that runs for seconds, and nothing moves
// afterwards. A sandbox breaks both halves of that -- blocks change while the
// frame budget is sixteen milliseconds -- so the work has to become
// proportional to what changed rather than to how much world there is.
//
// Nothing here is pushed at. `World` stamps chunks it touches with a global
// counter and this keeps the stamp it last meshed at; the two never coordinate,
// which is what lets a physics or lighting cache sit alongside without any of
// them knowing the others exist.
#include "engine/render/gl/gl_resources.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <unordered_map>

namespace blocky {

class ChunkMeshCache {
public:
    struct Stats {
        int remeshed = 0;      // chunks rebuilt by this call
        int dropped = 0;       // cached meshes released because the chunk is gone
        int stillStale = 0;    // stale chunks the budget did not reach
        int cached = 0;        // chunks held after the call
        double meshSeconds = 0.0;    // CPU face walking
        double uploadSeconds = 0.0;  // buffer upload
    };

    // Brings the cache up to date with the world, rebuilding at most `budget`
    // chunks -- zero means no limit. Chunks left over stay stale and come back
    // on the next call, which is what keeps a large edit from spiking a frame.
    //
    // Which stale chunks a limited budget picks is currently unspecified: it
    // follows hash order. A game wants nearest-to-camera first, and that is a
    // sort at this seam rather than a change to anything below it.
    Stats update(const World& world, int budget = 0);

    // Two passes, because the translucent one is drawn after with blending on
    // and depth writes off. Same split as MeshData's two outputs.
    void drawOpaque() const;
    void drawTranslucent() const;

    void clear();

    size_t chunkCount() const { return entries_.size(); }
    size_t triangleCount() const;

private:
    struct Entry {
        GpuMesh opaque, translucent;
        uint64_t stamp = 0;   // world stamp this mesh was built from
        int triangles = 0;
        bool present = false; // cleared each update, set by the world walk
    };

    std::unordered_map<uint64_t, Entry> entries_;

    // One scratch pair reused for every chunk, so a steady state does no
    // allocation at all -- the vectors keep their capacity between chunks.
    MeshData scratchOpaque_, scratchTranslucent_;
};

} // namespace blocky
