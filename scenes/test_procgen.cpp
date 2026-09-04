// Tests for the stage 6 additions: modelling primitives, the clipboard,
// Poisson scattering, and the denoiser.
//
// The denoiser is the one that needs real numbers rather than an eyeball:
// "looks smoother" is satisfied equally well by a filter that has destroyed
// the image, so the checks below demand that it cuts variance *and* keeps
// the mean, the texture and the edges.
#include "engine/core/random.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/post/effects.hpp"
#include "engine/render/post/stylize.hpp"
#include "engine/render/trace/intersect.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/vegetation.hpp"
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

bool nearly(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

uint64_t countBlocks(const World& world) { return world.blockCount(); }

// ------------------------------------------------------------------ shapes
void testShapes() {
    std::printf("modelling primitives\n");

    {   // A sphere is symmetric about every axis.
        World world(palette::registry());
        shape::ellipsoid(world, {0.5f, 0.5f, 0.5f}, Vec3{6.0f}, palette::Stone);
        check(countBlocks(world) > 800, "a radius-6 sphere fills a plausible volume");

        int asymmetric = 0;
        for (int y = -7; y <= 7; ++y) {
            for (int z = -7; z <= 7; ++z) {
                for (int x = -7; x <= 7; ++x) {
                    // Block x has its centre at x + 0.5, so mirroring about
                    // a centre of 0.5 maps block x to block -x.
                    if (world.get({x, y, z}) != world.get({-x, y, z})) ++asymmetric;
                }
            }
        }
        check(asymmetric == 0, "the sphere is symmetric about its centre");
    }

    {   // A hollow sphere must be lighter than a solid one but occupy the
        // same bounding box.
        World solid(palette::registry()), hollow(palette::registry());
        shape::ellipsoid(solid, {0.5f, 0.5f, 0.5f}, Vec3{6.0f}, palette::Stone, false);
        shape::ellipsoid(hollow, {0.5f, 0.5f, 0.5f}, Vec3{6.0f}, palette::Stone, true);
        check(hollow.blockCount() < solid.blockCount() / 2, "a hollow sphere is mostly empty");
        check(hollow.minBlock() == solid.minBlock() && hollow.maxBlock() == solid.maxBlock(),
              "hollow and solid share a bounding box");
    }

    {   // A vertical cylinder: exact height, and a round footprint.
        World world(palette::registry());
        shape::cylinder(world, {0.5f, 0.0f, 0.5f}, {0, 1, 0}, 4.0f, 10.0f, palette::Stone);
        check(world.minBlock().y == 0, "the cylinder starts at its base");
        check(world.maxBlock().y == 9, "the cylinder is ten blocks tall");
        check(world.get({0, 5, 0}) == palette::Stone, "the axis is filled");
        check(world.get({6, 5, 0}) == palette::Air, "nothing beyond the radius");
    }

    {   // A line reaches both endpoints.
        World world(palette::registry());
        shape::line(world, {0.5f, 0.5f, 0.5f}, {20.5f, 12.5f, 6.5f}, 0.6f, palette::OakLog);
        check(world.get({0, 0, 0}) != palette::Air, "the line covers its start");
        check(world.get({20, 12, 6}) != palette::Air, "the line covers its end");
    }

    {   // Erosion removes a lone block but leaves a solid mass alone.
        World world(palette::registry());
        world.fillBox({0, 0, 0}, {5, 5, 5}, palette::Stone);
        world.set({20, 20, 20}, palette::Stone);   // isolated, zero neighbours

        uint64_t before = world.blockCount();
        shape::erode(world, {-1, -1, -1}, {25, 25, 25}, 3, 1);

        check(world.get({20, 20, 20}) == palette::Air, "an isolated block erodes away");
        check(world.get({2, 2, 2}) == palette::Stone, "the interior of a mass survives");
        check(world.blockCount() < before, "erosion removed something");
    }
}

// --------------------------------------------------------------- clipboard
void testClipboard() {
    std::printf("clipboard: copy, mirror, rotate\n");

    World world(palette::registry());
    // An asymmetric shape, so a wrong transform cannot look right by accident.
    world.fillBox({0, 0, 0}, {4, 1, 2}, palette::Stone);
    world.set({0, 2, 0}, palette::GoldBlock);
    world.set({4, 2, 2}, palette::RedWool);

    shape::Clipboard clip = shape::copy(world, {0, 0, 0}, {4, 2, 2});
    check(clip.size == IVec3{5, 3, 3}, "the clipboard has the region's size");
    check(clip.at(0, 2, 0) == palette::GoldBlock, "the clipboard kept its contents");

    {   // Pasting somewhere else reproduces it exactly.
        World other(palette::registry());
        shape::paste(other, clip, {100, 0, 100}, false);
        check(other.get({100, 2, 100}) == palette::GoldBlock, "paste reproduces the marker");
        check(other.get({104, 2, 102}) == palette::RedWool, "paste reproduces the far marker");
        check(other.blockCount() == world.blockCount(), "paste reproduces the block count");
    }

    {   // Mirroring twice is the identity.
        shape::Clipboard once = shape::mirrored(clip, 0);
        shape::Clipboard twice = shape::mirrored(once, 0);
        check(twice.blocks == clip.blocks, "mirroring twice returns the original");
        check(once.at(4, 2, 0) == palette::GoldBlock, "the marker moved to the far side");
    }

    {   // Four quarter turns are the identity, and one turn swaps the footprint.
        shape::Clipboard turned = shape::rotatedY(clip, 1);
        check(turned.size == IVec3{3, 3, 5}, "a quarter turn swaps the footprint");

        shape::Clipboard full = shape::rotatedY(
            shape::rotatedY(shape::rotatedY(shape::rotatedY(clip, 1), 1), 1), 1);
        check(full.size == clip.size, "four turns restore the size");
        check(full.blocks == clip.blocks, "four turns restore the contents");
    }
}

// ------------------------------------------------------------- scattering
void testPoissonDisk() {
    std::printf("Poisson-disk scattering\n");

    const float spacing = 4.0f;
    std::vector<Vec2> points = poissonDisk({-40.0f, -40.0f}, {40.0f, 40.0f}, spacing, 1234u);

    check(points.size() > 100, "a plausible number of points");

    int outside = 0;
    for (Vec2 p : points) {
        if (p.x < -40.0f || p.x >= 40.0f || p.y < -40.0f || p.y >= 40.0f) ++outside;
    }
    check(outside == 0, "every point is inside the region");

    // The defining property: no two points closer than the spacing.
    int tooClose = 0;
    float closest = 1e9f;
    for (size_t i = 0; i < points.size(); ++i) {
        for (size_t j = i + 1; j < points.size(); ++j) {
            float dx = points[i].x - points[j].x;
            float dy = points[i].y - points[j].y;
            float distance = std::sqrt(dx * dx + dy * dy);
            closest = std::min(closest, distance);
            if (distance < spacing - 1e-3f) ++tooClose;
        }
    }
    check(tooClose == 0, "no two points are closer than the minimum spacing");
    std::printf("    %zu points, closest pair %.3f (minimum %.1f)\n",
                points.size(), closest, spacing);

    // The same seed must give the same layout, or scenes stop being
    // reproducible between runs.
    std::vector<Vec2> again = poissonDisk({-40.0f, -40.0f}, {40.0f, 40.0f}, spacing, 1234u);
    check(again.size() == points.size(), "the same seed gives the same count");
}

void testTrees() {
    std::printf("trees\n");

    World world(palette::registry());
    world.fillBox({-8, 0, -8}, {8, 0, 8}, palette::GrassBlock);

    growTree(world, {0, 0, 0}, tree::oak(palette::OakLog, palette::OakLeaves), 7u);
    check(world.get({0, 1, 0}) == palette::OakLog, "the trunk starts above the ground block");
    check(world.maxBlock().y >= 6, "the tree is a plausible height");

    uint64_t afterOak = world.blockCount();
    growTree(world, {6, 0, 6}, tree::spruce(palette::SpruceLog, palette::SpruceLeaves), 9u);
    check(world.blockCount() > afterOak, "a second tree adds blocks");
    check(world.get({6, 1, 6}) == palette::SpruceLog, "the conifer uses spruce wood");

    // Leaves must never overwrite wood.
    World lone(palette::registry());
    lone.set({0, 0, 0}, palette::GrassBlock);
    growTree(lone, {0, 0, 0}, tree::oak(palette::OakLog, palette::OakLeaves), 3u);
    int trunkGaps = 0;
    for (int y = 1; y <= 4; ++y) {
        if (lone.get({0, y, 0}) == palette::OakLeaves) ++trunkGaps;
    }
    check(trunkGaps == 0, "leaves never replace the trunk");
}

// --------------------------------------------------------------- denoiser
void testDenoiser() {
    std::printf("denoiser\n");

    constexpr int kWidth = 160, kHeight = 120;

    // A scene split down the middle: two surfaces with different albedo and
    // different lighting, so the filter has a real edge to preserve.
    RenderTargets targets;
    targets.width = kWidth;
    targets.height = kHeight;
    targets.color = Image(kWidth, kHeight);
    targets.albedo = Image(kWidth, kHeight);
    targets.normal = Image(kWidth, kHeight);
    targets.depth.assign(size_t(kWidth) * size_t(kHeight), 10.0f);

    Image clean(kWidth, kHeight);
    Rng rng(99);

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            bool leftHalf = x < kWidth / 2;
            Vec3 albedo = leftHalf ? Vec3{0.8f, 0.2f, 0.2f} : Vec3{0.2f, 0.5f, 0.8f};
            float lighting = leftHalf ? 1.0f : 0.35f;

            targets.albedo.at(x, y) = albedo;
            targets.normal.at(x, y) = leftHalf ? Vec3{0, 1, 0} : Vec3{1, 0, 0};

            Vec3 truth = albedo * lighting;
            clean.at(x, y) = truth;

            // Heavy multiplicative noise, like a badly undersampled render.
            float n = 1.0f + (rng.nextFloat() - 0.5f) * 1.6f;
            targets.color.at(x, y) = truth * std::max(0.0f, n);
        }
    }

    Image result = denoise(targets, {});

    auto meanAndVariance = [&](const Image& image, int x0, int x1, float& mean, float& variance) {
        double sum = 0.0, sumSquares = 0.0;
        int n = 0;
        for (int y = 4; y < kHeight - 4; ++y) {
            for (int x = x0; x < x1; ++x) {
                float v = image.at(x, y).x;
                sum += v;
                sumSquares += double(v) * v;
                ++n;
            }
        }
        mean = float(sum / n);
        variance = float(sumSquares / n - (sum / n) * (sum / n));
    };

    float noisyMean = 0, noisyVariance = 0, cleanMean = 0, cleanVariance = 0;
    float resultMean = 0, resultVariance = 0;
    meanAndVariance(targets.color, 8, kWidth / 2 - 8, noisyMean, noisyVariance);
    meanAndVariance(clean, 8, kWidth / 2 - 8, cleanMean, cleanVariance);
    meanAndVariance(result, 8, kWidth / 2 - 8, resultMean, resultVariance);

    std::printf("    variance %.5f -> %.5f   mean %.4f -> %.4f (truth %.4f)\n",
                noisyVariance, resultVariance, noisyMean, resultMean, cleanMean);

    check(resultVariance < noisyVariance * 0.1f, "variance drops by at least ten times");
    check(nearly(resultMean, cleanMean, 0.03f), "the mean survives filtering");

    // The edge must not be smeared. Comparing against the ground truth is
    // the honest check: a ratio between the two sides can be satisfied by an
    // image that has drifted a long way from correct.
    for (int offset : {-3, -2, 2, 3}) {
        int x = kWidth / 2 + offset;
        Vec3 got = result.at(x, kHeight / 2);
        Vec3 truth = clean.at(x, kHeight / 2);
        bool close = nearly(got.x, truth.x, 0.05f) && nearly(got.y, truth.y, 0.05f) &&
                     nearly(got.z, truth.z, 0.05f);
        if (!close) {
            std::printf("  FAIL  seam at x=%d: got (%.3f %.3f %.3f), truth (%.3f %.3f %.3f)\n",
                        x, got.x, got.y, got.z, truth.x, truth.y, truth.z);
            ++gFailures;
        }
    }

    // A depth step with matching normals and albedo: nothing but distance
    // separates the two surfaces, which is the case a loose depth tolerance
    // silently blurs away.
    {
        RenderTargets step;
        step.width = kWidth;
        step.height = kHeight;
        step.color = Image(kWidth, kHeight);
        step.albedo = Image(kWidth, kHeight, Vec3{0.5f, 0.5f, 0.5f});
        step.normal = Image(kWidth, kHeight, Vec3{0.0f, 1.0f, 0.0f});
        step.depth.assign(size_t(kWidth) * size_t(kHeight), 0.0f);

        Image truth(kWidth, kHeight);
        Rng noise(4242);

        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                bool near = x < kWidth / 2;
                // Seventy blocks away, two blocks apart -- a terrace edge.
                step.depth[size_t(y) * size_t(kWidth) + size_t(x)] = near ? 70.0f : 72.0f;

                float lighting = near ? 0.9f : 0.3f;
                Vec3 value = Vec3{0.5f} * lighting;
                truth.at(x, y) = value;
                step.color.at(x, y) = value * std::max(0.0f, 1.0f + (noise.nextFloat() - 0.5f) * 1.2f);
            }
        }

        Image filtered = denoise(step, {});
        float leftGot = filtered.at(kWidth / 2 - 3, kHeight / 2).x;
        float rightGot = filtered.at(kWidth / 2 + 3, kHeight / 2).x;
        float leftTruth = truth.at(kWidth / 2 - 3, kHeight / 2).x;
        float rightTruth = truth.at(kWidth / 2 + 3, kHeight / 2).x;

        std::printf("    depth step: got %.3f | %.3f, truth %.3f | %.3f\n",
                    leftGot, rightGot, leftTruth, rightTruth);
        check(nearly(leftGot, leftTruth, 0.04f) && nearly(rightGot, rightTruth, 0.04f),
              "a depth-only edge survives filtering");
    }

    // The row split must not be visible in the answer, and the claim is
    // exactness rather than closeness. A pass reads one buffer and writes
    // another, so a worker boundary that moved a pixel would mean some row
    // had read a value another row was still writing -- and a tolerance is
    // precisely what would hide that, since the first symptom of it is a
    // difference far below the noise the filter is there to remove.
    {
        DenoiseSettings alone;
        alone.threads = 1;
        DenoiseSettings split;
        split.threads = 8;

        Image oneThread = denoise(targets, alone);
        Image manyThreads = denoise(targets, split);

        float worst = 0.0f;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                worst = std::max(worst, maxComponent(absv(oneThread.at(x, y) - manyThreads.at(x, y))));
            }
        }

        std::printf("    one worker vs eight: worst pixel difference %g\n", double(worst));
        check(worst == 0.0f, "splitting the rows across workers changes nothing");
    }

    // Missing auxiliary buffers must be refused, not guessed at.
    RenderTargets empty;
    empty.color = targets.color;
    Image untouched = denoise(empty, {});
    check(untouched.width() == kWidth, "denoising without AOVs returns the input");
}

