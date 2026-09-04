// Tests for voxel items and props: the grid walk, the transform, item
// extrusion, capture from a world, and hand-drawn layers.
//
// The grid DDA gets the same treatment the BVH got, and for the same reason.
// A traversal can be wrong in ways that still produce a picture -- a stepping
// bug eats a plane of voxels and the sword just looks a bit thin. So it is
// checked against an exhaustive march: walk the ray in tiny steps, note the
// first solid cell, and demand the DDA agree, over thousands of rays.
//
// Needs no game files: every model here is built in code.
#include "engine/prop/item.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/core/random.hpp"
#include "engine/render/trace/intersect.hpp"
#include "engine/world/shapes.hpp"
#include "scenes/common/palette.hpp"

#include <cstdio>
#include <string>
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

bool nearly(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

VoxelMaterial colour(float r, float g, float b) {
    VoxelMaterial material;
    material.albedo = {r, g, b};
    return material;
}

// ---------------------------------------------------------------- the grid
void testModel() {
    std::printf("voxel model\n");

    VoxelModel model;
    check(model.empty(), "a fresh model is empty");
    check(model.at({0, 0, 0}) == VoxelModel::kEmpty, "and reads empty out of range");

    model.resize({4, 3, 2});
    check(model.dims() == IVec3(4, 3, 2), "resize sets the dimensions");
    check(model.empty(), "and leaves it empty");

    uint16_t red = model.addMaterial(colour(1.0f, 0.0f, 0.0f));
    uint16_t green = model.addMaterial(colour(0.0f, 1.0f, 0.0f));
    check(red != VoxelModel::kEmpty, "a material is never the empty slot");
    check(red != green, "distinct materials get distinct slots");
    check(model.addMaterial(colour(1.0f, 0.0f, 0.0f)) == red, "identical materials are folded");
    check(model.materialCount() == 2, "and the palette holds only the two");

    model.set({1, 1, 1}, red);
    check(model.solidCount() == 1, "setting a voxel counts it");
    check(model.at({1, 1, 1}) == red, "and it reads back");
    check(nearly(model.material(model.at({1, 1, 1})).albedo.x, 1.0f, 1e-5f),
          "with the right material");

    model.set({1, 1, 1}, green);
    check(model.solidCount() == 1, "overwriting does not double-count");

    model.set({1, 1, 1}, VoxelModel::kEmpty);
    check(model.solidCount() == 0, "clearing decrements");

    // Out of range writes are ignored rather than corrupting anything.
    model.set({-1, 0, 0}, red);
    model.set({9, 9, 9}, red);
    check(model.solidCount() == 0, "out-of-range writes are dropped");

    {   // trim shrinks to the occupied box and keeps the contents.
        VoxelModel big;
        big.resize({16, 16, 16});
        uint16_t slot = big.addMaterial(colour(0.5f, 0.5f, 0.5f));
        big.set({5, 6, 7}, slot);
        big.set({6, 7, 8}, slot);
        big.trim();

        check(big.dims() == IVec3(2, 2, 2), "trim shrinks to the occupied box");
        check(big.solidCount() == 2, "keeping every solid voxel");
        check(big.at({0, 0, 0}) == slot && big.at({1, 1, 1}) == slot, "moved to the corner");
    }
}

// Marches in small steps and reports the first solid cell it lands in.
//
// This can only ever prove the DDA *skipped* something. It cannot prove the
// DDA wrong for finding a voxel the march did not: a ray clipping the corner
// of a cell can be inside it for less than one step, and then the march walks
// straight over a voxel that is genuinely there. That is not hypothetical --
// it turned up on the first run, one ray in six thousand, crossing two cell
// boundaries 0.0009 apart.
//
// So the march is only half the check. The other half is exact.
bool marchReference(const VoxelModel& model, Vec3 origin, Vec3 direction, float tMax,
                    float& tHit, IVec3& cell) {
    const float step = 0.002f;
    for (float t = step; t <= tMax; t += step) {
        IVec3 v = floorToInt(origin + direction * t);
        if (!model.inside(v)) continue;
        if (model.at(v) == VoxelModel::kEmpty) continue;
        tHit = t;
        cell = v;
        return true;
    }
    return false;
}

// Exact: where does the ray enter this one cell's box, if at all? No sampling,
// so no sliver is too thin for it. This is what verifies a DDA hit is real.
bool cellEntry(Vec3 origin, Vec3 direction, IVec3 cell, float& tEnter) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float inv = 1.0f / direction[a];
        float near = (float(cell[a]) - origin[a]) * inv;
        float far = (float(cell[a] + 1) - origin[a]) * inv;
        if (inv < 0.0f) { float tmp = near; near = far; far = tmp; }
        if (near > t0) t0 = near;
        if (far < t1) t1 = far;
        if (t0 > t1) return false;
    }
    tEnter = t0;
    return true;
}

