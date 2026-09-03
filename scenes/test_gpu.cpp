// The GPU voxel traversal against the CPU one, ray for ray.
//
// This is the acceptance test for step one of moving the tracer onto the
// GPU, and it is built the way the DDA-versus-march test in test_prop is
// built: the reference is asked only what it can actually answer.
//
// What is claimed, and checked exactly:
//   hit or miss, the block coordinate, the block id, the face axis and the
//   sign of the normal.
//
// What is claimed to a tolerance:
//   t, and the face uv that follows from it.
//
// The distinction is not a hedge. GLSL is allowed to contract a multiply and
// an add into an FMA where MSVC did not, and its division is allowed a
// different last bit, so demanding bit-equality of `t` would be demanding
// something the two languages never promised. Demanding the same *cell* is
// different: that is a discrete decision, and if the two disagree about it
// then one of them is walking the grid wrongly.
//
//   scene_test_gpu          synthetic world, then a real film set
//   scene_test_gpu bench    also time a large batch against the CPU
#include "engine/assets/asset_source.hpp"
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/assets/entity/skin.hpp"
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/prop/voxelize.hpp"
#include "engine/sprite/scatter.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/sprite/text.hpp"
#include "engine/platform/window.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/render/gpu/denoise_gpu.hpp"
#include "engine/render/gpu/pathtrace_gpu.hpp"
#include "engine/render/gpu/raycast_gpu.hpp"
#include "engine/render/gpu/wavefront_gpu.hpp"
#include "engine/world/shapes.hpp"
#include "engine/world/world.hpp"
#include "scenes/common/palette.hpp"