// ---------------------------------------------------------------- effects
void testEffects() {
    std::printf("post effects\n");

    {   // Bloom must leave a dim image alone and spread light from a bright one.
        Image dim(64, 64, Vec3{0.1f});
        Image before = dim;
        applyBloom(dim, {});
        check(nearly(dim.at(32, 32).x, before.at(32, 32).x, 1e-4f),
              "bloom leaves everything below the threshold untouched");

        Image spot(64, 64, Vec3{0.0f});
        spot.at(32, 32) = Vec3{200.0f};
        applyBloom(spot, {});
        check(spot.at(40, 40).x > 0.0f, "bloom spreads light away from a bright pixel");
        check(spot.at(32, 32).x > 100.0f, "the source stays bright");
    }

    {   // Grading: saturation zero must leave grey, and exposure must scale.
        Image image(8, 8, Vec3{0.6f, 0.2f, 0.1f});
        GradeSettings grade;
        grade.saturation = 0.0f;
        applyGrade(image, grade);
        Vec3 c = image.at(4, 4);
        check(nearly(c.x, c.y, 1e-4f) && nearly(c.y, c.z, 1e-4f),
              "zero saturation produces grey");

        Image scaled(8, 8, Vec3{0.25f});
        GradeSettings exposure;
        exposure.exposure = 2.0f;
        applyGrade(scaled, exposure);
        check(nearly(scaled.at(4, 4).x, 0.5f, 1e-4f), "exposure scales linearly");
    }

    {   // Vignette darkens corners and leaves the centre alone.
        Image image(128, 128, Vec3{1.0f});
        applyVignette(image, {});
        check(nearly(image.at(64, 64).x, 1.0f, 1e-4f), "the centre is untouched");
        check(image.at(2, 2).x < 0.95f, "the corners are darkened");
    }
}