void testTraversal() {
    std::printf("grid traversal\n");

    // A shape with holes in it, so a stepping bug has somewhere to show.
    VoxelModel model;
    model.resize({8, 8, 8});
    uint16_t slot = model.addMaterial(colour(0.6f, 0.6f, 0.6f));

    Rng fill(4242u);
    for (int y = 0; y < 8; ++y) {
        for (int z = 0; z < 8; ++z) {
            for (int x = 0; x < 8; ++x) {
                if (fill.nextFloat() < 0.25f) model.set({x, y, z}, slot);
            }
        }
    }
    check(model.solidCount() > 60, "the test model has enough in it");

    {   // Faces of a single voxel, one at a time.
        VoxelModel one;
        one.resize({1, 1, 1});
        one.set({0, 0, 0}, one.addMaterial(colour(1.0f, 1.0f, 1.0f)));

        struct Probe { Vec3 origin, direction, normal; };
        const Probe probes[] = {
            {{-5.0f, 0.5f, 0.5f}, {1, 0, 0}, {-1, 0, 0}},
            {{6.0f, 0.5f, 0.5f}, {-1, 0, 0}, {1, 0, 0}},
            {{0.5f, -5.0f, 0.5f}, {0, 1, 0}, {0, -1, 0}},
            {{0.5f, 6.0f, 0.5f}, {0, -1, 0}, {0, 1, 0}},
            {{0.5f, 0.5f, -5.0f}, {0, 0, 1}, {0, 0, -1}},
            {{0.5f, 0.5f, 6.0f}, {0, 0, -1}, {0, 0, 1}},
        };

        for (const Probe& probe : probes) {
            VoxelHit hit;
            bool got = one.trace(probe.origin, probe.direction, 1e-4f, 100.0f, hit);
            check(got, "each face of a lone voxel is hit");
            if (!got) continue;
            check(nearly(hit.normal.x, probe.normal.x, 1e-4f) &&
                      nearly(hit.normal.y, probe.normal.y, 1e-4f) &&
                      nearly(hit.normal.z, probe.normal.z, 1e-4f),
                  "with the outward normal of that face");
        }

        // A ray that goes past it finds nothing.
        VoxelHit miss;
        check(!one.trace({-5.0f, 3.0f, 0.5f}, {1, 0, 0}, 1e-4f, 100.0f, miss),
              "a ray above the voxel misses");
        // And one that stops short of it.
        check(!one.trace({-5.0f, 0.5f, 0.5f}, {1, 0, 0}, 1e-4f, 2.0f, miss),
              "maxDistance is respected");
    }

    // The heart of it, in two halves that cover each other's blind spot:
    //   - every hit the DDA reports is verified exactly, against the cell's
    //     own box, so it cannot invent one or misplace `t`;
    //   - the march then proves the DDA did not walk past anything nearer.
    Rng rng(9090u);
    int wrongHit = 0, skipped = 0;
    const int kRays = 6000;

    for (int i = 0; i < kRays; ++i) {
        Vec3 origin{rng.nextFloat() * 24.0f - 8.0f, rng.nextFloat() * 24.0f - 8.0f,
                    rng.nextFloat() * 24.0f - 8.0f};
        Vec3 direction = normalize(Vec3{rng.nextFloat() * 2.0f - 1.0f,
                                        rng.nextFloat() * 2.0f - 1.0f,
                                        rng.nextFloat() * 2.0f - 1.0f});
        if (lengthSq(direction) < 0.5f) continue;

        VoxelHit hit;
        bool ddaHit = model.trace(origin, direction, 1e-4f, 60.0f, hit);

        if (ddaHit) {
            float tEnter = 0.0f;
            bool real = model.inside(hit.voxel) && model.at(hit.voxel) != VoxelModel::kEmpty &&
                        cellEntry(origin, direction, hit.voxel, tEnter) &&
                        nearly(tEnter, hit.t, 1e-3f);
            if (!real) {
                if (wrongHit == 0) {
                    std::printf("        bogus hit: origin %.6f %.6f %.6f\n", origin.x, origin.y,
                                origin.z);
                    std::printf("                   dir    %.6f %.6f %.6f  t=%.6f cell %d %d %d\n",
                                direction.x, direction.y, direction.z, hit.t, hit.voxel.x,
                                hit.voxel.y, hit.voxel.z);
                }
                ++wrongHit;
            }
        }

        float refT = 0.0f;
        IVec3 refCell{};
        if (!marchReference(model, origin, direction, 60.0f, refT, refCell)) continue;

        // The march landed in a solid cell. The DDA must have found something
        // at least as near -- the step tolerance keeps a cell the march
        // entered slightly late from reading as the DDA being late.
        if (!ddaHit || hit.t > refT + 0.01f) {
            if (skipped == 0) {
                std::printf("        skipped: origin %.6f %.6f %.6f\n", origin.x, origin.y,
                            origin.z);
                std::printf("                 dir    %.6f %.6f %.6f\n", direction.x, direction.y,
                            direction.z);
                std::printf("                 march t=%.6f cell %d %d %d\n", refT, refCell.x,
                            refCell.y, refCell.z);
                std::printf("                 dda   %s t=%.6f\n", ddaHit ? "hit" : "miss", hit.t);
            }
            ++skipped;
        }
    }

    check(wrongHit == 0, "every DDA hit lands exactly on its cell's entry face, 6k rays");
    check(skipped == 0, "the DDA never walks past a voxel the march found, 6k rays");
    if (wrongHit != 0) std::printf("        %d bogus of %d\n", wrongHit, kRays);
    if (skipped != 0) std::printf("        %d skipped of %d\n", skipped, kRays);

    {   // A ray starting inside must still return a usable surface.
        VoxelHit hit;
        bool got = model.trace({4.5f, 4.5f, 4.5f}, {0.3f, 0.8f, 0.5f}, 1e-4f, 50.0f, hit);
        if (got) {
            check(nearly(length(hit.normal), 1.0f, 1e-3f), "a ray starting inside gets a normal");
        }
    }
}

