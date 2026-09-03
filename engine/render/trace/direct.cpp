#include "engine/render/trace/direct.hpp"
#include "engine/core/random.hpp"
#include "engine/render/trace/bsdf.hpp"
#include "engine/render/trace/intersect.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace blocky {
namespace {

// Nudge spawned rays off the surface so they do not immediately re-hit the
// block they started on.
constexpr float kSurfaceBias = 1e-3f;

// How many transmissive layers a camera ray will pass through before giving
// up. Four is enough for a window, the water surface and the seabed.
constexpr int kMaxTransmissiveLayers = 4;

// --------------------------------------------------------------------- AO
// Minecraft smooth lighting: for each of the four corners of the face, look
// at the three blocks touching that corner on the outside of the face. Two
// adjacent occluders that meet at the corner fully darken it, which is what
// produces the recognisable creases in inside corners.
//
// Entities have no voxel neighbourhood, so this applies to block hits only.
struct FaceAo {
    float corner[2][2]{{1.0f, 1.0f}, {1.0f, 1.0f}};

    float sample(float u, float v) const {
        float a = lerp(corner[0][0], corner[1][0], u);
        float b = lerp(corner[0][1], corner[1][1], u);
        return lerp(a, b, v);
    }
};

FaceAo computeFaceAo(const World& world, IVec3 block, IVec3 normal, int axis) {
    // The two axes spanning the face.
    int t1 = (axis + 1) % 3;
    int t2 = (axis + 2) % 3;

    IVec3 front = block + normal;  // the air block just outside the face

    FaceAo ao;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            IVec3 du{}, dv{};
            du[t1] = i == 0 ? -1 : 1;
            dv[t2] = j == 0 ? -1 : 1;

            bool side1  = world.isOpaque(front + du);
            bool side2  = world.isOpaque(front + dv);
            bool corner = world.isOpaque(front + du + dv);

            // 0 = fully occluded corner, 3 = fully open.
            int level = (side1 && side2) ? 0 : 3 - (int(side1) + int(side2) + int(corner));
            ao.corner[i][j] = float(level) / 3.0f;
        }
    }
    return ao;
}

// ------------------------------------------------------------------ shading
// Fixed per-face brightness, the way the game itself shades an unlit world:
// no sun, no shadows, no ambient occlusion, just a constant per direction so
// that the three visible sides of a cube read apart. Ordering matches
// docs/conventions.md: -X, +X, -Y, +Y, -Z, +Z.
Vec3 shadeFlat(const SceneHit& hit) {
    const float kFaceLight[6] = {0.62f, 0.62f, 0.48f, 1.0f, 0.80f, 0.80f};

    // An entity, sprite or prop face is not axis-aligned, so index it by its
    // dominant axis instead -- the same value a block with that normal gets.
    Vec3 n = hit.normal;
    int face = 5;
    float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az)      face = n.x < 0.0f ? 0 : 1;
    else if (ay >= ax && ay >= az) face = n.y < 0.0f ? 2 : 3;
    else                           face = n.z < 0.0f ? 4 : 5;

    return hit.albedo * kFaceLight[face] + hit.emission;
}

