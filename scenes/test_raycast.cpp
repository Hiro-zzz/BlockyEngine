// Correctness tests for the voxel raycaster.
//
// The traversal is the single most load-bearing piece of the engine, and its
// failure mode is subtle: a ray silently skipping a chunk looks like a
// lighting artefact, not like a crash. These tests target that directly.
#include "engine/core/random.hpp"
#include "engine/world/raycast.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

bool nearly(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

// ------------------------------------------------------- axis-aligned faces
void testSingleBlockFaces() {
    std::printf("single block, all six faces\n");

    World world(palette::registry());
    world.set({0, 0, 0}, palette::Stone);  // spans [0,1) on every axis

    struct Case {
        Vec3 origin, direction;
        IVec3 expectedNormal;
        float expectedT;
        const char* name;
    };
    const Vec3 center{0.5f, 0.5f, 0.5f};
    const Case cases[] = {
        {center + Vec3{ 5, 0, 0}, {-1,  0,  0}, { 1,  0,  0}, 4.5f, "+X face"},
        {center + Vec3{-5, 0, 0}, { 1,  0,  0}, {-1,  0,  0}, 4.5f, "-X face"},
        {center + Vec3{0,  5, 0}, { 0, -1,  0}, { 0,  1,  0}, 4.5f, "+Y face"},
        {center + Vec3{0, -5, 0}, { 0,  1,  0}, { 0, -1,  0}, 4.5f, "-Y face"},
        {center + Vec3{0, 0,  5}, { 0,  0, -1}, { 0,  0,  1}, 4.5f, "+Z face"},
        {center + Vec3{0, 0, -5}, { 0,  0,  1}, { 0,  0, -1}, 4.5f, "-Z face"},
    };

    for (const Case& c : cases) {
        RayHit hit;
        bool got = raycast(world, {c.origin, c.direction}, 100.0f, hit);
        check(got, c.name);
        if (!got) continue;
        check(hit.block == IVec3{0, 0, 0}, c.name);
        check(hit.normal == c.expectedNormal, c.name);
        check(nearly(hit.t, c.expectedT), c.name);
        check(hit.id == palette::Stone, c.name);
    }

    // A ray aimed past the block must miss.
    RayHit miss;
    check(!raycast(world, {{5.0f, 5.0f, 0.5f}, {-1, 0, 0}}, 100.0f, miss), "ray above the block misses");
}

// -------------------------------------------------- no holes across chunks
// The regression test for the chunk-seam bug: a solid slab many chunks wide,
// with rays raining down on it. Every single one must land on the top face.
void testSlabHasNoHoles() {
    std::printf("solid slab spanning many chunks, 200k rays\n");

    World world(palette::registry());
    constexpr int kHalf = 80;   // 160 blocks across = 10 chunks
    constexpr int kTopY = 37;   // deliberately not on a chunk boundary
    world.fillBox({-kHalf, kTopY - 2, -kHalf}, {kHalf, kTopY, kHalf}, palette::Stone);

    Rng rng(12345);
    int misses = 0, wrongFace = 0, wrongHeight = 0;

    // A tilted ray drifts sideways on the way down, so the start region has
    // to be inset by more than the worst-case drift -- otherwise rays leave
    // the slab footprint and miss entirely, which is correct behaviour and
    // would make this test lie.
    constexpr float kDropHeight = 30.0f;
    constexpr float kMaxTilt = 0.35f;
    constexpr float kMaxDrift = kMaxTilt * (kDropHeight + 3.0f);  // ~11.6 blocks
    constexpr float kStartRange = float(kHalf) - kMaxDrift - 8.0f;

    for (int i = 0; i < 200000; ++i) {
        // Start above the slab, inside its footprint, and vary the direction
        // so rays cross chunk seams at every possible angle.
        float x = (rng.nextFloat() * 2.0f - 1.0f) * kStartRange;
        float z = (rng.nextFloat() * 2.0f - 1.0f) * kStartRange;
        Vec3 origin{x, float(kTopY) + kDropHeight, z};

        Vec3 direction = normalize(Vec3{
            (rng.nextFloat() * 2.0f - 1.0f) * kMaxTilt,
            -1.0f,
            (rng.nextFloat() * 2.0f - 1.0f) * kMaxTilt});

        RayHit hit;
        if (!raycast(world, {origin, direction}, 500.0f, hit)) { ++misses; continue; }
        if (hit.normal != IVec3{0, 1, 0}) ++wrongFace;
        if (hit.block.y != kTopY) ++wrongHeight;
    }

    check(misses == 0, "every ray hits the slab");
    check(wrongFace == 0, "every hit reports the top face");
    check(wrongHeight == 0, "every hit lands on the top layer");
    if (misses || wrongFace || wrongHeight) {
        std::printf("        misses=%d wrongFace=%d wrongHeight=%d\n", misses, wrongFace, wrongHeight);
    }
}

// ------------------------------------------- hits land exactly on the surface
void testHitsLieOnTheSurface() {
    std::printf("solid box, rays from every direction, 100k rays\n");

    World world(palette::registry());
    constexpr int kLo = -33, kHi = 30;  // straddles several chunk boundaries
    world.fillBox({kLo, kLo, kLo}, {kHi, kHi, kHi}, palette::Stone);

    const Vec3 boxLo = toVec3(IVec3{kLo, kLo, kLo});
    const Vec3 boxHi = toVec3(IVec3{kHi, kHi, kHi}) + Vec3{1.0f};
    const Vec3 center = (boxLo + boxHi) * 0.5f;

    Rng rng(777);
    int misses = 0, offSurface = 0, badNormal = 0;

    for (int i = 0; i < 100000; ++i) {
        // A random direction on the sphere, used to place the origin outside
        // the box and aim it back through the centre.
        float z = rng.nextFloat() * 2.0f - 1.0f;
        float phi = rng.nextFloat() * kTwoPi;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        Vec3 offset{r * std::cos(phi), r * std::sin(phi), z};

        Vec3 origin = center + offset * 140.0f;
        Vec3 aim = center + Vec3{
            (rng.nextFloat() * 2.0f - 1.0f) * 20.0f,
            (rng.nextFloat() * 2.0f - 1.0f) * 20.0f,
            (rng.nextFloat() * 2.0f - 1.0f) * 20.0f};

        RayHit hit;
        if (!raycast(world, {origin, normalize(aim - origin)}, 500.0f, hit)) { ++misses; continue; }

        // The hit point must sit on the plane of the face it reported.
        int axis = hit.axis;
        float plane = hit.normal[axis] > 0 ? float(hit.block[axis]) + 1.0f : float(hit.block[axis]);
        if (!nearly(hit.position[axis], plane, 2e-3f)) ++offSurface;

        // And that face must point back towards where the ray came from.
        Vec3 normal = toVec3(hit.normal);
        if (dot(normal, normalize(aim - origin)) > 0.0f) ++badNormal;
    }

    check(misses == 0, "every ray aimed at the box hits it");
    check(offSurface == 0, "every hit point lies on the reported face plane");
    check(badNormal == 0, "every normal faces the incoming ray");
    if (misses || offSurface || badNormal) {
        std::printf("        misses=%d offSurface=%d badNormal=%d\n", misses, offSurface, badNormal);
    }
}

// -------------------------------------------------- transparency separation
void testOpaqueOnlyQuery() {
    std::printf("shadow rays ignore non-opaque blocks\n");

    World world(palette::registry());
    world.fillBox({0, 0, 0}, {0, 0, 0}, palette::Glass);
    world.fillBox({6, 0, 0}, {6, 0, 0}, palette::Stone);

    Ray ray{{-5.0f, 0.5f, 0.5f}, {1, 0, 0}};

    RayHit hit;
    check(raycast(world, ray, 100.0f, hit), "camera ray sees the glass");
    check(hit.block == IVec3{0, 0, 0}, "camera ray stops at the glass");

    check(raycastOccluded(world, ray, 100.0f), "shadow ray reaches the stone behind");

    World glassOnly(palette::registry());
    glassOnly.set({0, 0, 0}, palette::Glass);
    check(!raycastOccluded(glassOnly, ray, 100.0f), "glass alone casts no shadow");
}

// ---------------------------------------------------------- sparse skipping
void testDistantBlockAcrossEmptySpace() {
    std::printf("single block 4000 units away across empty space\n");

    World world(palette::registry());
    world.set({4000, 0, 0}, palette::Stone);

    RayHit hit;
    bool got = raycast(world, {{-5.0f, 0.5f, 0.5f}, {1, 0, 0}}, 10000.0f, hit);
    check(got, "distant block is found");
    if (got) {
        check(hit.block == IVec3{4000, 0, 0}, "distant block coordinate is exact");
        check(nearly(hit.t, 4005.0f, 0.05f), "distant block distance is exact");
    }

    // maxDistance must be respected.
    RayHit clipped;
    check(!raycast(world, {{-5.0f, 0.5f, 0.5f}, {1, 0, 0}}, 100.0f, clipped),
          "maxDistance stops the ray short");
}

// -------------------------------------------------------------- world edits
void testWorldBookkeeping() {
    std::printf("world storage bookkeeping\n");

    World world(palette::registry());
    check(!world.hasBlocks(), "a new world is empty");

    world.fillBox({0, 0, 0}, {9, 9, 9}, palette::Stone);
    check(world.blockCount() == 1000, "fillBox writes 10x10x10 blocks");
    check(world.minBlock() == IVec3{0, 0, 0}, "min bound");
    check(world.maxBlock() == IVec3{9, 9, 9}, "max bound");

    world.set({5, 5, 5}, palette::Air);
    check(world.blockCount() == 999, "clearing a block decrements the count");
    check(world.get({5, 5, 5}) == palette::Air, "cleared block reads back as air");

    world.set({5, 5, 5}, palette::Dirt);
    check(world.blockCount() == 1000, "rewriting restores the count");
    check(world.get({5, 5, 5}) == palette::Dirt, "rewritten block keeps its id");

    // Overwriting an existing block must not double-count it.
    world.set({5, 5, 5}, palette::Sand);
    check(world.blockCount() == 1000, "overwriting does not change the count");

    // Negative coordinates must map to the correct chunk.
    World negative(palette::registry());
    negative.set({-1, -1, -1}, palette::Stone);
    check(negative.get({-1, -1, -1}) == palette::Stone, "negative coordinates round-trip");
    check(negative.get({0, 0, 0}) == palette::Air, "neighbouring block stays empty");
    check(World::toChunkCoord({-1, -1, -1}) == IVec3{-1, -1, -1}, "negative chunk coordinate floors");
    check(World::toChunkCoord({-16, -16, -16}) == IVec3{-1, -1, -1}, "chunk boundary floors");
    check(World::toChunkCoord({-17, 0, 0}) == IVec3{-2, 0, 0}, "past the boundary floors again");
}

// -------------------------------------------------------------- attenuation
void testTransmittance() {
    std::printf("Beer-Lambert transmittance through media\n");

    const BlockDef& water = palette::registry()[palette::Water];

    // Empty world along the ray: nothing absorbs.
    {
        World world(palette::registry());
        world.set({50, 0, 0}, palette::Stone);  // off to one side, so the world is non-empty
        Vec3 t = rayTransmittance(world, {{0.5f, 20.0f, 0.5f}, {0, -1, 0}}, 100.0f);
        check(nearly(t.x, 1.0f) && nearly(t.y, 1.0f) && nearly(t.z, 1.0f), "clear path transmits fully");
    }

    // An opaque block blocks completely.
    {
        World world(palette::registry());
        world.set({0, 5, 0}, palette::Stone);
        Vec3 t = rayTransmittance(world, {{0.5f, 20.0f, 0.5f}, {0, -1, 0}}, 100.0f);
        check(t.x == 0.0f && t.y == 0.0f && t.z == 0.0f, "stone blocks all light");
    }

    // A column of water attenuates by exp(-absorption * depth), exactly.
    {
        constexpr int kDepth = 10;
        World world(palette::registry());
        world.fillBox({0, 0, 0}, {0, kDepth - 1, 0}, palette::Water);

        Vec3 t = rayTransmittance(world, {{0.5f, 40.0f, 0.5f}, {0, -1, 0}}, 200.0f);
        Vec3 expected{std::exp(-water.absorption.x * float(kDepth)),
                      std::exp(-water.absorption.y * float(kDepth)),
                      std::exp(-water.absorption.z * float(kDepth))};

        check(nearly(t.x, expected.x, 1e-4f), "red attenuates by the exact Beer-Lambert factor");
        check(nearly(t.y, expected.y, 1e-4f), "green attenuates by the exact Beer-Lambert factor");
        check(nearly(t.z, expected.z, 1e-4f), "blue attenuates by the exact Beer-Lambert factor");
        check(t.x < t.y && t.y < t.z, "water absorbs red fastest and blue slowest");
    }

    // Water spanning many chunks must not lose or double-count any segment:
    // the same total depth has to give the same answer whichever way it is
    // laid out relative to chunk boundaries.
    {
        constexpr int kDepth = 40;  // crosses at least two chunk seams
        World a(palette::registry()), b(palette::registry());
        a.fillBox({0, 0, 0}, {0, kDepth - 1, 0}, palette::Water);
        b.fillBox({0, 7, 0}, {0, 7 + kDepth - 1, 0}, palette::Water);  // shifted off the seam

        Vec3 ta = rayTransmittance(a, {{0.5f, 90.0f, 0.5f}, {0, -1, 0}}, 400.0f);
        Vec3 tb = rayTransmittance(b, {{0.5f, 90.0f, 0.5f}, {0, -1, 0}}, 400.0f);
        check(nearly(ta.z, tb.z, 1e-5f), "depth is independent of chunk alignment");

        float expectedBlue = std::exp(-water.absorption.z * float(kDepth));
        check(nearly(ta.z, expectedBlue, 1e-4f), "40 blocks of water attenuate exactly");
    }

    // A slanted ray travels further through the same slab, so it must be
    // attenuated more -- this is what a low sun does to underwater light.
    {
        World world(palette::registry());
        world.fillBox({-40, 0, -40}, {40, 9, 40}, palette::Water);

        Vec3 straight = rayTransmittance(world, {{0.5f, 40.0f, 0.5f}, {0, -1, 0}}, 200.0f);
        Vec3 slanted = rayTransmittance(
            world, {{0.5f, 40.0f, 0.5f}, normalize(Vec3{0.6f, -1.0f, 0.0f})}, 200.0f);
        check(slanted.z < straight.z, "a slanted path through water absorbs more");
    }
}

} // namespace

int main() {
    testWorldBookkeeping();
    testSingleBlockFaces();
    testSlabHasNoHoles();
    testHitsLieOnTheSurface();
    testOpaqueOnlyQuery();
    testDistantBlockAcrossEmptySpace();
    testTransmittance();

    if (gFailures == 0) {
        std::printf("\nall raycast tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