// ----------------------------------------------------------------- styles
// Two seams, tested for two different things. The material style is checked
// for what it does to a surface *before* light reaches it; the stylise pass
// for what it does to light that has already arrived.

// A RenderTargets built by hand, so a check can state exactly what geometry
// and what illumination the pass is being shown.
RenderTargets makeTargets(int width, int height) {
    RenderTargets t;
    t.width = width;
    t.height = height;
    t.color = Image(width, height, Vec3{0.0f});
    t.albedo = Image(width, height, Vec3{1.0f});
    t.normal = Image(width, height, Vec3{0.0f, 1.0f, 0.0f});
    t.depth.assign(size_t(width) * size_t(height), 10.0f);
    return t;
}

void testMaterialStyle() {
    std::printf("material style\n");

    check(MaterialStyle::realistic().identity(), "the realistic style is identity");
    check(!MaterialStyle::plastic().identity(), "plastic is not");
    check(!MaterialStyle::matte().identity(), "matte is not");

    {   // Plastic forces a dielectric coat onto anything, gold included --
        // which is the whole point: a scene-wide style may not ask each
        // material's permission.
        StyledMaterial gold = MaterialStyle::plastic().apply(Vec3{1.0f, 0.77f, 0.34f}, 0.1f, 1.0f);
        check(gold.metallic == 0.0f, "plastic strips the metal");
        check(gold.coat > 0.0f, "plastic adds a coat");

        StyledMaterial clay = MaterialStyle::matte().apply(Vec3{0.5f, 0.5f, 0.5f}, 0.1f, 1.0f);
        check(clay.coat == 0.0f, "matte adds no coat");
        check(nearly(clay.roughness, 1.0f, 1e-6f), "matte is fully rough");
    }

    {   // Saturation acts on the albedo, before lighting. Zero must give an
        // exactly neutral colour, or bounce light picks up a tint.
        MaterialStyle grey;
        grey.saturation = 0.0f;
        StyledMaterial m = grey.apply(Vec3{0.7f, 0.2f, 0.1f}, 1.0f, 0.0f);
        check(nearly(m.albedo.x, m.albedo.y, 1e-5f) && nearly(m.albedo.y, m.albedo.z, 1e-5f),
              "zero saturation gives a neutral albedo");
    }

    {   // Through the real seam. A style must reach every kind of geometry and
        // must leave a refracting medium alone -- a plastic ocean stops being
        // an ocean, and the tracer's medium tracking depends on ior.
        Scene scene(palette::registry());
        scene.world.fillBox({0, 0, 0}, {3, 0, 3}, palette::Stone);
        scene.world.fillBox({0, 1, 0}, {3, 1, 3}, palette::Water);
        scene.materialStyle = MaterialStyle::plastic();

        SceneHit water;
        bool hitWater = intersectScene(scene, {{1.5f, 8.0f, 1.5f}, {0.0f, -1.0f, 0.0f}}, 64.0f,
                                       RayFilter{}, water);
        check(hitWater && water.transmissive, "the ray reached the water");
        check(hitWater && water.coat == 0.0f, "a refracting medium keeps its own material");

        SceneHit stone;
        RayFilter through;
        through.passThrough = palette::Water;
        bool hitStone = intersectScene(scene, {{1.5f, 1.5f, 1.5f}, {0.0f, -1.0f, 0.0f}}, 64.0f,
                                       through, stone);
        check(hitStone && stone.coat > 0.0f, "an opaque block is styled");
        check(hitStone && stone.metallic == 0.0f, "and made dielectric");
    }
}