Vec3 shadeOpaque(const Scene& scene, const SceneHit& hit, Vec3 wo, const DirectSettings& settings,
                 Rng& rng) {
    if (settings.flatLighting) return shadeFlat(hit);

    Vec3 normal = hit.normal;
    Vec3 shadingPoint = hit.position + normal * kSurfaceBias;

    // ---- direct sunlight
    Vec3 direct{0.0f};
    Vec3 specular{0.0f};
    float ndl = dot(normal, scene.sun.direction);
    if (ndl > 0.0f) {
        Vec3 toSun = scene.sun.direction;
        if (settings.softShadows && scene.sun.angularRadiusDegrees > 0.0f) {
            float cosMax = std::cos(radians(scene.sun.angularRadiusDegrees));
            toSun = rng.uniformCone(scene.sun.direction, cosMax);
        }
        // Transmittance rather than a binary occlusion test, so a point on the
        // seabed receives sunlight that has already been filtered by the water
        // above it.
        Vec3 transmittance = sceneTransmittance(scene, {shadingPoint, toSun}, settings.maxDistance);
        direct = scene.sun.radiance() * transmittance * (ndl * kInvPi);

        // The dielectric coat a material style may have added. Sun only: the
        // preview has no indirect light to reflect anyway, and the point of
        // showing it here is that a plastic scene is recognisable before the
        // path tracer has been asked for it.
        if (hit.coat > 0.0f) {
            bsdf::Frame frame(normal);
            Vec3 woLocal = frame.toLocal(wo);
            Vec3 wiLocal = frame.toLocal(toSun);
            if (woLocal.z > 0.0f && wiLocal.z > 0.0f) {
                specular = bsdf::ggxEvalTimesCos(woLocal, wiLocal, Vec3{0.04f},
                                                 bsdf::roughnessToAlpha(hit.coatRoughness)) *
                           hit.coat * scene.sun.radiance() * transmittance;
            }
        }
    }

    // ---- sky as ambient
    // Cosine-weighted sampling makes the estimator of irradiance/pi simply the
    // mean of the samples, so no extra factor is needed here.
    Vec3 skyDirection = rng.cosineHemisphere(normal);
    Vec3 ambient = scene.sky.sample(skyDirection) * scene.ambientStrength;

    if (settings.ambientOcclusion && hit.isBlock) {
        FaceAo ao = computeFaceAo(scene.world, hit.block, hit.blockNormal, hit.axis);

        // Face-local coordinates aligned with the AO corners, which is not the
        // same parameterisation as the texture uv.
        Vec3 local = hit.position - toVec3(hit.block);
        int t1 = (hit.axis + 1) % 3;
        int t2 = (hit.axis + 2) % 3;
        float occlusion = ao.sample(saturate(local[t1]), saturate(local[t2]));

        ambient *= lerp(1.0f, occlusion, saturate(settings.aoStrength));
    }

    return hit.albedo * (direct + ambient) + specular + hit.emission;
}

// One camera ray, passing straight through any transmissive blocks it meets.
//
// This is not refraction: the ray is not bent, which keeps the preview cheap
// and stable. What it does reproduce is the part that matters visually --
// colour deepening with distance travelled through the medium, and a Fresnel
// sheen of sky on the surface.
Vec3 traceCameraRay(const Scene& scene, Ray ray, const DirectSettings& settings, Rng& rng) {
    Vec3 result{0.0f};
    Vec3 transmittance{1.0f};

    SceneHit hit;
    if (!intersectScene(scene, ray, settings.maxDistance, RayFilter{}, hit)) {
        return scene.missColor(ray.direction);
    }

    for (int layer = 0; layer < kMaxTransmissiveLayers; ++layer) {
        // Air here means a medium ended without anything behind it -- pick the
        // trace back up from that point.
        if (hit.isBlock && hit.blockId == block::Air) {
            ray.origin = hit.position + ray.direction * kSurfaceBias;
            if (!intersectScene(scene, ray, settings.maxDistance, RayFilter{}, hit)) {
                return result + transmittance * scene.missColor(ray.direction);
            }
            continue;
        }

        if (!hit.transmissive) {
            return result + transmittance * shadeOpaque(scene, hit, -ray.direction, settings, rng);
        }

        Vec3 normal = hit.normal;

        // Sky reflected off the surface, weighted by Fresnel. At the grazing
        // angles an isometric camera produces this is what stops water from
        // looking like coloured glass laid flat.
        float reflectance = bsdf::fresnelDielectric(saturate(dot(normal, -ray.direction)), 1.0f / hit.ior);
        result += transmittance * reflectance * scene.sky.sample(reflect(ray.direction, normal));
        transmittance *= 1.0f - reflectance;

        // Walk to the far side of this body of medium. The hit that comes back
        // already describes whatever is behind it, with the right normal, so
        // it becomes the surface considered on the next turn of the loop.
        RayFilter through;
        through.passThrough = hit.blockId;
        Ray inside{hit.position + ray.direction * kSurfaceBias, ray.direction};

        Vec3 absorption = hit.absorption;

        SceneHit exit;
        if (!intersectScene(scene, inside, settings.maxDistance, through, exit)) {
            return result + transmittance * scene.sky.sample(ray.direction);
        }

        Vec3 a = absorption * exit.t;
        transmittance *= Vec3{std::exp(-a.x), std::exp(-a.y), std::exp(-a.z)};
        if (maxComponent(transmittance) < 1e-3f) return result;

        ray = inside;
        hit = exit;
    }
    return result;
}