// ----------------------------------------------------------------- placing
void testPlacement() {
    std::printf("prop placement\n");

    // A 2x2x2 block of voxels, so the maths is easy to check by hand.
    VoxelModel cube;
    cube.resize({2, 2, 2});
    uint16_t slot = cube.addMaterial(colour(0.9f, 0.1f, 0.1f));
    for (int y = 0; y < 2; ++y)
        for (int z = 0; z < 2; ++z)
            for (int x = 0; x < 2; ++x) cube.set({x, y, z}, slot);

    {   // BottomCentre: the model stands on `position`.
        PropSet set;
        Prop prop;
        prop.model = &cube;
        prop.position = {0.0f, 10.0f, 0.0f};
        prop.voxelSize = 0.5f;  // a one-unit cube
        prop.anchor = Prop::Anchor::BottomCentre;
        set.add(prop);
        set.build();

        check(set.size() == 1, "the prop is placed");
        check(set.voxelCount() == 8, "and its voxels are counted");

        PropHit hit;
        // Straight down onto the top face, which should sit at y = 11.
        check(set.intersect({{0.0f, 20.0f, 0.0f}, {0, -1, 0}}, 100.0f, hit),
              "a ray from above hits it");
        check(nearly(hit.t, 9.0f, 1e-3f), "with the top face at the model's height");
        check(nearly(hit.normal.y, 1.0f, 1e-3f), "and an upward normal");
        check(nearly(hit.albedo.x, 0.9f, 1e-3f), "carrying the palette colour");

        // The feet are at y = 10, so a ray below misses on the way down.
        check(!set.intersect({{0.0f, 9.0f, 0.0f}, {0, -1, 0}}, 100.0f, hit),
              "nothing hangs below the anchor");

        Vec3 lo, hi;
        check(set.bounds(lo, hi), "the set reports bounds");
        check(nearly(lo.y, 10.0f, 1e-3f) && nearly(hi.y, 11.0f, 1e-3f),
              "which sit on the anchor");
    }

    {   // Centre: the model straddles `position`.
        PropSet set;
        Prop prop;
        prop.model = &cube;
        prop.position = {0.0f, 10.0f, 0.0f};
        prop.voxelSize = 0.5f;
        prop.anchor = Prop::Anchor::Centre;
        set.add(prop);
        set.build();

        Vec3 lo, hi;
        set.bounds(lo, hi);
        check(nearly(lo.y, 9.5f, 1e-3f) && nearly(hi.y, 10.5f, 1e-3f),
              "centre anchoring straddles the position");
    }

    {   // Scale really scales: doubling voxelSize doubles the extent.
        PropSet small, large;
        Prop prop;
        prop.model = &cube;
        prop.anchor = Prop::Anchor::Min;

        prop.voxelSize = 1.0f;
        small.add(prop);
        small.build();

        prop.voxelSize = 3.0f;
        large.add(prop);
        large.build();

        Vec3 lo1, hi1, lo2, hi2;
        small.bounds(lo1, hi1);
        large.bounds(lo2, hi2);
        check(nearly((hi2.x - lo2.x), (hi1.x - lo1.x) * 3.0f, 1e-3f), "voxelSize scales the prop");
    }

    {   // A quarter turn about +Y moves the far corner where it should.
        VoxelModel bar;
        bar.resize({4, 1, 1});
        uint16_t s = bar.addMaterial(colour(0.5f, 0.5f, 0.5f));
        for (int x = 0; x < 4; ++x) bar.set({x, 0, 0}, s);

        PropSet set;
        Prop prop;
        prop.model = &bar;
        prop.position = {0.0f, 0.0f, 0.0f};
        prop.voxelSize = 1.0f;
        prop.anchor = Prop::Anchor::Min;
        prop.yawDegrees = 90.0f;
        set.add(prop);
        set.build();

        // Unrotated the bar runs along +X. Yaw of 90 sends +X to... whichever
        // way the convention says, but the extent must swap axes either way.
        Vec3 lo, hi;
        set.bounds(lo, hi);
        check(nearly(hi.z - lo.z, 4.0f, 1e-3f), "a quarter turn puts the length on Z");
        check(nearly(hi.x - lo.x, 1.0f, 1e-3f), "and the width on X");
    }

    {   // Ray distance survives the transform: the same surface, two scales.
        PropSet set;
        Prop prop;
        prop.model = &cube;
        prop.position = {0.0f, 0.0f, 0.0f};
        prop.voxelSize = 2.0f;   // a four-unit cube
        prop.anchor = Prop::Anchor::Centre;
        set.add(prop);
        set.build();

        PropHit hit;
        check(set.intersect({{0.0f, 0.0f, 30.0f}, {0, 0, -1}}, 100.0f, hit),
              "a scaled prop is hit");
        // Half extent is 2, so the near face is at z = 2 and the ray runs 28.
        check(nearly(hit.t, 28.0f, 1e-3f), "and t still measures world distance");
    }

    {   // Tint and emission scaling multiply the palette through.
        PropSet set;
        VoxelModel glow;
        glow.resize({1, 1, 1});
        VoxelMaterial material;
        material.albedo = {1.0f, 1.0f, 1.0f};
        material.emission = {2.0f, 2.0f, 2.0f};
        glow.set({0, 0, 0}, glow.addMaterial(material));

        Prop prop;
        prop.model = &glow;
        prop.voxelSize = 1.0f;
        prop.anchor = Prop::Anchor::Centre;
        prop.tint = {0.5f, 0.25f, 0.0f};
        prop.emissionScale = {3.0f, 3.0f, 3.0f};
        set.add(prop);
        set.build();

        PropHit hit;
        check(set.intersect({{0.0f, 0.0f, 10.0f}, {0, 0, -1}}, 100.0f, hit), "the glowing prop is hit");
        check(nearly(hit.albedo.x, 0.5f, 1e-3f) && nearly(hit.albedo.y, 0.25f, 1e-3f),
              "tint multiplies the albedo");
        check(nearly(hit.emission.x, 6.0f, 1e-3f), "emissionScale multiplies the emission");
    }

    {   // A prop with no model, or an empty one, is quietly ignored.
        PropSet set;
        Prop nothing;
        set.add(nothing);

        VoxelModel blank;
        blank.resize({4, 4, 4});
        Prop empty;
        empty.model = &blank;
        set.add(empty);
        set.build();

        check(set.empty(), "props with nothing in them are dropped");
    }

    {   // Nearest wins across props.
        PropSet set;
        for (float z : {-6.0f, 3.0f, -1.0f}) {
            Prop prop;
            prop.model = &cube;
            prop.position = {0.0f, 0.0f, z};
            prop.voxelSize = 1.0f;
            prop.anchor = Prop::Anchor::Centre;
            set.add(prop);
        }
        set.build();

        PropHit hit;
        check(set.intersect({{0.0f, 0.0f, 20.0f}, {0, 0, -1}}, 100.0f, hit), "a row of props is hit");
        check(nearly(hit.t, 16.0f, 1e-3f), "and the nearest one wins");
    }
}

