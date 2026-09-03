#include "engine/render/trace/pathtrace.hpp"
#include "engine/core/random.hpp"
#include "engine/render/trace/bsdf.hpp"
#include "engine/render/trace/intersect.hpp"
#include "engine/render/trace/lights.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace blocky {
namespace {

// Offset for spawned rays, in blocks. Large enough to clear float error at
// the scales we work at, small enough to be invisible.
constexpr float kSurfaceBias = 1e-3f;

enum class Lobe { Diffuse, Coated, Glossy, Mirror, Dielectric };

// Normal-incidence reflectance of an ordinary dielectric. Plastics, paint and
// varnish all sit within a whisker of this, which is why the coat needs no
// index of refraction of its own.
constexpr float kCoatF0 = 0.04f;

// Everything the shading code needs about a surface, assembled once so the
// NEE helpers and the BSDF sampler cannot disagree about it.
struct Shading {
    Lobe  lobe = Lobe::Diffuse;
    Vec3  albedo{};
    float alpha = 1.0f;
    float coat = 0.0f;
    float coatAlpha = 0.01f;
};

Shading shadingFor(const SceneHit& hit) {
    Shading s;
    s.albedo = hit.albedo;
    s.alpha = bsdf::roughnessToAlpha(hit.roughness);
    s.coat = hit.coat;
    s.coatAlpha = bsdf::roughnessToAlpha(hit.coatRoughness);

    if (hit.transmissive) s.lobe = Lobe::Dielectric;
    else if (hit.metallic > 0.5f) s.lobe = hit.roughness < 0.06f ? Lobe::Mirror : Lobe::Glossy;
    else if (hit.coat > 0.0f) s.lobe = Lobe::Coated;
    else s.lobe = Lobe::Diffuse;
    return s;
}

Vec3 expNegative(Vec3 v) {
    return {std::exp(-v.x), std::exp(-v.y), std::exp(-v.z)};
}

// Cap a single contribution so one lucky sample cannot dominate the average.
Vec3 clampFirefly(Vec3 c, float limit) {
    if (limit <= 0.0f) return c;
    float m = maxComponent(c);
    return m > limit ? c * (limit / m) : c;
}

// f * cos(theta_i), in the local frame. Only the lobes that support explicit
// light sampling return anything.
Vec3 evalTimesCos(const Shading& s, Vec3 woLocal, Vec3 wiLocal) {
    switch (s.lobe) {
        case Lobe::Diffuse:
            return s.albedo * (kInvPi * wiLocal.z);
        case Lobe::Coated: {
            // A diffuse base under a white coat. The coat takes its Fresnel
            // share of the arriving light and the base is given what is left,
            // so a grazing surface goes shiny and pale instead of merely
            // gaining a highlight on top of undiminished colour.
            float fresnel = bsdf::fresnelSchlick(wiLocal.z, Vec3{kCoatF0}).x * s.coat;
            Vec3 diffuse = s.albedo * (kInvPi * wiLocal.z * (1.0f - fresnel));
            Vec3 specular =
                bsdf::ggxEvalTimesCos(woLocal, wiLocal, Vec3{kCoatF0}, s.coatAlpha) * s.coat;
            return diffuse + specular;
        }
        case Lobe::Glossy:
            return bsdf::ggxEvalTimesCos(woLocal, wiLocal, s.albedo, s.alpha);
        default:
            return Vec3{0.0f};
    }
}

// The first surface a camera ray settles on -- skipping straight through
// glass and water, because the albedo of a refracting interface says nothing
// useful about what the pixel is looking at.
struct FirstHit {
    Vec3  albedo{1.0f, 1.0f, 1.0f};
    Vec3  normal{0.0f, 0.0f, 0.0f};
    float distance = -1.0f;
    bool  captured = false;
};

struct PathContext {
    const Scene& scene;
    const LightSet& lights;
    const PathSettings& settings;
    uint64_t rays = 0;
};

// Explicit sampling of the sun disc. The cone pdf and the radiance
// normalisation cancel exactly, leaving f * cos * irradiance.
Vec3 sampleSun(PathContext& ctx, const bsdf::Frame& frame, Vec3 woLocal, Vec3 shadingPoint,
               const Shading& s, Rng& rng) {
    const SunLight& sun = ctx.scene.sun;
    if (sun.intensity <= 0.0f) return Vec3{0.0f};

    float cosMax = std::cos(radians(std::max(sun.angularRadiusDegrees, 0.01f)));
    Vec3 wi = rng.uniformCone(sun.direction, cosMax);
    Vec3 wiLocal = frame.toLocal(wi);
    if (wiLocal.z <= 0.0f) return Vec3{0.0f};

    Vec3 f = evalTimesCos(s, woLocal, wiLocal);
    if (maxComponent(f) <= 0.0f) return Vec3{0.0f};

    ++ctx.rays;
    Vec3 transmittance = sceneTransmittance(ctx.scene, {shadingPoint, wi}, ctx.settings.maxDistance);
    if (maxComponent(transmittance) <= 0.0f) return Vec3{0.0f};

    return f * sun.radiance() * transmittance;
}

// Explicit sampling of emissive block faces.
Vec3 sampleAreaLights(PathContext& ctx, const bsdf::Frame& frame, Vec3 woLocal, Vec3 shadingPoint,
                      const Shading& s, Rng& rng) {
    if (ctx.lights.empty()) return Vec3{0.0f};

    Vec3 wi, radiance;
    float distance = 0.0f, pdf = 0.0f;
    if (!ctx.lights.sample(rng, shadingPoint, wi, distance, radiance, pdf)) return Vec3{0.0f};

    Vec3 wiLocal = frame.toLocal(wi);
    if (wiLocal.z <= 0.0f || pdf <= 0.0f) return Vec3{0.0f};

    Vec3 f = evalTimesCos(s, woLocal, wiLocal);
    if (maxComponent(f) <= 0.0f) return Vec3{0.0f};

    ++ctx.rays;
    // Stop just short of the light so the emitter itself is not an occluder.
    Vec3 transmittance =
        sceneTransmittance(ctx.scene, {shadingPoint, wi}, distance - 4.0f * kSurfaceBias);
    if (maxComponent(transmittance) <= 0.0f) return Vec3{0.0f};

    return f * radiance * transmittance / pdf;
}

Vec3 tracePath(PathContext& ctx, Ray ray, Rng& rng, FirstHit* first = nullptr) {
    const BlockRegistry& registry = ctx.scene.world.registry();
    const PathSettings& settings = ctx.settings;

    Vec3 throughput{1.0f};
    Vec3 result{0.0f};

    // True while emission found by a BSDF ray has not already been accounted
    // for by next-event estimation: camera rays and purely specular bounces.
    bool countEmission = true;

    // Which block type the ray is currently travelling inside, if any.
    BlockId medium = block::Air;

    // Total path length so far, so the recorded depth is the real distance to
    // the surface rather than the length of the last segment.
    float travelled = 0.0f;

    for (int bounce = 0;; ++bounce) {
        RayFilter filter;
        filter.passThrough = medium;

        SceneHit hit;
        ++ctx.rays;
        if (!intersectScene(ctx.scene, ray, settings.maxDistance, filter, hit)) {
            Vec3 background = bounce == 0 ? ctx.scene.missColor(ray.direction)
                                          : ctx.scene.sky.sample(ray.direction);
            result += throughput * background;
            break;
        }

        travelled += hit.t;

        // Beer-Lambert attenuation over the segment just travelled.
        if (medium != block::Air) {
            throughput *= expNegative(registry[medium].absorption * hit.t);
        }

        if (countEmission && maxComponent(hit.emission) > 0.0f) {
            Vec3 contribution = throughput * hit.emission;
            result += bounce == 0 ? contribution
                                  : clampFirefly(contribution, settings.clampIndirect);
        }

        if (bounce >= settings.maxBounces) break;

        Vec3 normal = hit.normal;
        Vec3 wo = -ray.direction;

        // ------------------------------------------------ dielectric interface
        // Only voxel media refract; entity surfaces are always opaque.
        bool leavingMedium  = medium != block::Air && hit.isBlock && hit.blockId == block::Air;
        bool enteringMedium = medium == block::Air && hit.transmissive;

        if (leavingMedium || enteringMedium) {
            float mediumIor = leavingMedium ? registry[medium].ior : hit.ior;
            // Ratio of incident to transmitted index of refraction.
            float eta = leavingMedium ? mediumIor : 1.0f / mediumIor;

            float reflectance = bsdf::fresnelDielectric(saturate(dot(normal, wo)), eta);

            Vec3 nextDirection;
            if (rng.nextFloat() < reflectance) {
                nextDirection = reflect(ray.direction, normal);
            } else if (bsdf::refractRay(ray.direction, normal, eta, nextDirection)) {
                medium = leavingMedium ? block::Air : hit.blockId;
            } else {
                nextDirection = reflect(ray.direction, normal);  // total internal reflection
            }

            // Choosing the branch with probability equal to its Fresnel weight
            // makes the throughput factor exactly one.
            ray.origin = hit.position + nextDirection * kSurfaceBias;
            ray.direction = nextDirection;
            countEmission = true;
            continue;
        }

        // ------------------------------------------------------ opaque surface
        Vec3 albedo = hit.albedo;
        Shading s = shadingFor(hit);
        float alpha = s.alpha;

        bsdf::Frame frame(normal);
        Vec3 woLocal = frame.toLocal(wo);
        if (woLocal.z <= 0.0f) break;

        Vec3 shadingPoint = hit.position + normal * kSurfaceBias;

        if (first && !first->captured) {
            first->albedo = albedo;
            first->normal = normal;
            first->distance = travelled;
            first->captured = true;
        }

        // Next-event estimation, for the lobes where it pays off.
        if (s.lobe == Lobe::Diffuse || s.lobe == Lobe::Coated || s.lobe == Lobe::Glossy) {
            Vec3 nee = sampleSun(ctx, frame, woLocal, shadingPoint, s, rng) +
                       sampleAreaLights(ctx, frame, woLocal, shadingPoint, s, rng);
            Vec3 contribution = throughput * nee;
            result += bounce == 0 ? contribution
                                  : clampFirefly(contribution, settings.clampIndirect);
        }

        // Sample the BSDF for the next segment.
        Vec3 wiLocal;
        Vec3 weight;

        switch (s.lobe) {
            case Lobe::Diffuse: {
                float u1 = rng.nextFloat(), u2 = rng.nextFloat();
                float r = std::sqrt(u1), phi = kTwoPi * u2;
                wiLocal = {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1))};
                // (albedo/pi * cos) / (cos/pi) collapses to albedo.
                weight = albedo;
                countEmission = false;
                break;
            }
            case Lobe::Coated: {
                // One of the two lobes per bounce, chosen at random and the
                // estimate divided by the probability of having chosen it.
                // The probability only has to be non-zero wherever the lobe
                // contributes, so a fixed split is enough and avoids having
                // to evaluate Fresnel before knowing the direction.
                float pSpecular = saturate(0.18f + 0.42f * s.coat);

                if (rng.nextFloat() < pSpecular) {
                    Vec3 h = bsdf::sampleGgxVndf(woLocal, s.coatAlpha, rng.nextFloat(),
                                                 rng.nextFloat());
                    wiLocal = h * (2.0f * dot(woLocal, h)) - woLocal;
                    if (wiLocal.z <= 0.0f) return result;
                    weight = bsdf::fresnelSchlick(saturate(dot(wiLocal, h)), Vec3{kCoatF0}) *
                             (bsdf::smithG1(wiLocal, s.coatAlpha) * s.coat / pSpecular);
                } else {
                    float u1 = rng.nextFloat(), u2 = rng.nextFloat();
                    float r = std::sqrt(u1), phi = kTwoPi * u2;
                    wiLocal = {r * std::cos(phi), r * std::sin(phi),
                               std::sqrt(std::max(0.0f, 1.0f - u1))};
                    float fresnel = bsdf::fresnelSchlick(wiLocal.z, Vec3{kCoatF0}).x * s.coat;
                    weight = albedo * ((1.0f - fresnel) / (1.0f - pSpecular));
                }
                countEmission = false;
                break;
            }
            case Lobe::Glossy: {
                Vec3 h = bsdf::sampleGgxVndf(woLocal, alpha, rng.nextFloat(), rng.nextFloat());
                wiLocal = h * (2.0f * dot(woLocal, h)) - woLocal;
                if (wiLocal.z <= 0.0f) return result;
                // With VNDF sampling the estimator reduces to F * G1(wi).
                weight = bsdf::fresnelSchlick(saturate(dot(wiLocal, h)), albedo) *
                         bsdf::smithG1(wiLocal, alpha);
                countEmission = false;
                break;
            }
            case Lobe::Mirror: {
                wiLocal = {-woLocal.x, -woLocal.y, woLocal.z};
                weight = bsdf::fresnelSchlick(woLocal.z, albedo);
                countEmission = true;
                break;
            }
            default:
                return result;
        }