void testCelShading() {
    std::printf("cel shading\n");

    {   // The claim that matters: banding quantises the *illumination*, and
        // the albedo passes through untouched. Two surfaces lit identically
        // must land in the same band while keeping their own colours -- the
        // demodulation the denoiser needs, for the same reason.
        RenderTargets t = makeTargets(2, 1);
        t.albedo.at(0, 0) = Vec3{0.80f, 0.10f, 0.10f};
        t.albedo.at(1, 0) = Vec3{0.10f, 0.80f, 0.10f};
        const float light = 0.55f;
        t.color.at(0, 0) = t.albedo.at(0, 0) * light;
        t.color.at(1, 0) = t.albedo.at(1, 0) * light;

        CelSettings cel;
        cel.bands = 4;
        cel.outlineWidth = 0;
        Image out = celShade(t, cel);

        float ratioRed = out.at(0, 0).x / t.albedo.at(0, 0).x;
        float ratioGreen = out.at(1, 0).y / t.albedo.at(1, 0).y;
        check(nearly(ratioRed, ratioGreen, 1e-4f), "equal light lands in the same band");
        check(out.at(0, 0).x > out.at(0, 0).y * 4.0f, "the red surface stayed red");
        check(out.at(1, 0).y > out.at(1, 0).x * 4.0f, "the green surface stayed green");
    }

    {   // A smooth ramp of light must come out with no more distinct values
        // than there are bands. Without this "banding" is satisfied by a pass
        // that changed nothing at all.
        const int width = 64;
        RenderTargets t = makeTargets(width, 1);
        for (int x = 0; x < width; ++x) t.color.at(x, 0) = Vec3{float(x) / float(width - 1)};

        CelSettings cel;
        cel.bands = 3;
        cel.outlineWidth = 0;
        cel.highlightCeiling = 0.0f;
        Image out = celShade(t, cel);

        int distinct = 0;
        float previous = -1.0f;
        for (int x = 0; x < width; ++x) {
            if (!nearly(out.at(x, 0).x, previous, 1e-5f)) { ++distinct; previous = out.at(x, 0).x; }
        }
        check(distinct <= 3, "a ramp comes out in at most `bands` steps");
        check(distinct > 1, "and in more than one");
    }

    {   // The outline reads geometry, and geometry means the depth *gradient*.
        // A floor seen at a glancing angle changes depth faster than the
        // threshold between one pixel and the next, and none of that is an
        // edge -- the same trap that once made the denoiser blur terraces.
        const int width = 32, height = 8;
        RenderTargets slope = makeTargets(width, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                slope.depth[size_t(y) * size_t(width) + size_t(x)] = 10.0f + 0.5f * float(x);
                slope.color.at(x, y) = Vec3{0.5f};
            }
        }

        CelSettings cel;
        cel.bands = 0;             // outline only, so nothing else can move a pixel
        cel.depthThreshold = 0.35f;
        cel.outlineWidth = 1;
        cel.outlineOpacity = 1.0f;
        Image out = celShade(slope, cel);

        // Interior only: at the border the central difference has one side
        // clamped, so it under-reports the slope by half. That is a real and
        // bounded inaccuracy, not an edge, and the test says which pixels it
        // is actually claiming something about.
        int slopeInked = 0;
        for (int y = 1; y < height - 1; ++y) {
            for (int x = 1; x < width - 1; ++x) {
                if (out.at(x, y).x < 0.4f) ++slopeInked;
            }
        }
        check(slopeInked == 0, "a sloped surface does not outline itself");

        // A real step, of the same magnitude the slope covers in two pixels.
        RenderTargets cliff = makeTargets(width, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                cliff.depth[size_t(y) * size_t(width) + size_t(x)] = x < width / 2 ? 10.0f : 13.0f;
                cliff.color.at(x, y) = Vec3{0.5f};
            }
        }
        Image inked = celShade(cliff, cel);

        int cliffInked = 0;
        for (int y = 1; y < height - 1; ++y) {
            for (int x = 1; x < width - 1; ++x) {
                if (inked.at(x, y).x < 0.4f) ++cliffInked;
            }
        }
        check(cliffInked >= height - 2, "a depth step is inked");
    }

    {   // Same contract as the denoiser: no auxiliary buffers, no guessing.
        RenderTargets empty;
        empty.color = Image(4, 4, Vec3{0.3f});
        Image out = celShade(empty, {});
        check(out.at(2, 2).x == 0.3f, "without AOVs the frame is returned unchanged");
    }
}

