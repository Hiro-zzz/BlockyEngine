// Tests for per-chunk staleness: the contract that lets a mesh be rebuilt
// only where the world changed.
//
// The interesting test is not "does the stamp move when I edit a block" --
// that one is nearly a tautology. It is the **superset property**:
//
//     if a chunk's mesh would come out different, its stamp must have moved.
//
// The other direction is allowed to be loose. Restamping a chunk whose mesh
// turns out identical costs one wasted rebuild; failing to restamp one whose
// mesh changed leaves a wrong picture on screen until something else happens
// to touch it, and the wrongness is usually a shading seam rather than a hole,
// which is the kind that survives for weeks.
//
// This is exactly the trap the neighbour rule exists for. Face culling reads
// the six neighbours, so a naive implementation dirties those and looks
// correct -- but vertex AO reads the eight blocks around each corner, so a
// block changed at a chunk corner alters shading in a chunk it does not even
// share a face with. Restricting the rule to face neighbours makes this test
// fail, which is the whole reason it is written this way round.
//
// Needs no game files and no GL context: meshing produces CPU-side MeshData,
// and only uploading it needs a window.
#include "engine/core/random.hpp"
#include "engine/render/gl/gl_resources.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

bool sameMesh(const MeshData& a, const MeshData& b) {
    if (a.vertices.size() != b.vertices.size()) return false;
    if (a.indices.size() != b.indices.size()) return false;
    if (!a.vertices.empty() &&
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(BlockVertex)) != 0)
        return false;
    if (!a.indices.empty() &&
        std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(uint32_t)) != 0)
        return false;
    return true;
}

struct ChunkSnapshot {
    MeshData opaque;
    MeshData translucent;
    uint64_t stamp = 0;
};

void snapshot(const World& world, std::map<uint64_t, ChunkSnapshot>& out) {
    out.clear();
    world.forEachChunk([&](IVec3 coord, const World::Chunk& chunk) {
        ChunkSnapshot& entry = out[World::chunkKey(coord)];
        entry.stamp = chunk.stamp;
        appendChunkMesh(world, chunk, entry.opaque, entry.translucent);
    });
}

void buildTerrain(World& world) {
    // Two chunks across in x and z, two tall -- enough that edits can land on
    // a face, an edge and a corner between chunks.
    world.fillBox({0, 0, 0}, {31, 16, 31}, palette::Stone);
    world.fillBox({0, 17, 0}, {31, 17, 31}, palette::GrassBlock);

    // A few holes, so there are interior faces whose AO depends on diagonals.
    world.fillBox({6, 14, 6}, {9, 17, 9}, palette::Air);
    world.fillBox({20, 15, 12}, {22, 17, 14}, palette::Air);
}

// ------------------------------------------------------------ stamp basics
void testStampBasics() {
    std::printf("stamp basics\n");

    World world(palette::registry());
    check(world.chunkStamp({0, 0, 0}) == 0, "an absent chunk stamps zero");

    world.set({4, 4, 4}, palette::Stone);
    uint64_t first = world.chunkStamp({0, 0, 0});
    check(first != 0, "creating a block stamps its chunk");

    world.set({4, 4, 4}, palette::Stone);
    check(world.chunkStamp({0, 0, 0}) == first, "rewriting the same block stamps nothing");

    world.set({4, 4, 4}, palette::Dirt);
    check(world.chunkStamp({0, 0, 0}) != first, "changing it to another block does stamp");

    // A block one step inside the boundary cannot affect the neighbour, and a
    // block on it must.
    World two(palette::registry());
    two.set({8, 8, 8}, palette::Stone);      // middle of chunk (0,0,0)
    two.set({20, 8, 8}, palette::Stone);     // chunk (1,0,0)
    uint64_t farStamp = two.chunkStamp({1, 0, 0});

    two.set({4, 4, 4}, palette::Stone);
    check(two.chunkStamp({1, 0, 0}) == farStamp, "an interior edit leaves the neighbour alone");

    two.set({15, 8, 8}, palette::Stone);     // last column of chunk (0,0,0)
    check(two.chunkStamp({1, 0, 0}) != farStamp, "an edit on the boundary stamps the neighbour");

    // Removing the last block of a chunk erases it entirely.
    World lone(palette::registry());
    lone.set({40, 40, 40}, palette::Stone);
    check(lone.chunkStamp({2, 2, 2}) != 0, "the chunk exists");
    lone.set({40, 40, 40}, palette::Air);
    check(lone.chunkStamp({2, 2, 2}) == 0, "and is gone once emptied");
    check(lone.chunkCount() == 0, "leaving no chunks behind");
}