        throughput *= weight;
        if (maxComponent(throughput) <= 0.0f) break;

        // Russian roulette: kill dim paths, and scale the survivors up so the
        // estimator stays unbiased.
        if (bounce >= settings.rouletteStartBounce) {
            float survival = std::max(0.05f, std::min(1.0f, maxComponent(throughput)));
            if (rng.nextFloat() > survival) break;
            throughput /= survival;
        }

        ray.origin = shadingPoint;
        ray.direction = frame.toWorld(wiLocal);
    }

    return result;
}

constexpr int kTileRows = 4;

struct SharedState {
    std::atomic<int> nextTile{0};
    std::atomic<int> doneTiles{0};
    std::atomic<uint64_t> totalRays{0};
    std::atomic<uint64_t> primaryRays{0};
};

void renderTiles(const Scene& scene, const LightSet& lights, const PathSettings& settings,
                 Image& image, RenderTargets* aovs, SharedState& shared, int tileCount,
                 bool reportProgress) {
    const float invWidth = 1.0f / float(settings.width);
    const float invHeight = 1.0f / float(settings.height);
    const float invSamples = 1.0f / float(settings.samplesPerPixel);

    PathContext ctx{scene, lights, settings, 0};
    uint64_t primary = 0;

    for (;;) {
        int tile = shared.nextTile.fetch_add(1, std::memory_order_relaxed);
        if (tile >= tileCount) break;

        int yBegin = tile * kTileRows;
        int yEnd = std::min(yBegin + kTileRows, settings.height);

        for (int y = yBegin; y < yEnd; ++y) {
            // Seeded per row, so the image does not depend on thread timing.
            Rng rng(settings.seed, uint64_t(y) + 1);

            for (int x = 0; x < settings.width; ++x) {
                Vec3 sum{0.0f};
                Vec3 albedoSum{0.0f};
                Vec3 normalSum{0.0f};
                float depthSum = 0.0f;
                int depthSamples = 0;

                for (int s = 0; s < settings.samplesPerPixel; ++s) {
                    float u = (float(x) + rng.nextFloat()) * invWidth;
                    float v = (float(y) + rng.nextFloat()) * invHeight;

                    // A point on the lens, uniform over the disc. Ignored
                    // unless the camera has an aperture.
                    Vec2 lens{0.0f, 0.0f};
                    if (scene.camera.aperture > 0.0f) {
                        float angle = rng.nextFloat() * kTwoPi;
                        float radius = std::sqrt(rng.nextFloat());
                        lens = {std::cos(angle) * radius, std::sin(angle) * radius};
                    }

                    ++primary;
                    FirstHit first;
                    sum += tracePath(ctx, scene.camera.generateRay(u, v, lens), rng, &first);

                    if (first.captured) {
                        albedoSum += first.albedo;
                        normalSum += first.normal;
                        depthSum += first.distance;
                        ++depthSamples;
                    }
                }
                image.at(x, y) = sum * invSamples;

                if (aovs) {
                    float weight = depthSamples > 0 ? 1.0f / float(depthSamples) : 0.0f;
                    aovs->albedo.at(x, y) = depthSamples > 0 ? albedoSum * weight : Vec3{1.0f};
                    aovs->normal.at(x, y) = depthSamples > 0 ? normalize(normalSum) : Vec3{0.0f};
                    aovs->depth[size_t(y) * size_t(settings.width) + size_t(x)] =
                        depthSamples > 0 ? depthSum * weight : -1.0f;
                }
            }
        }

        int done = shared.doneTiles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reportProgress) {
            std::printf("\r  %3d%%", int(100.0 * double(done) / double(tileCount)));
            std::fflush(stdout);
        }
    }

    shared.totalRays.fetch_add(ctx.rays, std::memory_order_relaxed);
    shared.primaryRays.fetch_add(primary, std::memory_order_relaxed);
}

} // namespace