// ------------------------------------------------------------------- items
void testItem() {
    std::printf("item extrusion\n");

    // A tiny stand-in for an item sprite: an L, so orientation is visible.
    ImageU8 sprite(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) sprite.set(x, y, {0, 0, 0, 0});

    // Column 0 full, plus the bottom row: an L in image space, where y = 0 is
    // the top.
    for (int y = 0; y < 4; ++y) sprite.set(0, y, {255, 0, 0, 255});
    for (int x = 0; x < 4; ++x) sprite.set(x, 3, {0, 255, 0, 255});

    Texture texture;
    texture.fromImage(sprite);

    VoxelModel model;
    item::ItemOptions options;
    options.depthTexels = 2;
    options.trim = false;
    item::buildModel(texture, model, options);

    check(model.dims() == IVec3(4, 4, 2), "the model is the sprite, extruded");
    check(model.solidCount() == 7u * 2u, "one voxel column per opaque texel");

    // The image's bottom row must end up at the model's bottom, not its top.
    check(model.at({3, 0, 0}) != VoxelModel::kEmpty, "the foot of the L is at the bottom");
    check(model.at({3, 3, 0}) == VoxelModel::kEmpty, "and not at the top");
    check(nearly(model.material(model.at({3, 0, 0})).albedo.y,
                 srgbToLinear(1.0f), 1e-3f),
          "the bottom row keeps its own colour");

    // Both depth layers are filled.
    check(model.at({0, 3, 1}) != VoxelModel::kEmpty, "the extrusion fills every layer");

    {   // Depth of one is still legal.
        VoxelModel thin;
        item::ItemOptions one;
        one.depthTexels = 1;
        item::buildModel(texture, thin, one);
        check(thin.dims().z == 1, "a single-texel depth works");
    }

    {   // A fully transparent texture yields nothing rather than a block.
        ImageU8 blank(4, 4);
        Texture empty;
        empty.fromImage(blank);
        VoxelModel model2;
        item::buildModel(empty, model2, {});
        check(model2.empty(), "a transparent texture builds nothing");
    }

    {   // trim removes the empty margin.
        ImageU8 dot(8, 8);
        dot.set(4, 4, {255, 255, 255, 255});
        Texture one;
        one.fromImage(dot);

        VoxelModel model3;
        item::ItemOptions trimmed;
        trimmed.depthTexels = 1;
        item::buildModel(one, model3, trimmed);
        check(model3.dims() == IVec3(1, 1, 1), "trim shrinks to the single texel");
    }
}