// -------------------------------------------------------- the real property
void testStampCoversEveryVisualChange() {
    std::printf("stamp covers every visual change\n");

    // Positions chosen to hit every relation an edit can have with a chunk
    // boundary: deep inside, on a face, on an edge, and on a corner.
    std::vector<IVec3> sites = {
        {8, 8, 8},     {15, 8, 8},    {16, 8, 8},    {15, 15, 8},
        {15, 8, 15},   {15, 15, 15},  {16, 16, 16},  {0, 8, 8},
        {8, 15, 8},    {8, 16, 8},    {31, 17, 31},  {7, 15, 9},
    };

    Rng rng(20260901u);
    for (int i = 0; i < 48; ++i) {
        sites.push_back({int(rng.nextFloat() * 32.0f), int(rng.nextFloat() * 18.0f),
                         int(rng.nextFloat() * 32.0f)});
    }

    int missedStamps = 0;
    int changedMeshes = 0;
    int wastedStamps = 0;
    int totalStamped = 0;

    for (IVec3 site : sites) {
        for (int variant = 0; variant < 2; ++variant) {
            World world(palette::registry());
            buildTerrain(world);

            std::map<uint64_t, ChunkSnapshot> before;
            snapshot(world, before);

            // Once placing a block, once removing one: the two dirty
            // different neighbours when the site sits against a boundary.
            world.set(site, variant == 0 ? palette::Cobblestone : palette::Air);

            std::map<uint64_t, ChunkSnapshot> after;
            snapshot(world, after);

            for (const auto& entry : after) {
                auto previous = before.find(entry.first);
                if (previous == before.end()) continue;   // new chunk: nothing cached

                bool meshChanged = !sameMesh(previous->second.opaque, entry.second.opaque) ||
                                   !sameMesh(previous->second.translucent, entry.second.translucent);
                bool stampChanged = previous->second.stamp != entry.second.stamp;

                if (stampChanged) ++totalStamped;
                if (meshChanged) ++changedMeshes;
                if (meshChanged && !stampChanged) ++missedStamps;
                if (stampChanged && !meshChanged) ++wastedStamps;
            }
        }
    }

    std::printf("  %d chunk meshes changed over %zu edits\n", changedMeshes, sites.size() * 2);
    check(changedMeshes > 0, "the edits changed something at all");
    check(missedStamps == 0, "every changed mesh had its stamp moved");
    if (missedStamps != 0)
        std::printf("  %d chunk(s) changed appearance without being stamped\n", missedStamps);

    // Not a failure: the price of the rule being conservative. Worth printing
    // because a number that climbs means the rule got sloppier.
    std::printf("  %d of %d stamps were redundant (%.0f%%)\n", wastedStamps, totalStamped,
                totalStamped ? 100.0 * double(wastedStamps) / double(totalStamped) : 0.0);
}

// ------------------------------------------------ incremental equals rebuild
void testIncrementalEqualsRebuild() {
    std::printf("incremental meshing equals a full rebuild\n");

    World world(palette::registry());
    buildTerrain(world);

    // The cache a renderer would keep, minus the GPU: chunk key -> mesh plus
    // the stamp it was built from.
    std::map<uint64_t, ChunkSnapshot> cache;
    snapshot(world, cache);

    Rng rng(770u);
    int rebuilt = 0;

    for (int edit = 0; edit < 200; ++edit) {
        IVec3 site{int(rng.nextFloat() * 32.0f), int(rng.nextFloat() * 18.0f),
                   int(rng.nextFloat() * 32.0f)};
        world.set(site, (edit % 3 == 0) ? palette::Air : palette::OakPlanks);

        // Update only what the stamps say is stale, exactly as ChunkMeshCache
        // does, then drop entries whose chunk is gone.
        std::vector<uint64_t> present;
        world.forEachChunk([&](IVec3 coord, const World::Chunk& chunk) {
            uint64_t key = World::chunkKey(coord);
            present.push_back(key);

            ChunkSnapshot& entry = cache[key];
            if (entry.stamp == chunk.stamp) return;

            entry.opaque.clear();
            entry.translucent.clear();
            appendChunkMesh(world, chunk, entry.opaque, entry.translucent);
            entry.stamp = chunk.stamp;
            ++rebuilt;
        });

        for (auto it = cache.begin(); it != cache.end();) {
            bool alive = false;
            for (uint64_t key : present) alive = alive || key == it->first;
            it = alive ? std::next(it) : cache.erase(it);
        }
    }

    std::map<uint64_t, ChunkSnapshot> fresh;
    snapshot(world, fresh);

    check(cache.size() == fresh.size(), "the cache holds exactly the live chunks");

    int mismatches = 0;
    for (const auto& entry : fresh) {
        auto cached = cache.find(entry.first);
        if (cached == cache.end()) { ++mismatches; continue; }
        if (!sameMesh(cached->second.opaque, entry.second.opaque)) ++mismatches;
        if (!sameMesh(cached->second.translucent, entry.second.translucent)) ++mismatches;
    }

    check(mismatches == 0, "200 incremental edits leave the same meshes as a rebuild");
    std::printf("  %d chunk rebuilds for 200 edits across %zu chunks\n", rebuilt, fresh.size());
}

} // namespace

int main() {
    testStampBasics();
    testStampCoversEveryVisualChange();
    testIncrementalEqualsRebuild();

    if (gFailures == 0) {
        std::printf("\nall chunk tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