Image renderPath(const Scene& scene, const PathSettings& settings, RenderStats* stats,
                 RenderTargets* aovs) {
    Image image(settings.width, settings.height);
    if (settings.width <= 0 || settings.height <= 0) return image;

    if (aovs) {
        aovs->width = settings.width;
        aovs->height = settings.height;
        aovs->albedo = Image(settings.width, settings.height, Vec3{1.0f});
        aovs->normal = Image(settings.width, settings.height);
        aovs->depth.assign(size_t(settings.width) * size_t(settings.height), -1.0f);
    }

    // Preparation, timed separately. Copying the scene deep-copies the world,
    // and building the light set walks every populated chunk -- both are
    // nothing next to a still, and both are paid again on every frame of a
    // take, so the number is worth having rather than assuming.
    const auto setupStart = std::chrono::steady_clock::now();

    Scene local = scene;
    local.camera.aspect = float(settings.width) / float(settings.height);

    LightSet lights;
    lights.build(local.world);

    const double setupSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStart).count();

    if (settings.progress && !lights.empty()) {
        std::printf("  %zu emissive faces, total power %.1f\n", lights.size(), double(lights.totalPower()));
    }

    int threadCount = settings.threads > 0 ? settings.threads : int(std::thread::hardware_concurrency());
    if (threadCount <= 0) threadCount = 1;

    int tileCount = (settings.height + kTileRows - 1) / kTileRows;
    threadCount = std::min(threadCount, std::max(1, tileCount));

    SharedState shared;
    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(size_t(threadCount) - 1);
    for (int i = 1; i < threadCount; ++i) {
        workers.emplace_back(renderTiles, std::cref(local), std::cref(lights), std::cref(settings),
                             std::ref(image), aovs, std::ref(shared), tileCount, false);
    }
    renderTiles(local, lights, settings, image, aovs, shared, tileCount, settings.progress);
    for (auto& worker : workers) worker.join();

    auto end = std::chrono::steady_clock::now();
    if (settings.progress) std::printf("\r      \r");

    if (stats) {
        stats->seconds = std::chrono::duration<double>(end - start).count();
        stats->setupSeconds = setupSeconds;
        stats->primaryRays = shared.primaryRays.load();
        stats->totalRays = shared.totalRays.load();
        stats->threadsUsed = threadCount;
        stats->emissiveFaces = lights.size();
    }
    if (aovs) aovs->color = image;
    return image;
}

} // namespace blocky