// -------------------------------------------------------------- authoring
void testAuthoring() {
    std::printf("authoring\n");

    {   // Capture from a world: everything shape:: can build becomes a prop.
        World world(palette::registry());
        shape::ellipsoid(world, {8.5f, 8.5f, 8.5f}, Vec3{4.0f}, palette::GoldBlock);

        VoxelModel model = voxelize::fromWorld(world, {0, 0, 0}, {16, 16, 16});
        check(!model.empty(), "a captured sphere has voxels");
        check(model.materialCount() == 1, "one block kind, one material");
        check(model.dims().x <= 9 && model.dims().y <= 9,
              "trim shrinks the capture to the sphere");

        IVec3 middle{model.dims().x / 2, model.dims().y / 2, model.dims().z / 2};
        uint16_t core = model.at(middle);
        check(core != VoxelModel::kEmpty, "the middle of a solid sphere is solid");
        check(model.material(core).albedo.x > 0.0f,
              "with a colour taken from the block palette");
    }

    {   // Transmissive blocks are skipped by default.
        World world(palette::registry());
        world.fillBox({0, 0, 0}, {3, 3, 3}, palette::Water);
        world.set({0, 0, 0}, palette::Stone);

        VoxelModel skipped = voxelize::fromWorld(world, {0, 0, 0}, {3, 3, 3});
        check(skipped.solidCount() == 1, "water is skipped by default");

        voxelize::CaptureOptions keep;
        keep.skipTransmissive = false;
        VoxelModel kept = voxelize::fromWorld(world, {0, 0, 0}, {3, 3, 3}, keep);
        check(kept.solidCount() == 64, "unless the caller asks for it");
    }

    {   // An empty region gives an empty model, not a crash.
        World world(palette::registry());
        VoxelModel nothing = voxelize::fromWorld(world, {0, 0, 0}, {8, 8, 8});
        check(nothing.empty(), "capturing empty air gives an empty model");
    }

    {   // Hand-drawn layers.
        VoxelMaterial clay = colour(0.7f, 0.4f, 0.3f);
        VoxelMaterial water = colour(0.1f, 0.3f, 0.8f);

        VoxelModel mug = voxelize::fromLayers(
            {
                {"###", "###", "###"},  // y = 0, a solid base
                {"###", "#~#", "###"},  // y = 1, hollow with something in it
            },
            {{'#', clay}, {'~', water}});

        check(mug.dims() == IVec3(3, 2, 3), "layers give the expected dimensions");
        check(mug.solidCount() == 9u + 9u, "every marked cell is filled");
        check(mug.materialCount() == 2, "both key entries are in the palette");

        // layers[y][z][x]: the '~' sits at x = 1, y = 1, z = 1.
        uint16_t inner = mug.at({1, 1, 1});
        check(inner != VoxelModel::kEmpty, "the keyed cell is where the text put it");
        check(nearly(mug.material(inner).albedo.z, 0.8f, 1e-3f), "with its own material");

        // '.' and ' ' are holes.
        VoxelModel holes = voxelize::fromLayers({{"#.#", ". .", "#.#"}}, {{'#', clay}});
        check(holes.solidCount() == 4, "dots and spaces are empty");

        // An unknown character is ignored rather than guessed at.
        VoxelModel unknown = voxelize::fromLayers({{"#?#"}}, {{'#', clay}});
        check(unknown.solidCount() == 2, "characters not in the key are skipped");

        check(voxelize::fromLayers({}, {}).empty(), "no layers, no model");
    }
}