// ---------------------------------------------------------------- tile loop
constexpr int kTileRows = 8;

void renderRows(const Scene& scene, const DirectSettings& settings, Image& image,
                std::atomic<int>& nextTile, int tileCount, std::atomic<uint64_t>& rayCount) {
    const float invWidth  = 1.0f / float(settings.width);
    const float invHeight = 1.0f / float(settings.height);
    const float invSamples = 1.0f / float(settings.samplesPerPixel);
    uint64_t localRays = 0;

    for (;;) {
        int tile = nextTile.fetch_add(1, std::memory_order_relaxed);
        if (tile >= tileCount) break;

        int yBegin = tile * kTileRows;
        int yEnd = std::min(yBegin + kTileRows, settings.height);

        for (int y = yBegin; y < yEnd; ++y) {
            // Seed per row so the image is identical regardless of how tiles
            // were handed out to threads.
            Rng rng(settings.seed, uint64_t(y) + 1);

            for (int x = 0; x < settings.width; ++x) {
                Vec3 sum{0.0f};
                for (int s = 0; s < settings.samplesPerPixel; ++s) {
                    // One sample lands on the pixel centre; the rest jitter.
                    float jx = settings.samplesPerPixel == 1 ? 0.5f : rng.nextFloat();
                    float jy = settings.samplesPerPixel == 1 ? 0.5f : rng.nextFloat();

                    float u = (float(x) + jx) * invWidth;
                    float v = (float(y) + jy) * invHeight;

                    ++localRays;
                    sum += traceCameraRay(scene, scene.camera.generateRay(u, v), settings, rng);
                }
                image.at(x, y) = sum * invSamples;
            }
        }
    }
    rayCount.fetch_add(localRays, std::memory_order_relaxed);
}

} // namespace

Image renderDirect(const Scene& scene, const DirectSettings& settings, RenderStats* stats) {
    Image image(settings.width, settings.height);
    if (settings.width <= 0 || settings.height <= 0) return image;

    // The scene owns the aspect ratio question; the camera must agree with the
    // resolution we were actually asked for.
    Scene local = scene;
    local.camera.aspect = float(settings.width) / float(settings.height);

    int threadCount = settings.threads > 0 ? settings.threads : int(std::thread::hardware_concurrency());
    if (threadCount <= 0) threadCount = 1;

    int tileCount = (settings.height + kTileRows - 1) / kTileRows;
    threadCount = std::min(threadCount, std::max(1, tileCount));

    std::atomic<int> nextTile{0};
    std::atomic<uint64_t> rayCount{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(size_t(threadCount) - 1);
    for (int i = 1; i < threadCount; ++i) {
        workers.emplace_back(renderRows, std::cref(local), std::cref(settings), std::ref(image),
                             std::ref(nextTile), tileCount, std::ref(rayCount));
    }
    renderRows(local, settings, image, nextTile, tileCount, rayCount);
    for (auto& worker : workers) worker.join();

    auto end = std::chrono::steady_clock::now();

    if (stats) {
        stats->seconds = std::chrono::duration<double>(end - start).count();
        stats->primaryRays = rayCount.load();
        stats->totalRays = rayCount.load();
        stats->threadsUsed = threadCount;
    }
    return image;
}

} // namespace blocky