void testPixelate() {
    std::printf("pixelate\n");

    {   // Every cell is flat, and it holds the cell's mean -- so the crush
        // loses detail without losing or inventing light.
        Image image(4, 4, Vec3{0.0f});
        float sum = 0.0f;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                float v = float(x + 4 * y) / 16.0f;
                image.at(x, y) = Vec3{v};
                sum += v;
            }
        }
        const float mean = sum / 16.0f;

        PixelateSettings settings;
        settings.factor = 4;
        pixelate(image, settings);

        bool flat = true;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (!nearly(image.at(x, y).x, mean, 1e-5f)) flat = false;
            }
        }
        check(flat, "a cell becomes its own mean, everywhere in the cell");
    }

    {   // A grid that does not divide the frame must still cover it: the last
        // column and row are narrower, not missing.
        Image image(7, 5, Vec3{0.25f});
        image.at(6, 4) = Vec3{0.9f};
        PixelateSettings settings;
        settings.factor = 4;
        pixelate(image, settings);
        check(image.at(6, 4).x > 0.25f, "a partial cell at the edge is still averaged");
    }

    {   // Quantisation must actually reduce the palette.
        Image ramp(64, 1, Vec3{0.0f});
        for (int x = 0; x < 64; ++x) ramp.at(x, 0) = Vec3{float(x) / 63.0f};

        PixelateSettings settings;
        settings.factor = 1;
        settings.levels = 4;
        pixelate(ramp, settings);

        int distinct = 0;
        float previous = -1.0f;
        for (int x = 0; x < 64; ++x) {
            if (!nearly(ramp.at(x, 0).x, previous, 1e-5f)) { ++distinct; previous = ramp.at(x, 0).x; }
        }
        check(distinct <= 5, "four levels give at most five values");
    }
}

} // namespace

int main() {
    testShapes();
    testClipboard();
    testPoissonDisk();
    testTrees();
    testDenoiser();
    testEffects();
    testMaterialStyle();
    testCelShading();
    testPixelate();

    if (gFailures == 0) {
        std::printf("\nall procgen and post tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