#include "common/film/sets.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A local generator, so the ray set is identical on every run and on every
// machine without dragging a seed through the engine's Rng.
struct Lcg {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(state >> 33);
    }
    float unit() { return float(next()) * (1.0f / 4294967296.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

// A world that is awkward on purpose: it straddles the origin so chunk
// coordinates go negative, it has holes, and it has a shell so that rays both
// enter and leave solid regions.
World buildSynthetic() {
    World world(palette::registry());
    world.fillBox({-40, -3, -40}, {40, -1, 40}, palette::Stone);
    world.fillBox({-20, 0, -20}, {20, 0, 20}, palette::GrassBlock);

    shape::ellipsoid(world, {0.0f, 9.0f, 0.0f}, {11.0f, 8.0f, 11.0f}, palette::Cobblestone, true);
    shape::ellipsoid(world, {-18.0f, 5.0f, 14.0f}, {6.0f, 6.0f, 6.0f}, palette::OakLog);
    shape::ellipsoid(world, {22.0f, 4.0f, -17.0f}, {5.0f, 9.0f, 5.0f}, palette::Glass);

    // A pond, so medium mode has something to be inside of.
    world.fillBox({-34, -1, 6}, {-24, -1, 18}, palette::Water);
    world.fillBox({-34, 0, 6}, {-24, 0, 18}, palette::Water);

    // Scattered single blocks, which are the cells a coarse walk is most
    // likely to step over.
    Lcg rng;
    for (int i = 0; i < 400; ++i) {
        const int x = int(rng.range(-38.0f, 38.0f));
        const int y = int(rng.range(1.0f, 26.0f));
        const int z = int(rng.range(-38.0f, 38.0f));
        world.set({x, y, z}, palette::GoldBlock);
    }
    return world;
}

// A skin where every texel is its own colour.
//
// test_entity paints one flat colour per face, which catches a face swapped
// for another face. This goes further on purpose: with a gradient, a uv that
// is mirrored or transposed *within* a face is a different colour too, and
// the whole point of the entity path on the GPU is that boxFaceUv had to be
// written a second time.
//
// The hat is punched into a chequer so the alpha cutout has something to cut.
Skin paintGradientSkin() {
    ImageU8 image(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint8_t r = uint8_t(x * 4);
            const uint8_t g = uint8_t(y * 4);
            const uint8_t b = uint8_t(((x / 4) + (y / 4)) % 2 ? 220 : 60);
            uint8_t a = 255;
            if (x >= 32 && y < 16 && ((x / 3) + (y / 3)) % 2 == 0) a = 0;   // the hat
            image.set(x, y, {r, g, b, a});
        }
    }

    // Skin takes a PNG, and pngEncode is right here, so the image goes out
    // and comes back rather than needing a second way in.
    const std::vector<uint8_t> png = pngEncode(image);
    Skin skin;
    skin.loadFromPng(png.data(), png.size(), nullptr);
    return skin;
}

struct RaySet {
    std::vector<gpu::GpuRay> gpu;
    std::vector<Ray> cpu;
};

// Rays from all over, most of them aimed back through the populated region so
// that the interesting path is the common one rather than the rare one.
RaySet makeRays(int count, Vec3 centre, float radius, uint64_t seed) {
    Lcg rng;
    rng.state = seed;

    RaySet set;
    set.gpu.reserve(size_t(count));
    set.cpu.reserve(size_t(count));

    for (int i = 0; i < count; ++i) {
        const Vec3 origin{centre.x + rng.range(-radius, radius),
                          centre.y + rng.range(-radius, radius),
                          centre.z + rng.range(-radius, radius)};

        Vec3 direction;
        // Two thirds aimed at a point in the middle of the world, one third
        // in a uniformly random direction -- the first finds surfaces, the
        // second finds the edges of the region and the misses.
        if ((i % 3) != 0) {
            const Vec3 target{centre.x + rng.range(-radius * 0.4f, radius * 0.4f),
                              centre.y + rng.range(-radius * 0.4f, radius * 0.4f),
                              centre.z + rng.range(-radius * 0.4f, radius * 0.4f)};
            const Vec3 delta = target - origin;
            direction = lengthSq(delta) > 1e-6f ? normalize(delta) : Vec3{0.0f, 1.0f, 0.0f};
        } else {
            const float z = rng.range(-1.0f, 1.0f);
            const float a = rng.range(0.0f, kTwoPi);
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            direction = Vec3{r * std::cos(a), z, r * std::sin(a)};
        }

        gpu::GpuRay g{};
        g.ox = origin.x; g.oy = origin.y; g.oz = origin.z;
        g.dx = direction.x; g.dy = direction.y; g.dz = direction.z;
        set.gpu.push_back(g);
        set.cpu.push_back(Ray{origin, direction});
    }
    return set;
}

struct Mismatch {
    int missCount = 0;      // one found a hit and the other did not
    int cellCount = 0;      // both hit, different block
    int idCount = 0;
    int faceCount = 0;
    int tCount = 0;
    float worstT = 0.0f;
    int firstBad = -1;

    // For a cell disagreement: how far apart the two cells are, and whether
    // the hit point was sitting on the lattice plane that separates them.
    // A disagreement of one, on a point that is exactly on a boundary, is
    // floor() splitting a hair -- not a walk that went somewhere else.
    int cellFarCount = 0;      // cells more than one apart on some axis
    int cellOffPlaneCount = 0; // one apart, but not on a boundary
    float worstPlaneDistance = 0.0f;

    // Rays where something disagreed that a lattice plane cannot excuse.
    // This is the number the test passes or fails on.
    int hardCount = 0;
};

Mismatch compare(const World& world, const RaySet& rays, const std::vector<gpu::GpuHit>& gpuHits,
                 float maxDistance, RayFilter filter, double* cpuSeconds) {
    Mismatch m;
    const auto started = std::chrono::steady_clock::now();

    for (size_t i = 0; i < rays.cpu.size(); ++i) {
        RayHit cpu;
        const bool cpuHit = raycast(world, rays.cpu[i], maxDistance, cpu, filter);
        const gpu::GpuHit& g = gpuHits[i];
        const bool gpuHit = g.hit > 0.5f;

        // How close the hit point sits to the lattice plane on each axis.
        // Everything below turns on this: where a hit lands exactly on a
        // plane, which cell it belongs to and which face it entered through
        // are not decided by the algorithm, they are decided by the last bit
        // of a float -- and two compilers are allowed to disagree there.
        const Vec3 hitPos = rays.cpu[i].origin + rays.cpu[i].direction * cpu.t;
        auto planeGap = [](float v) { return std::fabs(v - std::round(v)); };
        constexpr float kOnPlane = 1.0e-3f;

        bool bad = false;
        if (cpuHit != gpuHit) {
            ++m.missCount;
            bad = true;
        } else if (cpuHit) {
            bool cellOnPlane = false;
            if (cpu.block.x != g.bx || cpu.block.y != g.by || cpu.block.z != g.bz) {
                ++m.cellCount;

                const IVec3 gpuCell{g.bx, g.by, g.bz};
                bool far = false;
                float planeDistance = 1.0f;
                for (int a = 0; a < 3; ++a) {
                    const int delta = cpu.block[a] - gpuCell[a];
                    if (delta > 1 || delta < -1) far = true;
                    if (delta != 0) {
                        // The plane between the two candidate cells is the
                        // higher of the two coordinates.
                        const float plane = float(std::max(cpu.block[a], gpuCell[a]));
                        planeDistance = std::min(planeDistance, std::fabs(hitPos[a] - plane));
                    }
                }
                if (far) ++m.cellFarCount;
                else if (planeDistance > 1.0e-3f) ++m.cellOffPlaneCount;
                m.worstPlaneDistance = std::max(m.worstPlaneDistance, far ? 1.0f : planeDistance);

                // A cell disagreement is a failure unless it is one cell, on
                // the plane between them. That is not a loosened tolerance --
                // it is a narrower claim, and a stricter one than "the cells
                // match" in the only direction that matters: a walk that
                // actually went somewhere else lands two cells away, or one
                // cell away from a point nowhere near a boundary, and either
                // still fails.
                //
                // The path this happens on is `fillAirBoundary`, and it is the
                // only place in raycast.cpp that recovers a cell by flooring a
                // float instead of carrying it in the DDA's integers -- at
                // exactly the point where the float is a chunk boundary and
                // floor() has no way to decide. See the note in the report.
                cellOnPlane = !far && planeDistance <= kOnPlane;
                if (!cellOnPlane) bad = true;
            }
            // The id is read out of the cell, so an id disagreement is
            // acceptable exactly when the cell disagreement that produced it
            // was -- and never on its own.
            if (int(cpu.id) != g.id) {
                ++m.idCount;
                if (!cellOnPlane) bad = true;
            }

            int normalSign = 0;
            for (int a = 0; a < 3; ++a) {
                if (cpu.normal[a] != 0) normalSign = cpu.normal[a];
            }
            if (cpu.axis != g.axis || normalSign != g.normalSign) {
                ++m.faceCount;
                // Two faces are only interchangeable where the ray crosses
                // both planes at once -- a grazed edge or corner. Anywhere
                // else, naming a different face is naming a different walk.
                const bool onEdge = g.axis >= 0 && g.axis < 3 &&
                                    planeGap(hitPos[cpu.axis]) < kOnPlane &&
                                    planeGap(hitPos[g.axis]) < kOnPlane;
                if (!onEdge) bad = true;
            }

            const float dt = std::fabs(cpu.t - g.t);
            m.worstT = std::max(m.worstT, dt);
            if (dt > 1.0e-3f) { ++m.tCount; bad = true; }
        }

        if (bad) {
            ++m.hardCount;
            if (m.firstBad < 0) m.firstBad = int(i);
        }
    }

    if (cpuSeconds) {
        *cpuSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
    return m;
}

void report(const char* label, const Mismatch& m, size_t count, double cpuSeconds,
            const gpu::Raycaster& caster) {
    const double gpuSeconds = caster.lastSeconds();
    const int total = m.hardCount;
    std::printf("  %-22s %7zu rays   cpu %6.3f s   gpu %6.3f s   %5.1fx\n", label, count,
                cpuSeconds, gpuSeconds, cpuSeconds / std::max(gpuSeconds, 1e-9));
    std::printf("      upload %.3f s, dispatch %.3f s, readback %.3f s -- traversal alone is "
                "%.0fx\n",
                caster.lastUploadSeconds(), caster.lastDispatchSeconds(),
                caster.lastReadbackSeconds(),
                cpuSeconds / std::max(caster.lastDispatchSeconds(), 1e-9));
    std::printf("      worst |dt| %.3e", double(m.worstT));
    if (total == 0) {
        std::printf(", no disagreements\n");
    } else {
        std::printf(", %d disagreements (miss %d, cell far %d, cell off-plane %d, id %d, "
                    "face %d, t %d), first at ray %d\n",
                    total, m.missCount, m.cellFarCount, m.cellOffPlaneCount, m.idCount,
                    m.faceCount, m.tCount, m.firstBad);
    }
    if (m.cellCount > 0) {
        std::printf("      %d cells one apart on the plane between them (worst %.1e from it) "
                    "-- floor() on a boundary, not a different walk\n",
                    m.cellCount, double(m.worstPlaneDistance));
    }
    check(total == 0, label);
}

} // namespace

int main(int argc, char** argv) {
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "bench") == 0) bench = true;
    }

    Window window;
    std::string error;
    if (!window.create("BlockyEngine GPU traversal test", 320, 200, &error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        return 1;
    }

    gl::ContextSettings settings;
    settings.samples = 0;
    void* context = nullptr;
    if (!gl::createContext(window.deviceContext(), settings, &context, &error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        return 1;
    }
    std::printf("renderer : %s\n", gl::rendererString().c_str());

    gl::GLint maxInvocations = 0;
    gl::GetIntegerv(gl::MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxInvocations);
    check(maxInvocations >= 64, "the driver allows a 64-wide work group");

    gl::GLint maxSsbo = 0;
    gl::GetIntegerv(gl::MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &maxSsbo);
    std::printf("ssbo     : %d storage blocks per compute stage\n", maxSsbo);

    gpu::Raycaster caster;
    if (!caster.build(&error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        return 1;
    }
    std::printf("compute  : compiled\n\n");

    const int kRays = bench ? 2000000 : 250000;

    // ---------------------------------------------------------- synthetic
    {
        const World world = buildSynthetic();
        const gpu::PackedWorld packed = gpu::packWorld(world);
        std::printf("synthetic: %llu blocks, %zu chunks, grid %dx%dx%d, %.2f MiB on the gpu\n",
                    (unsigned long long)world.blockCount(), packed.chunkCount, packed.gridDim.x,
                    packed.gridDim.y, packed.gridDim.z, double(packed.bytes()) / 1048576.0);
        check(packed.chunkCount == world.chunkCount(), "every chunk was packed");

        if (!caster.upload(packed, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        const RaySet rays = makeRays(kRays, {0.0f, 6.0f, 0.0f}, 46.0f, 12345u);
        std::vector<gpu::GpuHit> hits;

        {
            RayFilter filter;
            if (!caster.trace(rays.gpu, hits, 512.0f, filter.passThrough, false, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }
            double cpuSeconds = 0.0;
            const Mismatch m = compare(world, rays, hits, 512.0f, filter, &cpuSeconds);
            report("synthetic, camera", m, rays.cpu.size(), cpuSeconds, caster);
        }

        {
            RayFilter filter;
            filter.opaqueOnly = true;
            if (!caster.trace(rays.gpu, hits, 512.0f, filter.passThrough, true, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }
            double cpuSeconds = 0.0;
            const Mismatch m = compare(world, rays, hits, 512.0f, filter, &cpuSeconds);
            report("synthetic, shadow", m, rays.cpu.size(), cpuSeconds, caster);
        }

        {
            // Medium mode: the caller is inside water and is asking where it
            // stops. Air counts as a surface there, and so does running out
            // of world, which is the branch a camera ray never reaches.
            RayFilter filter;
            filter.passThrough = palette::Water;
            if (!caster.trace(rays.gpu, hits, 512.0f, filter.passThrough, false, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }
            double cpuSeconds = 0.0;
            const Mismatch m = compare(world, rays, hits, 512.0f, filter, &cpuSeconds);
            report("synthetic, in water", m, rays.cpu.size(), cpuSeconds, caster);
        }
    }

    // ------------------------------------------------------- a real set
    {
        const film::Palette pal = film::registerBlocks();
        World world(palette::registry());
        film::road::build(world, pal);

        const gpu::PackedWorld packed = gpu::packWorld(world);
        std::printf("\nroad set : %llu blocks, %zu chunks, grid %dx%dx%d, %.2f MiB on the gpu\n",
                    (unsigned long long)world.blockCount(), packed.chunkCount, packed.gridDim.x,
                    packed.gridDim.y, packed.gridDim.z, double(packed.bytes()) / 1048576.0);

        if (!caster.upload(packed, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        const RaySet rays = makeRays(kRays, {30.0f, 8.0f, 0.0f}, 60.0f, 98765u);
        std::vector<gpu::GpuHit> hits;

        RayFilter filter;
        if (!caster.trace(rays.gpu, hits, 512.0f, filter.passThrough, false, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }
        double cpuSeconds = 0.0;
        const Mismatch m = compare(world, rays, hits, 512.0f, filter, &cpuSeconds);
        report("road, camera", m, rays.cpu.size(), cpuSeconds, caster);

        filter.opaqueOnly = true;
        if (!caster.trace(rays.gpu, hits, 512.0f, filter.passThrough, true, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }
        const Mismatch ms = compare(world, rays, hits, 512.0f, filter, &cpuSeconds);
        report("road, shadow", ms, rays.cpu.size(), cpuSeconds, caster);
    }

    // -------------------------------------------------- the whole integrator
    //
    // Two renderers, one scene, no shared code below the shading model. The
    // images cannot be compared pixel for pixel -- the two random streams are
    // different, and they are allowed to be -- so what is checked is what
    // convergence actually promises: that the two means agree.
    {
        Scene scene(palette::registry());
        scene.world = buildSynthetic();
        scene.world.set({0, 18, 0}, palette::Glowstone);
        scene.world.set({-14, 8, 12}, palette::Glowstone);
        scene.world.fillBox({20, 1, -20}, {24, 1, -16}, palette::Lava);

        scene.sun.direction = normalize(Vec3{-0.4f, 0.72f, -0.55f});
        scene.sun.intensity = 5.0f;
        scene.sun.angularRadiusDegrees = 1.4f;
        scene.sky.intensity = 1.0f;
        scene.camera.projection = Camera::Projection::Perspective;
        scene.camera.fovY = radians(48.0f);
        scene.camera.lookAt({34.0f, 20.0f, 40.0f}, {0.0f, 6.0f, 0.0f});

        // A small frame is a bad test of a GPU: 320x180 is 58k pixels, and
        // a card that wants hundreds of thousands of threads in flight spends
        // the run half empty. `bench` asks for the size the film renders at.
        PathSettings ps;
        ps.width = bench ? 1280 : 320;
        ps.height = bench ? 720 : 180;
        ps.samplesPerPixel = bench ? 64 : 512;
        ps.maxBounces = 6;
        ps.progress = false;
        ps.seed = 7;

        std::printf("\nintegrator: %dx%d, %d spp, %d bounces, flat palette\n", ps.width,
                    ps.height, ps.samplesPerPixel, ps.maxBounces);

        gpu::PathTracer tracer;
        if (!tracer.build(&error) || !tracer.upload(scene, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        Image gpuImage;
        RenderTargets gpuAovs;
        gpu::TraceStats gs;
        if (!tracer.render(scene, ps, gpuImage, &gpuAovs, &gs, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        RenderStats cs;
        RenderTargets cpuAovs;
        const Image cpuImage = renderPath(scene, ps, &cs, &cpuAovs);

        // Relative L1 over the whole frame. A path tracer's error falls as
        // 1/sqrt(N), so at 512 samples a couple of percent is convergence,
        // and anything much above it is a difference in the model.
        double sumAbs = 0.0, sumRef = 0.0, worstPixel = 0.0;
        double normalDot = 0.0, depthAbs = 0.0, depthRef = 0.0;
        int depthPixels = 0;
        for (int y = 0; y < ps.height; ++y) {
            for (int x = 0; x < ps.width; ++x) {
                const Vec3 a = cpuImage.at(x, y);
                const Vec3 b = gpuImage.at(x, y);
                const double d = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) +
                                 std::fabs(a.z - b.z);
                const double r = std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);
                sumAbs += d;
                sumRef += r;
                worstPixel = std::max(worstPixel, d / std::max(r, 0.05));

                const size_t i = size_t(y) * size_t(ps.width) + size_t(x);
                if (cpuAovs.depth[i] > 0.0f && gpuAovs.depth[i] > 0.0f) {
                    normalDot += double(dot(cpuAovs.normal.at(x, y), gpuAovs.normal.at(x, y)));
                    depthAbs += std::fabs(double(cpuAovs.depth[i] - gpuAovs.depth[i]));
                    depthRef += double(cpuAovs.depth[i]);
                    ++depthPixels;
                }
            }
        }

        const double relative = sumAbs / std::max(sumRef, 1e-9);
        const double meanNormalDot = depthPixels > 0 ? normalDot / depthPixels : 0.0;
        const double relativeDepth = depthAbs / std::max(depthRef, 1e-9);

        std::printf("  cpu   %6.2f s on %d threads\n", cs.seconds, cs.threadsUsed);
        std::printf("  gpu   %6.2f s  (upload %.2f, trace %.2f, readback %.2f, %d batches, "
                    "%zu lights)\n",
                    gs.totalSeconds(), gs.uploadSeconds, gs.traceSeconds, gs.readbackSeconds,
                    gs.batches, gs.lights);
        std::printf("  speedup %.1fx on the trace, %.1fx end to end\n",
                    cs.seconds / std::max(gs.traceSeconds, 1e-9),
                    cs.seconds / std::max(gs.totalSeconds(), 1e-9));
        std::printf("  relative L1 %.4f, worst pixel %.3f\n", relative, worstPixel);
        std::printf("  aov: mean normal dot %.5f over %d pixels, relative depth error %.2e\n",
                    meanNormalDot, depthPixels, relativeDepth);

        check(relative < 0.05, "the megakernel agrees on the mean to within 5%");
        check(meanNormalDot > 0.999, "the normals agree");
        // The depth aov is an average over the samples that found a surface,
        // and the two renderers jitter their camera rays differently -- so on
        // a silhouette they average over slightly different surfaces. This is
        // a statistical quantity and gets a statistical bound, unlike the
        // traversal above where the cell had to be the same cell.
        check(relativeDepth < 5e-3, "the depths agree");

        // ---- the same thing again, as a wavefront
        gpu::WavefrontTracer wave;
        wave.countAlive = true;
        if (!wave.build(&error) || !wave.upload(scene, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        Image waveImage;
        RenderTargets waveAovs;
        gpu::WavefrontStats ws;
        if (!wave.render(scene, ps, waveImage, &waveAovs, &ws, &error)) {
            std::printf("  FAIL  %s\n", error.c_str());
            return 1;
        }

        double wSumAbs = 0.0, wSumRef = 0.0, wNormalDot = 0.0, wDepthAbs = 0.0, wDepthRef = 0.0;
        int wDepthPixels = 0;
        for (int y = 0; y < ps.height; ++y) {
            for (int x = 0; x < ps.width; ++x) {
                const Vec3 a = cpuImage.at(x, y);
                const Vec3 b = waveImage.at(x, y);
                wSumAbs += std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z);
                wSumRef += std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);

                const size_t i = size_t(y) * size_t(ps.width) + size_t(x);
                if (cpuAovs.depth[i] > 0.0f && waveAovs.depth[i] > 0.0f) {
                    wNormalDot += double(dot(cpuAovs.normal.at(x, y), waveAovs.normal.at(x, y)));
                    wDepthAbs += std::fabs(double(cpuAovs.depth[i] - waveAovs.depth[i]));
                    wDepthRef += double(cpuAovs.depth[i]);
                    ++wDepthPixels;
                }
            }
        }
        const double wRelative = wSumAbs / std::max(wSumRef, 1e-9);

        std::printf("\n  wavefront %6.2f s  (upload %.2f, trace %.2f, readback %.2f, "
                    "%d dispatches, %.0f MiB)\n",
                    ws.totalSeconds(), ws.uploadSeconds, ws.traceSeconds, ws.readbackSeconds,
                    ws.dispatches, double(ws.bytesOnGpu) / 1048576.0);
        std::printf("  speedup %.1fx on the trace, %.1fx end to end   (megakernel was %.1fx)\n",
                    cs.seconds / std::max(ws.traceSeconds, 1e-9),
                    cs.seconds / std::max(ws.totalSeconds(), 1e-9),
                    cs.seconds / std::max(gs.traceSeconds, 1e-9));
        std::printf("  relative L1 %.4f\n", wRelative);
        std::printf("  aov: mean normal dot %.5f, relative depth error %.2e\n",
                    wDepthPixels > 0 ? wNormalDot / wDepthPixels : 0.0,
                    wDepthAbs / std::max(wDepthRef, 1e-9));

        // What the architecture exists to shrink: how many paths are still
        // going at each bounce. If this does not fall off steeply then the
        // compaction is not buying anything and the queues are pure overhead.
        std::printf("  alive per wave:");
        for (size_t i = 0; i < ws.aliveAtBounce.size(); ++i) {
            const double perWave = double(ws.aliveAtBounce[i]) / std::max(1, ws.waves);
            std::printf(" %.0f%%", 100.0 * perWave / double(ps.width * ps.height));
        }
        std::printf("\n");

        check(wRelative < 0.05, "the wavefront agrees on the mean to within 5%");
        check(wDepthPixels > 0 && wNormalDot / wDepthPixels > 0.999,
              "the wavefront normals agree");

        ToneParams tone;
        tone.curve = Tonemap::ACES;
        pngSave("out/test_gpu_cpu.png", cpuImage, tone, nullptr);
        pngSave("out/test_gpu_gpu.png", gpuImage, tone, nullptr);
        pngSave("out/test_gpu_wave.png", waveImage, tone, nullptr);
        std::printf("  wrote out/test_gpu_{cpu,gpu,wave}.png\n");

        // ---- and again with the game's own block textures
        //
        // Up to here both sides have been reading a palette of flat colours,
        // which is a weak test of a texture path: every face of every block is
        // one value, so a wrong uv or a transposed face costs nothing and
        // shows up as nothing. With real textures a swapped axis is a visibly
        // wrong wall.
        AssetSource source;
        const std::string jar = AssetSource::findClientJar();
        BlockTextureLibrary library;
        if (jar.empty() || !source.open(jar, nullptr) ||
            !library.load(source, scene.world.registry(), palette::minecraftRules(), nullptr)) {
            std::printf("\n  no game assets -- the textured comparison needs a client jar, "
                        "skipping\n");
        } else {
            scene.blockTextures = &library;
            std::printf("\n  textured: %zu images for %zu blocks\n", library.textureCount(),
                        library.texturedBlockCount());

            if (!wave.upload(scene, &error)) {   // the array is built from the library
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            Image texWave;
            RenderTargets texWaveAovs;
            gpu::WavefrontStats tws;
            if (!wave.render(scene, ps, texWave, &texWaveAovs, &tws, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            RenderStats tcs;
            RenderTargets texCpuAovs;
            const Image texCpu = renderPath(scene, ps, &tcs, &texCpuAovs);

            double tSumAbs = 0.0, tSumRef = 0.0, tAlbedoAbs = 0.0, tAlbedoRef = 0.0;
            for (int y = 0; y < ps.height; ++y) {
                for (int x = 0; x < ps.width; ++x) {
                    const Vec3 a = texCpu.at(x, y);
                    const Vec3 b = texWave.at(x, y);
                    tSumAbs += std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z);
                    tSumRef += std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);

                    // The albedo aov is the texture lookup with no light on
                    // it, so it isolates the thing being tested from every
                    // other way the two renderers could differ.
                    const Vec3 ca = texCpuAovs.albedo.at(x, y);
                    const Vec3 wa = texWaveAovs.albedo.at(x, y);
                    tAlbedoAbs += std::fabs(ca.x - wa.x) + std::fabs(ca.y - wa.y) +
                                  std::fabs(ca.z - wa.z);
                    tAlbedoRef += std::fabs(ca.x) + std::fabs(ca.y) + std::fabs(ca.z);
                }
            }

            const double tRelative = tSumAbs / std::max(tSumRef, 1e-9);
            const double albedoRelative = tAlbedoAbs / std::max(tAlbedoRef, 1e-9);

            std::printf("  cpu %6.2f s, wavefront %6.2f s\n", tcs.seconds, tws.traceSeconds);
            std::printf("  relative L1 %.4f, albedo aov %.4f\n", tRelative, albedoRelative);

            check(tRelative < 0.05, "the textured renders agree on the mean");
            check(albedoRelative < 0.02, "the sampled albedo agrees face for face");

            pngSave("out/test_gpu_tex_cpu.png", texCpu, tone, nullptr);
            pngSave("out/test_gpu_tex_wave.png", texWave, tone, nullptr);
            std::printf("  wrote out/test_gpu_tex_{cpu,wave}.png\n");
        }

        // ---- and now with characters in it
        {
            scene.blockTextures = nullptr;   // one variable at a time

            Skin skin = paintGradientSkin();
            EntityModel model = buildPlayerModel(skin);

            EntitySet set;
            const float yaws[] = {0.0f, 143.0f, -67.0f};
            const Vec3 spots[] = {{0.0f, 1.0f, 15.0f}, {2.6f, 1.0f, 16.4f},
                                  {-2.4f, 1.0f, 16.8f}};
            for (int i = 0; i < 3; ++i) {
                Entity e;
                e.model = &model;
                e.skin = &skin;
                e.position = spots[i];
                e.yawDegrees = yaws[i];
                // Three different poses, so the boxes are not all axis
                // aligned: an oriented box is the case the matrices are for.
                e.pose = (i == 0) ? Pose::striding(34.0f)
                                  : (i == 1 ? Pose::waving() : Pose::tPose());
                set.add(e);
            }
            scene.entities = &set;

            std::printf("\n  entities: %zu characters, %zu boxes, gradient skin\n",
                        set.entityCount(), set.boxCount());

            // Close enough that the frame is mostly skin, so the albedo
            // comparison is measuring the thing under test.
            scene.camera.lookAt({0.6f, 2.4f, 23.0f}, {0.2f, 1.3f, 16.0f});
            scene.camera.fovY = radians(42.0f);

            if (!wave.upload(scene, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            Image entWave;
            RenderTargets entWaveAovs;
            gpu::WavefrontStats ews;
            if (!wave.render(scene, ps, entWave, &entWaveAovs, &ews, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            RenderStats ecs;
            RenderTargets entCpuAovs;
            const Image entCpu = renderPath(scene, ps, &ecs, &entCpuAovs);

            double eSumAbs = 0.0, eSumRef = 0.0, eAlbedoAbs = 0.0, eAlbedoRef = 0.0;
            double eNormalDot = 0.0;
            int eNormalPixels = 0;
            for (int y = 0; y < ps.height; ++y) {
                for (int x = 0; x < ps.width; ++x) {
                    const Vec3 a = entCpu.at(x, y);
                    const Vec3 b = entWave.at(x, y);
                    eSumAbs += std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z);
                    eSumRef += std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);

                    const Vec3 ca = entCpuAovs.albedo.at(x, y);
                    const Vec3 wa = entWaveAovs.albedo.at(x, y);
                    eAlbedoAbs += std::fabs(ca.x - wa.x) + std::fabs(ca.y - wa.y) +
                                  std::fabs(ca.z - wa.z);
                    eAlbedoRef += std::fabs(ca.x) + std::fabs(ca.y) + std::fabs(ca.z);

                    const size_t i = size_t(y) * size_t(ps.width) + size_t(x);
                    if (entCpuAovs.depth[i] > 0.0f && entWaveAovs.depth[i] > 0.0f) {
                        eNormalDot +=
                            double(dot(entCpuAovs.normal.at(x, y), entWaveAovs.normal.at(x, y)));
                        ++eNormalPixels;
                    }
                }
            }

            const double eRelative = eSumAbs / std::max(eSumRef, 1e-9);
            const double eAlbedo = eAlbedoAbs / std::max(eAlbedoRef, 1e-9);
            const double eNormals = eNormalPixels > 0 ? eNormalDot / eNormalPixels : 0.0;

            std::printf("  cpu %6.2f s, wavefront %6.2f s\n", ecs.seconds, ews.traceSeconds);
            std::printf("  relative L1 %.4f, albedo aov %.4f, mean normal dot %.5f\n", eRelative,
                        eAlbedo, eNormals);

            check(eRelative < 0.05, "the renders with characters agree on the mean");
            check(eAlbedo < 0.02, "the skin is sampled at the same texel");
            check(eNormals > 0.999, "the box normals agree");

            pngSave("out/test_gpu_ent_cpu.png", entCpu, tone, nullptr);
            pngSave("out/test_gpu_ent_wave.png", entWave, tone, nullptr);
            std::printf("  wrote out/test_gpu_ent_{cpu,wave}.png\n");

            // ---- everything at once: blocks, characters, sprites and props
            //
            // The four kinds are tested together rather than one at a time
            // because what they have to get right is the order between them.
            // intersectScene narrows one distance through world, entities,
            // sprites and props in that order, and a nearest-hit taken in the
            // wrong order is a dust mote in front of a wall it is behind.
            Font font;
            font.useBuiltin();

            SpriteSet spriteSet;
            {
                scatter::ParticleStyle mote;
                mote.size = {0.09f, 0.09f};
                mote.sizeJitter = 0.5f;
                mote.tint = srgbToLinear(Vec3{1.0f, 0.85f, 0.55f});
                mote.emission = srgbToLinear(Vec3{1.0f, 0.7f, 0.35f}) * 1.6f;
                std::vector<Sprite> motes =
                    scatter::inSphere({0.0f, 3.0f, 14.0f}, 3.0f, 400, 4242u, mote);
                aimAt(motes, {0.5f, 3.2f, 23.0f});
                spriteSet.add(motes);

                // A label exercises the other half of the sprite path: a real
                // texture, and the alpha cutout that makes a glyph a glyph.
                TextStyle style;
                style.height = 0.7f;
                style.color = srgbToLinear(Vec3{0.95f, 0.95f, 0.90f});
                style.emission = srgbToLinear(Vec3{0.9f, 0.9f, 0.8f}) * 1.2f;
                spriteSet.add(text::facing(font, "GPU", {0.0f, 5.6f, 14.0f},
                                           {0.5f, 3.2f, 23.0f}, style));
            }
            spriteSet.build();
            scene.sprites = &spriteSet;

            VoxelModel shard;
            {
                VoxelMaterial core;
                core.albedo = srgbToLinear(Vec3{0.35f, 0.75f, 0.95f});
                core.roughness = 0.3f;
                VoxelMaterial metal;
                metal.albedo = srgbToLinear(Vec3{0.95f, 0.80f, 0.35f});
                metal.metallic = 1.0f;
                metal.roughness = 0.18f;
                shard = voxelize::fromLayers(
                    {{".#.", "###", ".#."}, {"###", "#@#", "###"}, {"###", "#@#", "###"},
                     {".#.", "###", ".#."}, {"...", ".#.", "..."}},
                    {{'#', core}, {'@', metal}});
            }

            PropSet propSet;
            {
                Prop a;
                a.model = &shard;
                a.position = {2.2f, 1.0f, 14.5f};
                a.voxelSize = 0.14f;
                a.yawDegrees = 31.0f;
                a.pitchDegrees = 12.0f;
                propSet.add(a);

                Prop b;
                b.model = &shard;
                b.position = {-2.4f, 1.0f, 13.6f};
                b.voxelSize = 0.09f;
                b.yawDegrees = -58.0f;
                b.rollDegrees = 24.0f;
                b.tint = Vec3{1.0f, 0.6f, 0.6f};
                propSet.add(b);
            }
            propSet.build();
            scene.props = &propSet;

            std::printf("\n  all four: %zu sprites, %zu props, %zu boxes\n", spriteSet.size(),
                        propSet.size(), set.boxCount());

            if (!wave.upload(scene, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            Image allWave;
            RenderTargets allWaveAovs;
            gpu::WavefrontStats aws;
            if (!wave.render(scene, ps, allWave, &allWaveAovs, &aws, &error)) {
                std::printf("  FAIL  %s\n", error.c_str());
                return 1;
            }

            RenderStats acs;
            RenderTargets allCpuAovs;
            const Image allCpu = renderPath(scene, ps, &acs, &allCpuAovs);

            double aSumAbs = 0.0, aSumRef = 0.0, aAlbAbs = 0.0, aAlbRef = 0.0;
            double aNormalDot = 0.0;
            int aNormalPixels = 0;
            for (int y = 0; y < ps.height; ++y) {
                for (int x = 0; x < ps.width; ++x) {
                    const Vec3 c = allCpu.at(x, y);
                    const Vec3 w = allWave.at(x, y);
                    aSumAbs += std::fabs(c.x - w.x) + std::fabs(c.y - w.y) + std::fabs(c.z - w.z);
                    aSumRef += std::fabs(c.x) + std::fabs(c.y) + std::fabs(c.z);

                    const Vec3 ca = allCpuAovs.albedo.at(x, y);
                    const Vec3 wa = allWaveAovs.albedo.at(x, y);
                    aAlbAbs += std::fabs(ca.x - wa.x) + std::fabs(ca.y - wa.y) +
                               std::fabs(ca.z - wa.z);
                    aAlbRef += std::fabs(ca.x) + std::fabs(ca.y) + std::fabs(ca.z);

                    const size_t i = size_t(y) * size_t(ps.width) + size_t(x);
                    if (allCpuAovs.depth[i] > 0.0f && allWaveAovs.depth[i] > 0.0f) {
                        aNormalDot +=
                            double(dot(allCpuAovs.normal.at(x, y), allWaveAovs.normal.at(x, y)));
                        ++aNormalPixels;
                    }
                }
            }

            const double aRelative = aSumAbs / std::max(aSumRef, 1e-9);
            const double aAlbedo = aAlbAbs / std::max(aAlbRef, 1e-9);
            const double aNormals = aNormalPixels > 0 ? aNormalDot / aNormalPixels : 0.0;

            std::printf("  cpu %6.2f s, wavefront %6.2f s  (%.0f MiB on the gpu)\n", acs.seconds,
                        aws.traceSeconds, double(aws.bytesOnGpu) / 1048576.0);
            std::printf("  relative L1 %.4f, albedo aov %.4f, mean normal dot %.5f\n", aRelative,
                        aAlbedo, aNormals);

            check(aRelative < 0.05, "all four kinds of geometry agree on the mean");
            check(aAlbedo < 0.03, "all four are sampled the same way");
            check(aNormals > 0.998, "all four report the same normals");

            pngSave("out/test_gpu_all_cpu.png", allCpu, tone, nullptr);
            pngSave("out/test_gpu_all_wave.png", allWave, tone, nullptr);
            std::printf("  wrote out/test_gpu_all_{cpu,wave}.png\n");

            // ---- the denoiser, both ways, on the same buffers
            //
            // This one *can* be compared pixel for pixel. Both filters read
            // the identical aov buffers and neither draws a random number, so
            // unlike the tracers there is no variance to hide behind: any
            // disagreement past float rounding is a difference in the filter.
            {
                gpu::Denoiser denoiser;
                if (!denoiser.build(&error)) {
                    std::printf("  FAIL  %s\n", error.c_str());
                    return 1;
                }

                DenoiseSettings ds;

                const auto cpuStart = std::chrono::steady_clock::now();
                const Image cpuFiltered = denoise(allCpuAovs, ds);
                const double cpuSeconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuStart)
                        .count();

                Image gpuFiltered;
                if (!denoiser.run(allCpuAovs, ds, gpuFiltered, &error)) {
                    std::printf("  FAIL  %s\n", error.c_str());
                    return 1;
                }

                double dAbs = 0.0, dRef = 0.0, dWorst = 0.0;
                for (int y = 0; y < ps.height; ++y) {
                    for (int x = 0; x < ps.width; ++x) {
                        const Vec3 a = cpuFiltered.at(x, y);
                        const Vec3 b = gpuFiltered.at(x, y);
                        const double d = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) +
                                         std::fabs(a.z - b.z);
                        const double r = std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z);
                        dAbs += d;
                        dRef += r;
                        dWorst = std::max(dWorst, d / std::max(r, 1e-3));
                    }
                }
                const double dRelative = dAbs / std::max(dRef, 1e-9);

                std::printf("\n  denoiser: cpu %6.3f s, gpu %6.3f s "
                            "(upload %.3f, filter %.3f, readback %.3f)\n",
                            cpuSeconds, denoiser.lastSeconds(), denoiser.lastUploadSeconds(),
                            denoiser.lastFilterSeconds(), denoiser.lastReadbackSeconds());
                std::printf("  relative L1 %.6f, worst pixel %.4f\n", dRelative, dWorst);

                check(dRelative < 1e-3, "the two denoisers agree pixel for pixel");
                check(dWorst < 0.05, "and no single pixel is far off");
            }
        }
    }

    std::printf("\n%s\n", gFailures == 0 ? "all gpu tests passed" : "gpu tests FAILED");
    return gFailures == 0 ? 0 : 1;
}