// ------------------------------------------------------------------ scene
void testSceneIntegration() {
    std::printf("scene integration\n");

    VoxelModel cube;
    cube.resize({1, 1, 1});
    VoxelMaterial material;
    material.albedo = {0.1f, 0.9f, 0.2f};
    material.metallic = 1.0f;
    material.roughness = 0.2f;
    cube.set({0, 0, 0}, cube.addMaterial(material));

    Scene scene(palette::registry());
    scene.world.fillBox({-4, -4, -4}, {4, -1, 4}, palette::Stone);
    scene.world.set({0, 0, 0}, palette::Stone);

    PropSet props;
    Prop prop;
    prop.model = &cube;
    prop.position = {0.5f, 0.5f, 4.0f};
    prop.voxelSize = 1.0f;
    prop.anchor = Prop::Anchor::Centre;
    props.add(prop);
    props.build();

    Ray ray{{0.5f, 0.5f, 12.0f}, {0.0f, 0.0f, -1.0f}};

    {   // Without the set, the ray reaches the block behind.
        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the block is hit");
        check(hit.isBlock, "and it is a block");
    }

    scene.props = &props;

    {   // With it, the nearer prop wins and brings its whole material.
        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the prop is hit");
        check(!hit.isBlock, "and it is not a block");
        check(nearly(hit.t, 7.5f, 1e-3f), "at the prop's distance");
        check(nearly(hit.albedo.y, 0.9f, 1e-3f), "with the prop's albedo");
        check(nearly(hit.metallic, 1.0f, 1e-3f), "a prop can be a conductor");
        check(nearly(hit.roughness, 0.2f, 1e-3f), "and carries its roughness");
        check(!hit.transmissive, "props are never media");
    }

    {   // And it blocks light.
        Vec3 transmittance = sceneTransmittance(scene, ray, 100.0f);
        check(maxComponent(transmittance) <= 0.0f, "a prop stops a shadow ray");
    }

    {   // An unbuilt set is inert.
        PropSet unbuilt;
        unbuilt.add(prop);
        scene.props = &unbuilt;

        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the scene still traces");
        check(hit.isBlock, "an unbuilt set contributes nothing");
    }
}

} // namespace

int main() {
    testModel();
    testTraversal();
    testPlacement();
    testItem();
    testAuthoring();
    testSceneIntegration();

    if (gFailures == 0) {
        std::printf("\nall prop tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
