#include "engine/render/gl/chunk_cache.hpp"

#include <chrono>

namespace blocky {
namespace {

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

ChunkMeshCache::Stats ChunkMeshCache::update(const World& world, int budget) {
    Stats stats;

    for (auto& entry : entries_) entry.second.present = false;

    world.forEachChunk([&](IVec3 coord, const World::Chunk& chunk) {
        uint64_t key = World::chunkKey(coord);
        Entry& entry = entries_[key];
        entry.present = true;

        // A stamp of zero means "never meshed", and the world's counter starts
        // at one, so a fresh entry always disagrees.
        if (entry.stamp == chunk.stamp) return;

        if (budget > 0 && stats.remeshed >= budget) {
            ++stats.stillStale;
            return;
        }

        auto meshStart = std::chrono::steady_clock::now();
        scratchOpaque_.clear();
        scratchTranslucent_.clear();
        appendChunkMesh(world, chunk, scratchOpaque_, scratchTranslucent_);
        stats.meshSeconds += secondsSince(meshStart);

        auto uploadStart = std::chrono::steady_clock::now();
        entry.opaque.upload(scratchOpaque_);
        entry.translucent.upload(scratchTranslucent_);
        stats.uploadSeconds += secondsSince(uploadStart);

        entry.stamp = chunk.stamp;
        entry.triangles = int((scratchOpaque_.indices.size() + scratchTranslucent_.indices.size()) / 3);
        ++stats.remeshed;
    });

    // Whatever the walk did not reach no longer exists: a chunk is erased when
    // its last block goes, so its mesh has to go with it. Doing this by
    // absence rather than by notification is the same choice as the stamp --
    // the world does not know who is caching what.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.present) {
            ++it;
        } else {
            it = entries_.erase(it);
            ++stats.dropped;
        }
    }

    stats.cached = int(entries_.size());
    return stats;
}

void ChunkMeshCache::drawOpaque() const {
    for (const auto& entry : entries_) entry.second.opaque.draw();
}

void ChunkMeshCache::drawTranslucent() const {
    for (const auto& entry : entries_) entry.second.translucent.draw();
}

void ChunkMeshCache::clear() {
    entries_.clear();
    scratchOpaque_.clear();
    scratchTranslucent_.clear();
}

size_t ChunkMeshCache::triangleCount() const {
    size_t total = 0;
    for (const auto& entry : entries_) total += size_t(entry.second.triangles);
    return total;
}

} // namespace blocky
